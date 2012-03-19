/* Insanity QA system

 Copyright (c) 2012, Collabora Ltd
 Author: Vincent Penquerc'h <vincent@collabora.co.uk>
 libsoup server setup taken from souphttpsrc.c from -good:
 * Copyright (C) 2006-2007 Tim-Philipp Müller <tim centricular net>
 * Copyright (C) 2008 Wouter Cloetens <wouter@mind.be>
 * Copyright (C) 2001-2003, Ximian, Inc.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this program; if not, write to the
 Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 Boston, MA 02111-1307, USA.
*/

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <libsoup/soup-address.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-misc.h>
#include <libsoup/soup-server.h>
#include <libsoup/soup-auth-domain.h>
#include <libsoup/soup-auth-domain-basic.h>
#include <libsoup/soup-auth-domain-digest.h>
#include <insanity-gst/insanity-gst.h>

/* Size of chunks to send, in bytes */
#define BUFLEN 4096

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (40 * GST_MSECOND)

static GstElement *global_pipeline = NULL;
static SoupServer *global_server = NULL;
static SoupServer *global_ssl_server = NULL;
static guint global_server_port = 0;
static guint global_ssl_server_port = 0;
static char *global_source_filename = NULL;
static const char *global_validate_on_playing = NULL;
static gboolean global_done_http = FALSE;

/* SSL setup */
static const char *good_user = "good_user";
static const char *bad_user = "bad_user";
static const char *good_pw = "good_pw";
static const char *bad_pw = "bad_pw";
static const char *realm = "SOUPHTTPSRC_REALM";
static const char *basic_auth_path = "/basic_auth";
static const char *digest_auth_path = "/digest_auth";

static GstPipeline*
http_test_create_pipeline (InsanityGstPipelineTest *ptest, gpointer userdata)
{
  GstElement *pipeline = NULL, *playbin2 = NULL;
  const char *launch_line = "playbin2 audio-sink=fakesink video-sink=fakesink";
  GError *error = NULL;

  pipeline = gst_parse_launch (launch_line, &error);
  if (!pipeline) {
    insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
      error ? error->message : NULL);
    if (error)
      g_error_free (error);
    return NULL;
  }
  else if (error) {
    /* Do we get a dangling pointer here ? gst-launch.c does not unref */
    pipeline = NULL;
    insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
      error->message);
    g_error_free (error);
    return NULL;
  }

  global_pipeline = pipeline;

  return GST_PIPELINE (pipeline);
}

static gboolean
do_seek (gpointer data)
{
  InsanityTest *test = data;
  GstEvent *event;
  gboolean res;

  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
      GST_SEEK_TYPE_SET, 15 * GST_SECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  global_validate_on_playing = "seek";
  res = gst_element_send_event (global_pipeline, event);
  if (!res) {
    global_validate_on_playing = NULL;
    insanity_test_validate_step (test, "seek", FALSE, "Failed to send seek event");
    return FALSE;
  }
  gst_element_get_state (global_pipeline, NULL, NULL, SEEK_TIMEOUT);

  return FALSE;
}

static gboolean
end_step (gpointer data)
{
  InsanityTest *test = data;
  char *httpsuri;

  /* If we have both a non SSL and a SSL server, test both */
  if (global_done_http || global_ssl_server == NULL) {
    insanity_test_done (test);
  }
  else {
    global_done_http = TRUE;
    gst_element_set_state (global_pipeline, GST_STATE_READY);
    httpsuri = g_strdup_printf ("https://127.0.0.1:%u/", global_ssl_server_port);
    g_object_set (global_pipeline, "uri", httpsuri, NULL);
    g_free (httpsuri);
    gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
    gst_element_get_state (global_pipeline, NULL, NULL, GST_SECOND * 2);

    g_timeout_add (4000, (GSourceFunc)&do_seek, test);
  }
  return FALSE;
}

static gboolean
http_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage *msg)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (global_pipeline)) {
        const char *validate_step = global_validate_on_playing;
        GstState oldstate, newstate, pending;

        gst_message_parse_state_changed (msg, &oldstate, &newstate, &pending);
        if (newstate == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING && validate_step) {
          global_validate_on_playing = NULL;
          insanity_test_validate_step (INSANITY_TEST (ptest), validate_step, TRUE, NULL);
          /* let it run a couple seconds */
          g_timeout_add (2000, (GSourceFunc)&end_step, INSANITY_TEST (ptest));
        }
      }
      break;
  }

  return TRUE;
}

typedef struct {
  SoupServer *server;
  GMappedFile *f;
  gsize size;
  const char *contents;
  const char *ptr;
} ChunkedTransmitter;

