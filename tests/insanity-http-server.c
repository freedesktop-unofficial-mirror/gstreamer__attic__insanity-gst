/**
 * Insanity QA system
 *
 * Copyright (c) 2012, Collabora Ltd
 *    Author: Thibault Saunier <thibault.saunier@collabora.com>
 * libsoup server setup taken from souphttpsrc.c from -good:
 *    Copyright (C) 2006-2007 Tim-Philipp Müller <tim centricular net>
 *    Copyright (C) 2008 Wouter Cloetens <wouter@mind.be>
 *    Copyright (C) 2001-2003, Ximian, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:insanityhttpserver
 * @short_description: Helper to create an HTTP server
 *
 * %TODO.
 */

#include "insanity-http-server.h"

#include <glib.h>
#include <string.h>
#include <gst/gstmarshal.h>
#include <libsoup/soup-address.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-misc.h>
#include <libsoup/soup-auth-domain.h>
#include <libsoup/soup-auth-domain-basic.h>
#include <libsoup/soup-auth-domain-digest.h>

#define DEFAULT_CHUNKS_SIZE 4096
#define LOG(format, args...) \
  INSANITY_LOG (test, "httpserver", INSANITY_LOG_LEVEL_DEBUG, format, ##args)

enum
{
  PROP_0,
  PROP_TEST,
  PROP_CHUNKS_SIZE,
  N_PROPERTIES
};

enum
{
  SIGNAL_0,
  SIGNAL_GET_CONTENT,
  SIGNAL_WRITING_CHUNK,
  SIGNAL_WRITING_DONE,
  SIGNAL_LAST
};

const char *good_user = "good_user";
const char *bad_user = "bad_user";
const char *good_pw = "good_pw";
const char *bad_pw = "bad_pw";
const char *realm = "SOUPHTTPSRC_REALM";
const char *basic_auth_path = "/basic_auth";
const char *digest_auth_path = "/digest_auth";

static GParamSpec *properties[N_PROPERTIES] = { NULL, };
static guint signals[SIGNAL_LAST] = { 0, };

struct _InsanityHttpServerPrivate
{
  /* API properties */
  InsanityTest *test;
  gsize chunks_size;

  gchar *ssl_cert_file;
  gchar *ssl_key_file;

  GThread *thread;              /* Thread running the server */
  GMainContext *mcontext;       /* Main context for the server */
  GMainLoop *mloop;             /* Main loop for the server */
  gboolean running;             /* Wether the server is currently running */
  gboolean ready;               /* Server is ready to be run (ressources allocated ) */

  GSimpleAsyncResult *async_res;

#ifdef USE_NEW_GLIB_MUTEX_API
  GMutex lock;
  GCond cond;
#else
  GMutex *lock;
  GCond *cond;
#endif

  guint port;
  guint ssl_port;
  char *source_folder;
  SoupServer *server;
  SoupServer *ssl_server;
};

#ifdef USE_NEW_GLIB_MUTEX_API
#define LOCK(srv) g_mutex_lock(&(srv)->priv->lock)
#define UNLOCK(srv) g_mutex_unlock(&(srv)->priv->lock)
#define SIGNAL(srv) g_cond_signal(&(srv)->priv->cond)
#else
#define LOCK(srv) g_mutex_lock((srv)->priv->lock)
#define UNLOCK(srv) g_mutex_unlock((srv)->priv->lock)
#define SIGNAL(srv) g_cond_signal((srv)->priv->cond)
#endif

typedef struct
{
  /* Context fields */
  InsanityHttpServer *srv;
  SoupServer *server;
  gchar *path;

  /* Content fields */
  GMappedFile *f;
  GstBuffer *buf;
  gsize size;
  const char *contents;
  const char *ptr;
} ChunkedTransmitter;

G_DEFINE_TYPE (InsanityHttpServer, insanity_http_server, G_TYPE_OBJECT);

static void
insanity_http_server_dispose_simple (InsanityHttpServer * srv)
{
  InsanityHttpServerPrivate *priv = srv->priv;

  if (priv->server) {
    g_object_unref (priv->server);
    priv->server = NULL;
  }

  if (priv->ssl_server) {
    g_object_unref (priv->ssl_server);
    priv->ssl_server = NULL;
  }

  if (priv->test) {
    g_object_unref (srv->priv->test);
    srv->priv->test = NULL;
  }

  if (priv->mcontext) {
    g_main_context_unref (priv->mcontext);
    priv->mcontext = NULL;
  }

  if (priv->mloop) {
    g_main_loop_unref (priv->mloop);
    priv->mloop = NULL;
  }
}

static void
insanity_http_server_dispose (GObject * gobject)
{
  insanity_http_server_dispose_simple (INSANITY_HTTP_SERVER (gobject));

  G_OBJECT_CLASS (insanity_http_server_parent_class)->dispose (gobject);
}

static void
insanity_http_server_finalize_simple (InsanityHttpServer * srv)
{
  InsanityHttpServerPrivate *priv = srv->priv;

  g_free (priv->source_folder);
  g_free (priv->ssl_cert_file);
  g_free (priv->ssl_key_file);

#ifdef USE_NEW_GLIB_MUTEX_API
  g_mutex_clear (&priv->lock);
  g_cond_clear (&priv->cond);
#else
  g_mutex_free (priv->lock);
  g_cond_free (priv->cond);
#endif

  priv->ssl_cert_file = NULL;
  priv->ssl_key_file = NULL;
  priv->port = 0;
  priv->ssl_port = 0;
  priv->source_folder = NULL;
  priv->server = NULL;
  priv->ssl_server = NULL;
  srv->priv->test = NULL;
}

static void
insanity_http_server_finalize (GObject * gobject)
{
  insanity_http_server_finalize_simple (INSANITY_HTTP_SERVER (gobject));

  G_OBJECT_CLASS (insanity_http_server_parent_class)->finalize (gobject);
}

static void
insanity_http_server_get_property (GObject * gobject,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  InsanityHttpServer *srv = INSANITY_HTTP_SERVER (gobject);

  switch (prop_id) {
    case PROP_TEST:
      g_value_set_object (value, srv->priv->test);
      break;
    case PROP_CHUNKS_SIZE:
      g_value_set_ulong (value, srv->priv->chunks_size);
      break;
    default:
      g_assert_not_reached ();
  }
}

#define g_marshal_value_peek_object(v)   g_value_get_object (v)
static void
insanity_cclosure_user_marshal_GSTBUFFER__VOID (GClosure * closure,
    GValue * return_value G_GNUC_UNUSED,
    guint n_param_values,
    const GValue * param_values,
    gpointer invocation_hint G_GNUC_UNUSED, gpointer marshal_data)
{
  typedef GstBuffer *(*GMarshalFunc_GSTBUFFER__VOID) (gpointer data1,
      gpointer data2);
  register GMarshalFunc_GSTBUFFER__VOID callback;
  register GCClosure *cc = (GCClosure *) closure;
  register gpointer data1, data2;
  GstBuffer *v_return;

  g_return_if_fail (return_value != NULL);
  g_return_if_fail (n_param_values == 1);

  if (G_CCLOSURE_SWAP_DATA (closure)) {
    data1 = closure->data;
    data2 = g_value_peek_pointer (param_values + 0);
  } else {
    data1 = g_value_peek_pointer (param_values + 0);
    data2 = closure->data;
  }
  callback =
      (GMarshalFunc_GSTBUFFER__VOID) (marshal_data ? marshal_data :
      cc->callback);

  v_return = callback (data1, data2);

  gst_value_set_buffer (return_value, v_return);
}

static void
insanity_http_server_set_property (GObject * gobject,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  InsanityHttpServer *srv = INSANITY_HTTP_SERVER (gobject);

  switch (prop_id) {
    case PROP_TEST:
      srv->priv->test = g_value_dup_object (value);
      break;
    case PROP_CHUNKS_SIZE:
      srv->priv->chunks_size = g_value_get_ulong (value);
      break;
    default:
      g_assert_not_reached ();
  }
}

static void
insanity_http_server_class_init (InsanityHttpServerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = insanity_http_server_finalize;
  gobject_class->dispose = insanity_http_server_dispose;
  gobject_class->get_property = &insanity_http_server_get_property;
  gobject_class->set_property = &insanity_http_server_set_property;

  g_type_class_add_private (klass, sizeof (InsanityHttpServerPrivate));

  properties[PROP_TEST] =
      g_param_spec_object ("test", "Test", "The test the server is used for",
      INSANITY_TYPE_TEST,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_CHUNKS_SIZE] =
      g_param_spec_ulong ("chunks-size", "Chunks size", "The size of the chunks"
      "the severs writes in bytes", 0, G_MAXUINT64, DEFAULT_CHUNKS_SIZE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  signals[SIGNAL_GET_CONTENT] = g_signal_new ("get-content",
      G_TYPE_FROM_CLASS (gobject_class),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      0, NULL, NULL, insanity_cclosure_user_marshal_GSTBUFFER__VOID,
      GST_TYPE_BUFFER, 0, NULL);

  signals[SIGNAL_WRITING_CHUNK] = g_signal_new ("writing-chunk", G_TYPE_FROM_CLASS (gobject_class), G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS, 0, NULL, NULL, NULL, G_TYPE_NONE, 4, G_TYPE_STRING,    /* Path to file */
      G_TYPE_POINTER,           /* Pointer to the data prepared to be written */
      G_TYPE_UINT64,            /* Size of the data that will be written */
      G_TYPE_UINT64,            /* Size of the data missing before that chunk */
      NULL);

  signals[SIGNAL_WRITING_DONE] = g_signal_new ("writing-done", G_TYPE_FROM_CLASS (gobject_class), G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING,      /* Path to file writen */
      NULL);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, properties);
}

static void
insanity_http_server_init (InsanityHttpServer * srv)
{
  InsanityHttpServerPrivate *priv;

  priv = srv->priv = G_TYPE_INSTANCE_GET_PRIVATE (srv,
      INSANITY_TYPE_HTTP_SERVER, InsanityHttpServerPrivate);

#ifdef USE_NEW_GLIB_MUTEX_API
  g_mutex_init (&priv->lock);
  g_cond_init (&priv->cond);
#else
  priv->lock = g_mutex_new ();
  priv->cond = g_cond_new ();
#endif

  priv->port = 0;
  priv->chunks_size = DEFAULT_CHUNKS_SIZE;
  priv->ssl_port = 0;
  priv->source_folder = NULL;
  priv->server = NULL;
  priv->ssl_server = NULL;

  priv->thread = NULL;
  priv->mcontext = NULL;
  priv->mloop = NULL;
  priv->running = FALSE;
  priv->ready = TRUE;

  priv->ssl_cert_file = NULL;
  priv->ssl_key_file = NULL;
}

static void
free_chunked_transmitter (ChunkedTransmitter * ct)
{
  g_object_unref (ct->server);

  if (ct->f)
    g_mapped_file_unref (ct->f);

  if (ct->buf)
    gst_buffer_unref (ct->buf);

  g_free (ct->path);

  g_free (ct);
}

static void
http_message_finished (SoupMessage * msg, gpointer data)
{
  ChunkedTransmitter *ct = data;

  g_signal_emit (ct->srv, signals[SIGNAL_WRITING_DONE], 0, ct->path);

  free_chunked_transmitter (ct);
}

static void
write_next_chunk (SoupMessage * msg, gpointer data)
{
  ChunkedTransmitter *ct = data;
  SoupServer *server = g_object_ref (ct->server);
  char *buf;
  gsize size, buflen = ct->srv->priv->chunks_size,
      written = ct->ptr - ct->contents;

  size =
      (ct->ptr + buflen >
      ct->contents + ct->size) ? ct->contents + ct->size - ct->ptr : buflen;

  buf = g_malloc (size);
  memcpy (buf, ct->ptr, size);

  g_signal_emit (ct->srv, signals[SIGNAL_WRITING_CHUNK], 0,
      ct->path, ct->contents + written, size, ct->size - written);

  soup_message_body_append (msg->response_body, SOUP_MEMORY_TAKE, buf, size);
  ct->ptr += size;

  if (ct->ptr == ct->contents + ct->size) {
    /* done, will be automatically stopped */
  }

  soup_server_unpause_message (server, msg);
  g_object_unref (server);
}

static void
do_get (InsanityHttpServer * srv, SoupServer * server, SoupMessage * msg,
    const char *path)
{
  char *uri, *local_uri = NULL;
  GMappedFile *f = NULL;
  GstBuffer *buf = NULL;
  gssize size = -1;
  InsanityHttpServerPrivate *priv = srv->priv;
  InsanityTest *test = priv->test;
  ChunkedTransmitter *ct;

  SoupKnownStatusCode status = SOUP_STATUS_OK;

  uri = soup_uri_to_string (soup_message_get_uri (msg), FALSE);
  LOG ("request: \"%s\"\n", uri);

  /* Known URLs */
  if (!strcmp (path, "/404"))
    status = SOUP_STATUS_NOT_FOUND;

  if (status != SOUP_STATUS_OK)
    goto leave;

  if (priv->source_folder == NULL) {
    g_signal_emit (srv, signals[SIGNAL_GET_CONTENT], 0, &buf);

    size = GST_BUFFER_SIZE (buf);
    if (buf == NULL || size == -1) {
      LOG ("Make sure to set a callback to the "
          "\"content-size\" signal or set a local folder");
      status = SOUP_STATUS_NOT_FOUND;

      goto leave;
    }
  } else {
    if (strcmp (path, "/") == 0)
      local_uri = g_strdup (priv->source_folder);
    else
      local_uri = g_build_filename (priv->source_folder, path, NULL);

    if ((f = g_mapped_file_new (local_uri, FALSE, NULL)) == NULL) {
      status = SOUP_STATUS_NOT_FOUND;
      goto leave;
    }

    size = g_mapped_file_get_length (f);
  }

  if (msg->method == SOUP_METHOD_GET) {
    const char *contents;
    SoupRange *ranges = NULL;
    int nranges = 0;
    goffset start, end;

    if (size == 0) {
      contents = "";
    } else {
      if (priv->source_folder == NULL) {
        contents = (gchar *) GST_BUFFER_DATA (buf);
      } else
        contents = g_mapped_file_get_contents (f);
    }
    if (!contents) {
      status = SOUP_STATUS_NOT_FOUND;
      goto leave;
    }

    if (soup_message_headers_get_ranges (msg->request_headers, size, &ranges,
            &nranges)) {
      start = ranges[0].start;
      end = ranges[0].end;
      soup_message_headers_free_ranges (msg->request_headers, ranges);
      soup_message_headers_set_content_range (msg->response_headers, start, end,
          size);
      status = SOUP_STATUS_PARTIAL_CONTENT;
    } else {
      start = 0;
      end = size - 1;
    }
    soup_message_headers_set_content_length (msg->response_headers, size);

    /* We'll send in chunks */
    ct = g_malloc (sizeof (ChunkedTransmitter));
    ct->server = g_object_ref (server);
    ct->srv = srv;
    ct->path = g_strdup (path);
    if (f) {
      ct->f = g_mapped_file_ref (f);
      ct->buf = NULL;
    } else {
      ct->buf = gst_buffer_ref (buf);
      ct->f = NULL;
    }
    ct->contents = contents;
    ct->ptr = contents + start;
    ct->size = end + 1;

    soup_message_headers_set_encoding (msg->response_headers,
        SOUP_ENCODING_CONTENT_LENGTH);
    g_signal_connect (msg, "finished", G_CALLBACK (http_message_finished), ct);
    g_signal_connect (msg, "wrote-chunk", G_CALLBACK (write_next_chunk), ct);
    g_signal_connect (msg, "wrote-headers", G_CALLBACK (write_next_chunk), ct);
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
  g_free (local_uri);
  if (f)
    g_mapped_file_unref (f);
}

static void
print_header (const char *name, const char *value, gpointer data)
{
  InsanityTest *test = data;
  LOG ("header: %s: %s\n", name, value);
}

static void
server_callback (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query,
    SoupClientContext * context, gpointer data)
{
  InsanityHttpServer *srv = data;
  InsanityTest *test = srv->priv->test;

  LOG ("%s %s HTTP/1.%d\n",
      msg->method, path, soup_message_get_http_version (msg));
  soup_message_headers_foreach (msg->request_headers, print_header, test);
  if (msg->request_body->length)
    LOG ("%s\n", msg->request_body->data);

  if (msg->method == SOUP_METHOD_GET || msg->method == SOUP_METHOD_HEAD)
    do_get (srv, server, msg, path);
  else
    soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);

  LOG ("  -> %d %s\n", msg->status_code, msg->reason_phrase);
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
find_link_local_address (InsanityTest * test)
{
  SoupAddress *sa = NULL;

#ifdef HAVE_IFADDRS_H
  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs (&ifaddr) < 0) {
    LOG ("Failed to fetch interface addresses, will bind on ANY");
    return NULL;
  }

  ifa = ifaddr;
  while (ifa) {
    if (ifa->ifa_addr->sa_family == AF_INET) {
      if (ifa->ifa_name && !strcmp (ifa->ifa_name, "lo")) {
        sa = soup_address_new_from_sockaddr (ifa->ifa_addr,
            sizeof (struct sockaddr_in));
        break;
      }
    }
    ifa = ifa->ifa_next;
  }

  freeifaddrs (ifaddr);
#endif

  return sa;
}

static gboolean
http_server_start (InsanityHttpServer * srv)
{
  SoupServer *server = NULL;
  SoupServer *ssl_server = NULL;
  SoupAddress *bind_address;
  guint port = SOUP_ADDRESS_ANY_PORT;
  guint ssl_port = SOUP_ADDRESS_ANY_PORT;
  SoupAuthDomain *domain = NULL;
  GSimpleAsyncResult *async_res = srv->priv->async_res;
  gboolean ret = TRUE;

  InsanityHttpServerPrivate *priv = srv->priv;
  InsanityTest *test = priv->test;

  /* We add the server-started as a checklist item of the test */
  insanity_test_add_checklist_item (test, "server-started",
      "The internal HTTP server was started", NULL, FALSE);

  priv->port = 0;
  priv->ssl_port = 0;

  bind_address = find_link_local_address (test);

  server =
      soup_server_new (SOUP_SERVER_PORT, port, SOUP_SERVER_INTERFACE,
      bind_address, SOUP_SERVER_ASYNC_CONTEXT, priv->mcontext, NULL);

  if (!server) {
    char *message = g_strdup_printf ("Unable to bind to server port %u", port);
    insanity_test_validate_checklist_item (test, "server-started", FALSE,
        message);
    g_free (message);

    if (async_res)
      g_simple_async_result_set_op_res_gboolean (async_res, FALSE);

    ret = FALSE;
    goto done;
  }
  priv->port = soup_server_get_port (server);
  LOG ("HTTP server listening on port %u\n", priv->port);
  soup_server_add_handler (server, NULL, server_callback, srv, NULL);

  priv->server = server;

  if (priv->ssl_cert_file && priv->ssl_key_file) {
    ssl_server = soup_server_new (SOUP_SERVER_PORT, ssl_port,
        SOUP_SERVER_INTERFACE, bind_address,
        SOUP_SERVER_SSL_CERT_FILE, priv->ssl_cert_file,
        SOUP_SERVER_SSL_KEY_FILE, priv->ssl_key_file, NULL);

    if (!ssl_server) {
      char *message =
          g_strdup_printf ("Unable to bind to SSL server port %u", ssl_port);
      insanity_test_validate_checklist_item (test, "server-started", FALSE,
          message);
      g_free (message);
      g_object_unref (priv->server);
      priv->server = NULL;

      if (async_res)
        g_simple_async_result_set_op_res_gboolean (async_res, FALSE);

      ret = FALSE;
      goto done;
    }
    priv->ssl_port = soup_server_get_port (ssl_server);
    LOG ("HTTPS server listening on port %u\n", priv->ssl_port);
    soup_server_add_handler (ssl_server, NULL, server_callback, srv, NULL);

    priv->ssl_server = ssl_server;
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
  if (ssl_server)
    soup_server_run_async (ssl_server);

  insanity_test_validate_checklist_item (test, "server-started", TRUE, NULL);

  if (async_res)
    g_simple_async_result_set_op_res_gboolean (async_res, TRUE);

done:
  /* Complete async operation async */
  g_simple_async_result_complete_in_idle (async_res);

  srv->priv->running = ret;
  srv->priv->ready = TRUE;
  SIGNAL (srv);

  return ret;
}

static gpointer
mainloop_thread_func (gpointer data)
{
  InsanityHttpServer *srv = INSANITY_HTTP_SERVER (data);
  InsanityHttpServerPrivate *priv = srv->priv;

  /* Make sure that we allocate all the ressources when needed */
  if (priv->ready == FALSE) {
    priv->mcontext = g_main_context_new ();
    priv->mloop = g_main_loop_new (priv->mcontext, TRUE);

    LOCK (srv);
    srv->priv->running = http_server_start (srv);
    UNLOCK (srv);
  }

  while (priv->running)
    g_main_context_iteration (priv->mcontext, TRUE);

  return NULL;
}

/**
 * insanity_http_server_is_running:
 * @srv: The #InsanityHttpServer to check
 *
 * Check if @srv is running or not
 */
gboolean
insanity_http_server_is_running (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), FALSE);

  return srv->priv->running;
}

