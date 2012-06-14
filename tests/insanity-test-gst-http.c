/* Insanity QA system

 Copyright (c) 2012, Collabora Ltd
 Author: Vincent Penquerc'h <vincent@collabora.co.uk>

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
#include <insanity-gst/insanity-gst.h>
#include "insanity-http-server.h"

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (40 * GST_MSECOND)

static GstElement *global_pipeline = NULL;
static const char *global_validate_on_playing = NULL;
static gboolean global_done_http = FALSE;
static InsanityHttpServer *global_server = NULL;
static GstClockTime global_duration = GST_CLOCK_TIME_NONE;
static int global_seek_target = -1;
static GstClockTime global_wait_time = GST_CLOCK_TIME_NONE;
static GstClockTime global_playback_time = GST_CLOCK_TIME_NONE;
static guint global_duration_timeout = 0;
static guint global_timer_id = 0;

static gchar *http_uri = NULL;
static gchar *https_uri = NULL;

static void
source_setup_cb (GstElement * playbin, GstElement * source, gpointer user_data)
{
  g_object_set (source, "user-id", good_user, "user-pw", good_pw, NULL);
}

static GstPipeline *
http_test_create_pipeline (InsanityGstPipelineTest * ptest, gpointer userdata)
{
  GstElement *pipeline = NULL;
  const char *launch_line = "playbin2 audio-sink=fakesink video-sink=fakesink";
  GError *error = NULL;

  pipeline = gst_parse_launch (launch_line, &error);
  if (!pipeline) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, error ? error->message : NULL);
    if (error)
      g_error_free (error);
    return NULL;
  } else if (error) {
    /* Do we get a dangling pointer here ? gst-launch.c does not unref */
    pipeline = NULL;
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, error->message);
    g_error_free (error);
    return NULL;
  }

  g_signal_connect (pipeline, "source-setup", G_CALLBACK (source_setup_cb),
      NULL);

  global_pipeline = pipeline;

  return GST_PIPELINE (pipeline);
}

static GstClockTime
http_test_get_position (InsanityTest * test)
{
  gint64 pos = 0;
  gboolean res;
  GstFormat format = GST_FORMAT_TIME;

  res = gst_element_query_position (global_pipeline, &format, &pos);
  if (format != GST_FORMAT_TIME)
    res = FALSE;
  insanity_test_validate_checklist_item (test, "position-queried", res, NULL);
  if (!res) {
    pos = GST_CLOCK_TIME_NONE;
  }
  return pos;
}

static GstClockTime
http_test_get_wait_time (InsanityTest * test)
{
  gint64 pos = http_test_get_position (test);
  if (GST_CLOCK_TIME_IS_VALID (pos)) {
    pos += global_playback_time * GST_SECOND;
  }
  return pos;
}

static gboolean
wait_and_do_seek (gpointer data)
{
  InsanityTest *test = data;
  GstEvent *event;
  gboolean res;

  if (GST_CLOCK_TIME_IS_VALID (global_wait_time)
      && http_test_get_position (test) < global_wait_time)
    return TRUE;

  /* If duration did not become known yet, we cannot test */
  if (!GST_CLOCK_TIME_IS_VALID (global_duration)) {
    insanity_test_validate_checklist_item (test, "duration-known", FALSE, NULL);
    insanity_test_done (test);
    return FALSE;
  }

  /* seek to the middle of the stream */
  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
      GST_SEEK_TYPE_SET, global_duration * global_seek_target / 100,
      GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  global_validate_on_playing = "seek";
  res = gst_element_send_event (global_pipeline, event);
  if (!res) {
    global_validate_on_playing = NULL;
    insanity_test_validate_checklist_item (test, "seek", FALSE,
        "Failed to send seek event");
    return FALSE;
  }
  gst_element_get_state (global_pipeline, NULL, NULL, SEEK_TIMEOUT);

  return FALSE;
}