static void
free_chunked_transmitter(ChunkedTransmitter *ct)
{
  g_object_unref (ct->server);
  g_mapped_file_unref (ct->f);
  g_free (ct);
}

static void
http_message_finished (SoupMessage *msg, gpointer data)
{
  ChunkedTransmitter *ct = data;

  free_chunked_transmitter (ct);
}

static void
http_message_write_next_chunk (SoupMessage *msg, gpointer data)
{
  ChunkedTransmitter *ct = data;
  SoupServer *server = g_object_ref (ct->server);
  char *buf;
  gsize size;

  size = (ct->ptr + BUFLEN > ct->contents + ct->size) ? ct->contents + ct->size - ct->ptr : BUFLEN;

  buf = g_malloc (size);
  memcpy (buf, ct->ptr, size);
  soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE,
      buf, size);
  ct->ptr += size;

  if (ct->ptr == ct->contents + ct->size) {
    /* done, will be automatically stopped */
  }

  soup_server_unpause_message (server, msg);
  g_object_unref (server);
}

static void
do_get (InsanityGstPipelineTest *ptest, SoupServer *server, SoupMessage * msg, const char *path)
{
  char *uri;
  GMappedFile *f = NULL;
  gsize size;

  SoupKnownStatusCode status = SOUP_STATUS_OK;

  uri = soup_uri_to_string (soup_message_get_uri (msg), FALSE);
  insanity_test_printf (INSANITY_TEST (ptest), "request: \"%s\"\n", uri);

  /* Known URLs */
  if (!strcmp (path, "/404"))
    status = SOUP_STATUS_NOT_FOUND;

  if (status != SOUP_STATUS_OK)
    goto leave;

  /* We only allow the default path here. We only need one. */
  if (strcmp (path, "/")) {
    status = SOUP_STATUS_NOT_FOUND;
    goto leave;
  }

  f = g_mapped_file_new (global_source_filename, FALSE, NULL);
  if (!f) {
    status = SOUP_STATUS_NOT_FOUND;
    goto leave;
  }
  size = g_mapped_file_get_length (f);

  if (msg->method == SOUP_METHOD_GET) {
    char *buf, *length;
    const char *contents, *ptr;
    SoupRange *ranges = NULL;
    int nranges = 0;
    goffset start, end;

    if (size == 0) {
      contents = "";
    }
    else {
      contents = g_mapped_file_get_contents (f);
    }
    if (!contents) {
      status = SOUP_STATUS_NOT_FOUND;
      goto leave;
    }

    if (soup_message_headers_get_ranges (msg->request_headers, size, &ranges, &nranges)) {
      start = ranges[0].start;
      end = ranges[0].end;
      soup_message_headers_free_ranges (msg->request_headers, ranges);
      soup_message_headers_set_content_range (msg->response_headers, start, end, size);
      status = SOUP_STATUS_PARTIAL_CONTENT;
    }
    else {
      start = 0;
      end = size - 1;
    }
    soup_message_headers_set_content_length (msg->response_headers, size);

    /* We'll send in chunks */
    ChunkedTransmitter *ct = g_malloc (sizeof (ChunkedTransmitter));
    ct->server = g_object_ref (server);
    ct->f = g_mapped_file_ref(f);
    ct->contents = contents;
    ct->ptr = contents + start;
    ct->size = end + 1;

    soup_message_headers_set_encoding (msg->response_headers, SOUP_ENCODING_CONTENT_LENGTH);
    g_signal_connect (msg, "finished", G_CALLBACK (http_message_finished), ct);
    g_signal_connect (msg, "wrote-chunk", G_CALLBACK (http_message_write_next_chunk), ct);
    g_signal_connect (msg, "wrote-headers", G_CALLBACK (http_message_write_next_chunk), ct);
  } else {                      /* msg->method == SOUP_METHOD_HEAD */

    char *length;

    /* We could just use the same code for both GET and
     * HEAD. But we'll optimize and avoid the extra
     * malloc.
     */

    length = g_strdup_printf ("%lu", (gulong) size);
    soup_message_headers_append (msg->response_headers,
        "Content-Length", length);
    g_free (length);
  }

leave:
  soup_message_set_status (msg, status);
  g_free (uri);
  if (f)
    g_mapped_file_unref(f);
}

static void
print_header (const char *name, const char *value, gpointer data)
{
  InsanityTest *test = data;
  insanity_test_printf(test, "header: %s: %s\n", name, value);
}