/**
 * insanity_http_server_start_async:
 * @srv: The #InsanityHttpServer to start
 * @callback: (scope async): The #GAsyncReadyCallback callback that will be called
 * when the server is set up, you can easily check whether the operation
 * succeded or not with g_simple_async_result_get_op_res_gboolean on
 * the GAsynResult.
 * @ssl_cert_file: the ssl certification file to use or %NULL if not using an
 * ssl server
 * @ssl_key_file: The ssl key file to use, or %NULL if not using an SSL server
 * @user_data: (closure):callback data.
 *
 * Starts running @srv
 */
gboolean
insanity_http_server_start_async (InsanityHttpServer * srv,
    GAsyncReadyCallback callback, const char *ssl_cert_file,
    const char *ssl_key_file, gpointer user_data)
{
  InsanityHttpServerPrivate *priv = srv->priv;
  InsanityTest *test = priv->test;

  if (srv->priv->thread != NULL) {
    LOG ("Server already started");

    return FALSE;
  }

  /* The server has been stopped but it is still ready to run */
  if (srv->priv->ready == TRUE) {
    soup_server_run_async (priv->server);

    if (priv->ssl_server)
      soup_server_run_async (priv->ssl_server);

    LOCK (srv);
    srv->priv->running = TRUE;
    UNLOCK (srv);
  }

  srv->priv->async_res = g_simple_async_result_new (G_OBJECT (srv),
      (GAsyncReadyCallback) callback, user_data,
      insanity_http_server_start_async);

  /* Start the server thread */
  srv->priv->thread =
#if GLIB_CHECK_VERSION(2,31,2)
      g_thread_new ("insanity_http_server", mainloop_thread_func, srv);
#else
      g_thread_create ((GThreadFunc) mainloop_thread_func, srv, TRUE, NULL);
#endif

  if (srv->priv->thread == NULL) {
    LOG ("Could not start thread");
    g_object_unref (srv->priv->async_res);
    srv->priv->async_res = NULL;

    return FALSE;
  }

  srv->priv->ssl_cert_file = g_strdup (ssl_cert_file);
  srv->priv->ssl_key_file = g_strdup (ssl_key_file);

  return TRUE;
}