static gboolean
wait_and_end_step (gpointer data)
{
  InsanityTest *test = data;
  SoupServer *ssl_server;

  if (GST_CLOCK_TIME_IS_VALID (global_wait_time)
      && http_test_get_position (test) < global_wait_time)
    return TRUE;

  ssl_server = insanity_http_server_get_soup_ssl_server (global_server);
  /* If we have both a non SSL and a SSL server, test both */
  if (global_done_http || ssl_server == NULL) {
    insanity_test_done (test);
  } else {
    global_done_http = TRUE;

    gst_element_set_state (global_pipeline, GST_STATE_READY);
    g_object_set (global_pipeline, "uri", https_uri, NULL);
    gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
    gst_element_get_state (global_pipeline, NULL, NULL, GST_SECOND * 2);

    global_wait_time = http_test_get_wait_time (test);
    global_timer_id =
        g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, test);
  }
  return FALSE;
}

static gboolean
http_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (global_pipeline)) {
        const char *validate_checklist_item = global_validate_on_playing;
        GstState oldstate, newstate, pending;

        gst_message_parse_state_changed (msg, &oldstate, &newstate, &pending);
        if (newstate == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING
            && validate_checklist_item) {
          global_validate_on_playing = NULL;
          insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
              validate_checklist_item, TRUE, NULL);
          /* let it run a couple seconds */
          global_wait_time = http_test_get_wait_time (INSANITY_TEST (ptest));
          global_timer_id =
              g_timeout_add (250, (GSourceFunc) & wait_and_end_step,
              INSANITY_TEST (ptest));
        }
      }
      break;
    default:
      break;
  }

  return TRUE;

}

static gboolean
http_test_setup (InsanityTest * test)
{
  char *ssl_cert_file = NULL;
  char *ssl_key_file = NULL;
  GValue v = { 0 };
  gboolean started;

  if (insanity_test_get_argument (test, "ssl-cert-file", &v)) {
    ssl_cert_file = g_value_dup_string (&v);
    g_value_unset (&v);
  }
  if (insanity_test_get_argument (test, "ssl-key-file", &v)) {
    ssl_key_file = g_value_dup_string (&v);
    g_value_unset (&v);
  }

  global_server = insanity_http_server_new (test);

  started = insanity_http_server_start (global_server,
      ssl_cert_file, ssl_key_file);

  g_free (ssl_key_file);
  g_free (ssl_cert_file);

  return started;
}

static void
http_test_teardown (InsanityTest * test)
{
  if (global_server != NULL) {
    g_object_unref (global_server);
    global_server = NULL;
  }
}

static gboolean
duration_timeout (gpointer data)
{
  InsanityTest *test = data;

  insanity_test_validate_checklist_item (test, "duration-known", FALSE,
      "No duration, even after playing for a bit");
  insanity_test_done (test);
  return FALSE;
}

static gboolean
http_test_start (InsanityTest * test)
{
  GValue uri = { 0 }, ival = {
  0};
  const char *protocol;
  gchar *source_folder;
  SoupServer *ssl_server;
  guint port, ssl_port;

  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (g_value_get_string (&uri), "")) {
    insanity_test_validate_checklist_item (test, "valid-pipeline", FALSE,
        "No URI to test on");
    g_value_unset (&uri);
    return FALSE;
  }

  if (!gst_uri_is_valid (g_value_get_string (&uri))) {
    insanity_test_validate_checklist_item (test, "uri-is-file", FALSE, NULL);
    g_value_unset (&uri);
    return FALSE;
  }
  protocol = gst_uri_get_protocol (g_value_get_string (&uri));
  if (!protocol || g_ascii_strcasecmp (protocol, "file")) {
    insanity_test_validate_checklist_item (test, "uri-is-file", FALSE, NULL);
    g_value_unset (&uri);
    return FALSE;
  }
  insanity_test_validate_checklist_item (test, "uri-is-file", TRUE, NULL);
  source_folder = gst_uri_get_location (g_value_get_string (&uri));
  insanity_http_server_set_source_folder (global_server, source_folder);
  g_free (source_folder);
  g_value_unset (&uri);

  port = insanity_http_server_get_port (global_server);
  ssl_port = insanity_http_server_get_ssl_port (global_server);
  ssl_server = insanity_http_server_get_soup_ssl_server (global_server);
  if (ssl_server) {
    https_uri = g_strdup_printf ("https://127.0.0.1:%u/", ssl_port);
  }
  http_uri = g_strdup_printf ("http://127.0.0.1:%u/", port);
  g_object_set (global_pipeline, "uri", http_uri, NULL);

  global_validate_on_playing = NULL;
  global_done_http = FALSE;
  global_duration = GST_CLOCK_TIME_NONE;

  insanity_test_get_argument (test, "playback-time", &ival);
  global_playback_time = g_value_get_int (&ival);
  g_value_unset (&ival);

  insanity_test_get_argument (test, "seek-target", &ival);
  global_seek_target = g_value_get_int (&ival);
  g_value_unset (&ival);

  return TRUE;
}