static void
server_callback (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query,
    SoupClientContext * context, gpointer data)
{
  InsanityGstPipelineTest *ptest = data;
  InsanityTest *test = data;

  insanity_test_printf (test, "%s %s HTTP/1.%d\n",
      msg->method, path, soup_message_get_http_version (msg));
  soup_message_headers_foreach (msg->request_headers, print_header, test);
  if (msg->request_body->length)
    insanity_test_printf (test, "%s\n", msg->request_body->data);

  if (msg->method == SOUP_METHOD_GET || msg->method == SOUP_METHOD_HEAD)
    do_get (ptest, server, msg, path);
  else
    soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);

  insanity_test_printf (test, "  -> %d %s\n", msg->status_code, msg->reason_phrase);
}

static gboolean
basic_auth_cb (SoupAuthDomain * domain, SoupMessage * msg,
    const char *username, const char *password, gpointer user_data)
{
  /* There is only one good login for testing */
  return (strcmp (username, good_user) == 0)
      && (strcmp (password, good_pw) == 0);
}

static char *
digest_auth_cb (SoupAuthDomain * domain, SoupMessage * msg,
    const char *username, gpointer user_data)
{
  /* There is only one good login for testing */
  if (strcmp (username, good_user) == 0)
    return soup_auth_domain_digest_encode_password (good_user, realm, good_pw);
  return NULL;
}

static SoupAddress *
find_link_local_address (InsanityTest *test)
{
  SoupAddress *sa = NULL;

#ifdef HAVE_IFADDRS_H
  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs (&ifaddr) < 0) {
    insanity_test_printf (test, "Failed to fetch interface addresses, will bind on ANY");
    return NULL;
  }

  ifa = ifaddr;
  while (ifa) {
    if (ifa->ifa_addr->sa_family == AF_INET) {
      if (ifa->ifa_name && !strcmp (ifa->ifa_name, "lo")) {
        sa = soup_address_new_from_sockaddr (ifa->ifa_addr, sizeof (struct sockaddr_in));
        break;
      }
    }
    ifa = ifa->ifa_next;
  }

  freeifaddrs (ifaddr);
#endif

  return sa;
}

int
start_server (InsanityTest *test, const char *ssl_cert_file, const char *ssl_key_file)
{
  SoupServer *server = NULL;
  SoupServer *ssl_server = NULL;
  SoupAddress *bind_address;
  guint port = SOUP_ADDRESS_ANY_PORT;
  guint ssl_port = SOUP_ADDRESS_ANY_PORT;
  SoupAuthDomain *domain = NULL;

  global_server_port = 0;
  global_ssl_server_port = 0;

  bind_address = find_link_local_address (test);

  server = soup_server_new (SOUP_SERVER_PORT, port, SOUP_SERVER_INTERFACE, bind_address, NULL);
  if (!server) {
    char *message = g_strdup_printf ("Unable to bind to server port %u", port);
    insanity_test_validate_step (test, "server-started", FALSE, message);
    g_free (message);
    return 1;
  }
  global_server_port = soup_server_get_port (server);
  insanity_test_printf (test, "HTTP server listening on port %u\n", global_server_port);
  soup_server_add_handler (server, NULL, server_callback, test, NULL);

  global_server = server;

  if (ssl_cert_file && ssl_key_file) {
    ssl_server = soup_server_new (SOUP_SERVER_PORT, ssl_port,
        SOUP_SERVER_INTERFACE, bind_address,
        SOUP_SERVER_SSL_CERT_FILE, ssl_cert_file,
        SOUP_SERVER_SSL_KEY_FILE, ssl_key_file, NULL);

    if (!ssl_server) {
      char *message = g_strdup_printf ("Unable to bind to SSL server port %u", ssl_port);
      insanity_test_validate_step (test, "ssl-server-started", FALSE, message);
      g_free (message);
      g_object_unref (global_server);
      global_server = NULL;
      return 1;
    }
    global_ssl_server_port = soup_server_get_port (ssl_server);
    insanity_test_printf (test, "HTTPS server listening on port %u\n", global_ssl_server_port);
    soup_server_add_handler (ssl_server, NULL, server_callback, test, NULL);

    global_ssl_server = ssl_server;
  }

  domain = soup_auth_domain_basic_new (SOUP_AUTH_DOMAIN_REALM, realm,
      SOUP_AUTH_DOMAIN_BASIC_AUTH_CALLBACK, basic_auth_cb,
      SOUP_AUTH_DOMAIN_ADD_PATH, basic_auth_path, NULL);
  soup_server_add_auth_domain (server, domain);
  if (ssl_server)
    soup_server_add_auth_domain (ssl_server, domain);
  g_object_unref (domain);
  domain = soup_auth_domain_digest_new (SOUP_AUTH_DOMAIN_REALM, realm,
      SOUP_AUTH_DOMAIN_DIGEST_AUTH_CALLBACK, digest_auth_cb,
      SOUP_AUTH_DOMAIN_ADD_PATH, digest_auth_path, NULL);
  soup_server_add_auth_domain (server, domain);
  if (ssl_server)
    soup_server_add_auth_domain (ssl_server, domain);
  g_object_unref (domain);

  if (bind_address)
    g_object_unref (bind_address);

  soup_server_run_async (server);
  insanity_test_validate_step (test, "server-started", TRUE, NULL);

  if (ssl_server) {
    soup_server_run_async (ssl_server);
    insanity_test_validate_step (test, "server-started", TRUE, NULL);
  }

  return 0;
}