static void
server_started_cb (GObject * server, GAsyncResult * res, gpointer psrv)
{
  InsanityHttpServer *srv = INSANITY_HTTP_SERVER (psrv);
  InsanityTest *test = srv->priv->test;
  GSimpleAsyncResult *sres = G_SIMPLE_ASYNC_RESULT (res);

  /* Server could no start */
  if (g_simple_async_result_get_op_res_gboolean (sres) == FALSE) {
    LOG ("Server could not start");
    insanity_test_done (srv->priv->test);
  } else {
    LOG ("Server started");
  }
}

/**
 * insanity_http_server_start:
 * @srv: The #InsanityHttpServer to start
 * @ssl_cert_file: the ssl certification file to use or %NULL if not using an
 * ssl server
 * @ssl_key_file: The ssl key file to use, or %NULL if not using an SSL server
 *
 * Starts running @srv
 *
 * Returns: %TRUE if the server could be started %FALSE otherwize
 */
gboolean
insanity_http_server_start (InsanityHttpServer * srv,
    const char *ssl_cert_file, const char *ssl_key_file)
{
  gboolean started;

  srv->priv->ready = FALSE;

  started = insanity_http_server_start_async (srv,
      (GAsyncReadyCallback) server_started_cb, ssl_cert_file,
      ssl_key_file, srv);

  if (started == FALSE)
    return FALSE;

  while (srv->priv->ready == FALSE)
    g_cond_wait (srv->priv->cond, srv->priv->lock);

  return srv->priv->running;
}