static void
http_test_stop (InsanityTest * test)
{
  if (global_timer_id) {
    g_source_remove (global_timer_id);
    global_timer_id = 0;
  }

  if (global_duration_timeout) {
    g_source_remove (global_duration_timeout);
    global_duration_timeout = 0;
  }

  g_free (http_uri);
  http_uri = NULL;
  g_free (https_uri);
  https_uri = NULL;
}

static gboolean
wait_and_start (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;

  /* Try getting duration */
  if (!GST_CLOCK_TIME_IS_VALID (global_wait_time)) {
    insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_TIME, NULL);
  }

  /* If we have it, start; if not, we'll be called again */
  if (GST_CLOCK_TIME_IS_VALID (global_wait_time)) {
    global_wait_time = http_test_get_wait_time (INSANITY_TEST (ptest));
    global_timer_id =
        g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, ptest);
    return FALSE;
  }
  return TRUE;
}

static void
http_test_test (InsanityGstPipelineTest * ptest)
{
  global_duration_timeout =
      g_timeout_add (5000, (GSourceFunc) & duration_timeout, ptest);
  global_timer_id = g_timeout_add (250, (GSourceFunc) & wait_and_start, ptest);
}

static void
http_test_duration (InsanityGstPipelineTest * ptest, GstFormat fmt,
    GstClockTime duration)
{
  gboolean start = FALSE;

  /* If we were waiting on it to start up, do it now */
  if (global_duration_timeout) {
    g_source_remove (global_duration_timeout);
    global_duration_timeout = 0;
    start = TRUE;
  }

  insanity_test_printf (INSANITY_TEST (ptest),
      "Just got notified duration is %" GST_TIME_FORMAT "\n",
      GST_TIME_ARGS (duration));
  global_duration = duration;
  insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
      "duration-known", TRUE, NULL);

  if (start) {
    /* start now if we were waiting for the duration before doing so */
    global_wait_time = http_test_get_wait_time (INSANITY_TEST (ptest));
    global_timer_id =
        g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, ptest);
  }
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  GValue vdef = { 0 };

  g_type_init ();

  ptest =
      insanity_gst_pipeline_test_new ("http-test", "Tests HTTP streaming",
      NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, "");
  insanity_test_add_argument (test, "uri", "The uri to test on (file only)",
      NULL, FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, NULL);
  insanity_test_add_argument (test, "ssl-cert-file",
      "Certificate file for SSL server", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, NULL);
  insanity_test_add_argument (test, "ssl-key-file", "Key file for SSL server",
      NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_INT);
  g_value_set_int (&vdef, 5);
  insanity_test_add_argument (test, "playback-time",
      "Stream time to playback for before seeking, in seconds", NULL, TRUE,
      &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_INT);
  g_value_set_int (&vdef, 50);
  insanity_test_add_argument (test, "seek-target",
      "Seek target in percentage of the stream duration", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "uri-is-file",
      "The URI is a file URI", NULL, FALSE);

  insanity_test_add_checklist_item (test, "seek", "A seek succeeded", NULL,
      FALSE);
  insanity_test_add_checklist_item (test, "duration-known",
      "Stream duration could be determined", NULL, FALSE);
  insanity_test_add_checklist_item (test, "position-queried",
      "Stream position could be determined", NULL, FALSE);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &http_test_create_pipeline, NULL, NULL);
  g_signal_connect_after (test, "setup", G_CALLBACK (&http_test_setup), 0);
  g_signal_connect_after (test, "bus-message",
      G_CALLBACK (&http_test_bus_message), 0);
  g_signal_connect (test, "start", G_CALLBACK (&http_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&http_test_stop), 0);
  g_signal_connect (test, "test", G_CALLBACK (&http_test_test), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&http_test_teardown),
      0);
  g_signal_connect_after (ptest, "duration::time",
      G_CALLBACK (&http_test_duration), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