static void
stop_server (void)
{
  if (global_server) {
    g_object_unref (global_server);
    global_server = NULL;
  }
  if (global_ssl_server) {
    g_object_unref (global_ssl_server);
    global_ssl_server = NULL;
  }
}

static gboolean
http_test_setup(InsanityTest *test)
{
  char *ssl_cert_file = NULL;
  char *ssl_key_file = NULL;
  GValue v = {0};
  gboolean started;

  if (insanity_test_get_argument (test, "ssl-cert-file", &v)) {
    ssl_cert_file = g_value_dup_string (&v);
    g_value_unset (&v);
  }
  if (insanity_test_get_argument (test, "ssl-key-file", &v)) {
    ssl_key_file = g_value_dup_string (&v);
    g_value_unset (&v);
  }

  started = start_server (test, ssl_cert_file, ssl_key_file) == 0;

  g_free (ssl_key_file);
  g_free (ssl_cert_file);

  return started;
}

static void
http_test_teardown (InsanityTest *test)
{
  stop_server ();
  g_free (global_source_filename);
  global_source_filename = NULL;
}

static gboolean
http_test_start(InsanityTest *test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  GValue uri = {0};
  const char *protocol;
  char *httpuri;

  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (g_value_get_string (&uri), "")) {
    insanity_test_validate_step (test, "valid-pipeline", FALSE, "No URI to test on");
    g_value_unset (&uri);
    return FALSE;
  }

  if (!gst_uri_is_valid (g_value_get_string (&uri))) {
    insanity_test_validate_step (test, "uri-is-file", FALSE, NULL);
    g_value_unset (&uri);
    return FALSE;
  }
  protocol = gst_uri_get_protocol (g_value_get_string (&uri));
  if (!protocol || g_ascii_strcasecmp (protocol, "file")) {
    insanity_test_validate_step (test, "uri-is-file", FALSE, NULL);
    g_value_unset (&uri);
    return FALSE;
  }
  insanity_test_validate_step (test, "uri-is-file", TRUE, NULL);
  global_source_filename = gst_uri_get_location (g_value_get_string (&uri));
  g_value_unset (&uri);

  if (global_ssl_server) {
    httpuri = g_strdup_printf ("https://127.0.0.1:%u/", global_ssl_server_port);
  }
  else {
    httpuri = g_strdup_printf ("http://127.0.0.1:%u/", global_server_port);
  }
  g_object_set (global_pipeline, "uri", httpuri, NULL);
  g_free (httpuri);

  global_validate_on_playing = NULL;
  global_done_http = FALSE;

  return TRUE;
}

static void
http_test_pipeline_test (InsanityGstPipelineTest *ptest)
{
  GstElement *source = NULL;

  g_object_get (global_pipeline, "source", &source, NULL);
  g_object_set (source, "user-id", good_user, "user-pw", good_pw, NULL);
  gst_object_unref (source);

  /* So we're in PLAYING, let's wait a few seconds and seek */
  g_timeout_add (4000, (GSourceFunc)&do_seek, ptest);
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  GValue vdef = {0};

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("http-test", "Tests HTTP streaming", NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, "");
  insanity_test_add_argument (test, "uri", "The uri to test on (file only)", NULL, FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, NULL);
  insanity_test_add_argument (test, "ssl-cert-file", "Certificate file for SSL server", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, NULL);
  insanity_test_add_argument (test, "ssl-key-file", "Key file for SSL server", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "uri-is-file", "The URI is a file URI", NULL);
  insanity_test_add_checklist_item (test, "server-started", "The internal HTTP server was started", NULL);
  insanity_test_add_checklist_item (test, "ssl-server-started", "The SSL internal HTTP server was started", NULL);
  insanity_test_add_checklist_item (test, "seek", "A seek succeeded", NULL);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &http_test_create_pipeline, NULL, NULL);
  g_signal_connect_after (test, "setup", G_CALLBACK (&http_test_setup), 0);
  g_signal_connect_after (test, "bus-message", G_CALLBACK (&http_test_bus_message), 0);
  g_signal_connect_after (test, "start", G_CALLBACK (&http_test_start), 0);
  g_signal_connect_after (test, "pipeline-test", G_CALLBACK (&http_test_pipeline_test), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&http_test_teardown), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