/**
 * insanity_http_server_stop:
 * @srv: The #InsanityHttpServer to stop
 *
 * Stop running @srv
 *
 * Returns: %TRUE if the server could be stoped %FALSE otherwize. If the server
 * was not running, returns %FALSE
 */
gboolean
insanity_http_server_stop (InsanityHttpServer * srv)
{
  InsanityHttpServerPrivate *priv = srv->priv;

  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), FALSE);

  if (priv->running == FALSE)
    return FALSE;

  soup_server_quit (priv->server);
  if (priv->ssl_server)
    soup_server_quit (priv->ssl_server);

  LOCK (srv);
  priv->running = FALSE;
  UNLOCK (srv);

  return TRUE;
}

/**
 * insanity_http_server_new:
 * @test: The #InsanityTest that will make use of @server
 *
 * Creates a new #InsanityHttpServer
 *
 * Returns: (transfer full): A newly created #InsanityHttpServer
 */
InsanityHttpServer *
insanity_http_server_new (InsanityTest * test)
{
  g_return_val_if_fail (INSANITY_IS_TEST (test), NULL);

  return g_object_new (INSANITY_TYPE_HTTP_SERVER, "test", test, NULL);
}

/**
 * insanity_http_server_get_port:
 * @srv: The #InsanityHttpServer to get the port from
 *
 * Get the port currently in use by @srv
 *
 * Returns: The port in use by @srv
 */
guint
insanity_http_server_get_port (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), 0);

  return srv->priv->port;
}

/**
 * insanity_http_server_get_ssl_port:
 * @srv: The #InsanityHttpServer to get the ssl port from
 *
 * Get the ssl port currently in use by @srv
 *
 * Returns: The ssl port in use by @srv
 */
guint
insanity_http_server_get_ssl_port (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), 0);

  return srv->priv->ssl_port;
}

/**
 * insanity_http_server_get_source_folder:
 * @srv: The #InsanityHttpServer to get the source folder from.
 *
 * Get the source folder currently used by @srv. If set to NULL, the
 * callback method will be used.
 *
 * Returns: (transfer-none): The path to the working directory
 * of @srv
 */
const char *
insanity_http_server_get_source_folder (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), NULL);

  return srv->priv->source_folder;
}

/**
 * insanity_http_server_set_source_folder:
 * @srv: The #InsanityHttpServer to set the source folder on
 * @source_folder: The path to the directory @server should work in.
 *
 * Set the working directory of @srv.
 */
void
insanity_http_server_set_source_folder (InsanityHttpServer * srv,
    const gchar * source_folder)
{
  g_return_if_fail (INSANITY_IS_HTTP_SERVER (srv));

  g_free (srv->priv->source_folder);
  srv->priv->source_folder = g_strdup (source_folder);
}

/**
 * insanity_http_server_get_soup_server:
 * @srv: The #InsanityHttpServer to get the non ssl server from
 *
 * Get the #SoupServer used by @srv as non ssl server.
 *
 * Returns: (transfer-none): The #SoupServer in use by @srv
 */
SoupServer *
insanity_http_server_get_soup_server (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), NULL);

  return srv->priv->server;
}

/**
 * insanity_http_server_get_soup_ssl_server:
 * @srv: The #InsanityHttpServer to get the ssl server from
 *
 * Get the #SoupServer used by @srv as ssl server.
 *
 * Returns: (transfer-none): The #SoupServer in use by as ssl server in @srv
 */
SoupServer *
insanity_http_server_get_soup_ssl_server (InsanityHttpServer * srv)
{
  g_return_val_if_fail (INSANITY_IS_HTTP_SERVER (srv), NULL);

  return srv->priv->ssl_server;
}

/**
 * insanity_http_server_reset:
 * @srv: The #InsanityHttpServer to reset
 *
 * Reset @srv, frees all ressource and make it usable as if newly created
 */
void
insanity_http_server_reset (InsanityHttpServer * srv)
{
  g_return_if_fail (INSANITY_IS_HTTP_SERVER (srv));

  insanity_http_server_dispose_simple (srv);
  insanity_http_server_finalize_simple (srv);
}
