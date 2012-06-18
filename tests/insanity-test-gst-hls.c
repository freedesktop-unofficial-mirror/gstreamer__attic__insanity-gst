/**
 * Insanity QA system
*
 * Copyright (c) 2012, Collabora Ltd
 *    Author: Thibault Saunier <thibault.saunier@collabora.com>
 *
 *  (Partly cc'ed from tests/insanity-test-gst-http.c)
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
#define PLAY_TIMEOUT (10 * G_USEC_PER_SEC)

#define LOG(format, args...) \
  INSANITY_LOG (test, "hls", INSANITY_LOG_LEVEL_DEBUG, format, ##args)

static unsigned glob_nsinks = 0;
static guint glob_timer_id = 0;
static guint glob_seek_nb = 0;
static gint64 glob_first_wait = 0;
static unsigned int glob_state = 0;
static gboolean glob_is_live = FALSE;
static gboolean glob_done_hls = FALSE;
static gboolean glob_buffered = FALSE;
static gboolean glob_play_in_time = TRUE;
static guint glob_duration_timeout = 0;
static guint glob_buffering_timeout = 0;
static gulong glob_probes[2] = { 0, 0 };

static GstElement *glob_hlsdemux = NULL;
static GstElement *glob_pipeline = NULL;
static gboolean glob_is_seekable = FALSE;
static GstPad *glob_sinks[2] = { NULL, NULL };

static const char *glob_validate_on_playing = NULL;
static GstClockTime glob_target = GST_CLOCK_TIME_NONE;
static GstClockTime glob_segment = GST_CLOCK_TIME_NONE;
static GstClockTime glob_duration = GST_CLOCK_TIME_NONE;
static GstClockTime glob_playback_time = 5;
static GstClockTime glob_wait_time = GST_CLOCK_TIME_NONE;

typedef struct
{
  guint perc;
  gboolean seeked;
  gboolean segment_received;
  gboolean buffer_received;
} SeekingStatus;

static SeekingStatus seek_targets[] = {
  {50, FALSE, FALSE, FALSE},
  {75, FALSE, FALSE, FALSE},
};

static GStaticMutex glob_mutex = G_STATIC_MUTEX_INIT;

static InsanityHttpServer *glob_server;

#define HLS_TEST_LOCK() g_static_mutex_lock (&glob_mutex)
#define HLS_TEST_UNLOCK() g_static_mutex_unlock (&glob_mutex)


/* How far we allow a timestamp to be to match our target */
/* 3 quarters of a second for now. Seeking precision isn't
   very good it seems. Needs to be at the very least one
   frame's worth for low framerate video. */
#define SEEK_THRESHOLD  (GST_SECOND * 3)

static void
source_setup_cb (GstElement * playbin, GstElement * source, gpointer user_data)
{
  g_object_set (source, "user-id", good_user, "user-pw", good_pw, NULL);
}

static GstPipeline *
hls_test_create_pipeline (InsanityGstPipelineTest * ptest, gpointer userdata)
{
  GstElement *pipeline = NULL;
  const char *launch_line =
      "playbin2 audio-sink=\"fakesink name=asink\" video-sink=\"fakesink name=vsink\"";
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

  glob_pipeline = pipeline;

  return GST_PIPELINE (pipeline);
}

static GstClockTime
hls_test_get_position (InsanityTest * test)
{
  gint64 pos = 0;
  gboolean res;
  GstFormat format = GST_FORMAT_TIME;

  res = gst_element_query_position (glob_pipeline, &format, &pos);
  if (format != GST_FORMAT_TIME)
    res = FALSE;
  LOG ("Position %" GST_TIME_FORMAT " ..queried (success %i)\n",
      GST_TIME_ARGS (pos), res);

  insanity_test_validate_checklist_item (test, "position-queried", res, NULL);
  if (!res) {
    pos = GST_CLOCK_TIME_NONE;
  }

  return pos;
}

static GstClockTime
hls_test_get_wait_time (InsanityTest * test)
{
  gint64 pos = hls_test_get_position (test);

  if (GST_CLOCK_TIME_IS_VALID (pos)) {
    pos += glob_playback_time * GST_SECOND;
  }

  return pos;
}

static gboolean
wait_and_do_seek (gpointer data)
{
  InsanityTest *test = data;
  GstEvent *event;
  gboolean res;

  if (GST_CLOCK_TIME_IS_VALID (glob_wait_time)) {
    GstClockTime cur = hls_test_get_position (test);

    if (glob_first_wait == 0)
      glob_first_wait = g_get_monotonic_time ();

    if (cur < glob_wait_time) {
      guint64 diff = g_get_monotonic_time () - glob_first_wait;

      if (diff > glob_playback_time * G_USEC_PER_SEC + PLAY_TIMEOUT)
        glob_play_in_time = FALSE;
      else
        return TRUE;
    }
  }

  glob_first_wait = 0;

  LOG ("Seeking at %i\n", seek_targets[glob_seek_nb].perc);

  /* If duration did not become known yet, we cannot test */
  if (!GST_CLOCK_TIME_IS_VALID (glob_duration)) {
    insanity_test_validate_checklist_item (test, "duration-known", FALSE, NULL);
    insanity_test_done (test);
    return FALSE;
  }

  glob_target = gst_util_uint64_scale (glob_duration,
      seek_targets[glob_seek_nb].perc, 100);

  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
      GST_SEEK_TYPE_SET, glob_target, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

  glob_validate_on_playing = "seek";
  res = gst_element_send_event (glob_pipeline, event);
  if (!res) {
    glob_validate_on_playing = NULL;
    insanity_test_validate_checklist_item (test, "seek", FALSE,
        "Failed to send seek event");
    return FALSE;
  }
  seek_targets[glob_seek_nb].seeked = TRUE;
  gst_element_get_state (glob_pipeline, NULL, NULL, SEEK_TIMEOUT);
  insanity_test_validate_checklist_item (test, "seek", TRUE, NULL);

  return FALSE;
}

static gboolean
wait_and_end_step (gpointer data)
{
  InsanityTest *test = data;

  if (GST_CLOCK_TIME_IS_VALID (glob_wait_time)
      && hls_test_get_position (test) < glob_wait_time)
    return TRUE;

  if (glob_is_seekable) {
    glob_wait_time = hls_test_get_wait_time (test);
    glob_timer_id = g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, test);
  }

  return FALSE;
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
buffering_timeout (gpointer data)
{
  InsanityTest *test = data;

  if (glob_buffered)
    return FALSE;

  insanity_test_validate_checklist_item (test, "buffering-done", FALSE,
      "Buffering never ended");
  insanity_test_done (test);
  return FALSE;
}

static gint
find_hlsdemux (GstElement * e)
{
  GstObject *fact = GST_OBJECT (gst_element_get_factory (e));
  gchar *name = gst_object_get_name (fact);

  if (g_strcmp0 (name, "hlsdemux") == 0) {
    g_free (name);
    return 0;
  }

  g_free (name);
  gst_object_unref (e);

  return 1;
}

static gboolean
probe (InsanityGstTest * ptest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  InsanityTest *test = INSANITY_TEST (ptest);
  GstClockTimeDiff diff;


  HLS_TEST_LOCK ();

  if (GST_IS_BUFFER (object)) {
    if (GST_CLOCK_TIME_IS_VALID (glob_target) &&
        GST_CLOCK_TIME_IS_VALID (glob_segment)) {
      diff = GST_CLOCK_DIFF (GST_BUFFER_TIMESTAMP (object), glob_target);
      if (diff < 0)
        diff = -diff;

      LOG ("Got buffer start %" GST_TIME_FORMAT
          ", expected around %" GST_TIME_FORMAT ", off by %" GST_TIME_FORMAT
          ", method %d\n",
          GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (object)),
          GST_TIME_ARGS (glob_target), GST_TIME_ARGS (diff), glob_state);

      if (diff < SEEK_THRESHOLD) {
        /* Reseting global segment for next seek */
        glob_segment = GST_CLOCK_TIME_NONE;

        seek_targets[glob_seek_nb].buffer_received = TRUE;
        glob_seek_nb++;
        if (glob_is_seekable && glob_seek_nb < G_N_ELEMENTS (seek_targets)) {
          /* Program next seek */
          glob_wait_time = hls_test_get_wait_time (INSANITY_TEST (ptest));
          glob_timer_id = g_timeout_add (250, (GSourceFunc) & wait_and_end_step,
              INSANITY_TEST (ptest));
        } else {
          /* Done with the test */
          insanity_test_done (test);
        }
      }
    }

  } else {
    GstEvent *event = GST_EVENT (object);

    if (GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT) {
      gint64 start;
      gboolean update;

      gst_event_parse_new_segment (event, &update, NULL, NULL, &start, NULL,
          NULL);

      /* ignore segment updates */
      if (update)
        goto ignore_segment;

      /* Not waiting for a segment, ignoring */
      if (!GST_CLOCK_TIME_IS_VALID (glob_target)) {
        LOG ("Got segment starting at %" GST_TIME_FORMAT
            ", but we are not waiting for segment\n", GST_TIME_ARGS (start));
        goto ignore_segment;
      }

      /* Checking the segment has good timing */
      diff = GST_CLOCK_DIFF (start, glob_target);
      if (diff < 0)
        diff = -diff;

      if (diff > SEEK_THRESHOLD) {
        LOG ("Got segment start %" GST_TIME_FORMAT
            ", expected around %" GST_TIME_FORMAT ", off by %" GST_TIME_FORMAT
            ", method %d\n",
            GST_TIME_ARGS (start), GST_TIME_ARGS (glob_target),
            GST_TIME_ARGS (diff), glob_state);

      } else {
        LOG ("Got segment start %" GST_TIME_FORMAT
            ", expected around %" GST_TIME_FORMAT ", off by %" GST_TIME_FORMAT
            ", method %d\n", GST_TIME_ARGS (start), GST_TIME_ARGS (glob_target),
            GST_TIME_ARGS (diff), glob_state);
        seek_targets[glob_seek_nb].segment_received = TRUE;

        glob_segment = start;
      }

    }
  }

ignore_segment:
  HLS_TEST_UNLOCK ();

  return TRUE;
}

static void
watch_pipeline (InsanityGstPipelineTest * ptest)
{
  guint n;
  GstElement *e;

  gboolean error = FALSE;
  InsanityTest *test = INSANITY_TEST (ptest);
  static const char *const sink_names[] = { "asink", "vsink" };

  /* Look for sinks and add probes */
  glob_nsinks = 0;
  for (n = 0; n < G_N_ELEMENTS (sink_names); n++) {
    e = gst_bin_get_by_name (GST_BIN (glob_pipeline), sink_names[n]);
    if (e) {
      gboolean ok = insanity_gst_test_add_data_probe (INSANITY_GST_TEST (ptest),
          GST_BIN (glob_pipeline), sink_names[n], "sink",
          &glob_sinks[glob_nsinks], &glob_probes[glob_nsinks],
          &probe, NULL, NULL);
      if (ok) {
        glob_nsinks++;
        insanity_test_printf (test, "Probe attached\n");
      } else {
        insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
            "install-probes", FALSE, "Failed to attach probe to fakesink");
        insanity_test_printf (test, "Failed to attach probe to fakesink\n");
        error = TRUE;
      }
      gst_object_unref (e);
    }
  }

  if (!error)
    insanity_test_validate_checklist_item (test, "install-probes",
        glob_nsinks > 0, NULL);

  if (glob_nsinks == 0) {
    insanity_test_done (test);
    return;
  }
}


static gboolean
hls_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_BUFFERING:
    {
      gint per;

      gst_message_parse_buffering (msg, &per);

      /* First buffering happend properly, this is requiered to be able to
       * start seeking */
      if (G_UNLIKELY (glob_buffered == FALSE)) {
        if (per == 100) {
          insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
              "done-buffering", TRUE, NULL);
          glob_buffered = TRUE;

          if (glob_buffering_timeout != 0) {
            g_source_remove (glob_buffering_timeout);
            glob_buffering_timeout = 0;
          }
        } else {
          glob_buffering_timeout = g_timeout_add (250,
              (GSourceFunc) buffering_timeout, INSANITY_TEST (ptest));
        }
      }

      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (glob_pipeline)) {
        const char *validate_checklist_item = glob_validate_on_playing;
        GstState oldstate, newstate, pending;

        gst_message_parse_state_changed (msg, &oldstate, &newstate, &pending);
        if (newstate == GST_STATE_PAUSED && oldstate == GST_STATE_READY) {
          GstIterator *it;
          gboolean queried;
          InsanityTest *test = INSANITY_TEST (ptest);
          GstQuery *query = gst_query_new_latency ();
          const gchar *step_message = "Could not query seeking\n";

          if ((queried = gst_element_query (glob_pipeline, query))) {
            gst_query_parse_latency (query, &glob_is_live, NULL, NULL);
            step_message = NULL;
          } else
            insanity_test_printf (test, "Could not query\n");

          insanity_gst_pipeline_test_set_live (ptest, glob_is_live);
          insanity_test_validate_checklist_item (test, "queried-live", queried,
              step_message);
          gst_query_unref (query);

          step_message = "Could not query seekable\n";
          query = gst_query_new_seeking (GST_FORMAT_TIME);
          if ((queried = gst_element_query (glob_pipeline, query))) {
            gst_query_parse_seeking (query, NULL, &glob_is_seekable, NULL,
                NULL);
            step_message = NULL;
          } else
            insanity_test_printf (test, "Could not query\n");

          insanity_test_validate_checklist_item (test, "queried-seekable",
              queried, step_message);
          gst_query_unref (query);

          /* Iterate over the bins to find a hlsdemux */
          it = gst_bin_iterate_recurse (GST_BIN (glob_pipeline));
          glob_hlsdemux = gst_iterator_find_custom (it,
              (GCompareFunc) find_hlsdemux, NULL);
          gst_iterator_free (it);

          if (glob_hlsdemux != NULL) {
            insanity_test_validate_checklist_item (test, "protocol-is-hls",
                TRUE, "HLS protocol in use");

            gst_object_unref (glob_hlsdemux);
          } else {
            insanity_test_validate_checklist_item (test, "protocol-is-hls",
                FALSE, "HLS protocol in use");
            insanity_test_done (test);
          }

          /* Watch pipeline only if seekable */
          if (glob_is_seekable)
            watch_pipeline (ptest);

        } else if (newstate == GST_STATE_PLAYING
            && pending == GST_STATE_VOID_PENDING && validate_checklist_item) {
          glob_validate_on_playing = NULL;
          insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
              validate_checklist_item, TRUE, NULL);
          /* let it run a couple seconds */
          glob_wait_time = hls_test_get_wait_time (INSANITY_TEST (ptest));
          glob_timer_id =
              g_timeout_add (250, (GSourceFunc) & wait_and_end_step,
              INSANITY_TEST (ptest));
        }
      }
      break;
    case GST_MESSAGE_EOS:
      return FALSE;
    default:
      break;
  }

  return TRUE;

}

static gboolean
hls_test_setup (InsanityTest * test)
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

  glob_server = insanity_http_server_new (test);

  started = insanity_http_server_start (glob_server,
      ssl_cert_file, ssl_key_file);

  g_free (ssl_key_file);
  g_free (ssl_cert_file);

  return started;
}

static void
hls_test_teardown (InsanityTest * test)
{
  if (glob_server != NULL) {
    g_object_unref (glob_server);
    glob_server = NULL;
  }
}

static gboolean
hls_test_start (InsanityTest * test)
{
  GValue uri = { 0 };
  const char *protocol;
  gchar *playlist, *hlsuri, *source_folder, *folder_uri;
  SoupServer *ssl_server;
  guint port, ssl_port;

  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (g_value_get_string (&uri), "")) {
    insanity_test_validate_checklist_item (test, "uri-is-file", FALSE,
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
  folder_uri = g_path_get_dirname (source_folder);
  insanity_http_server_set_source_folder (glob_server, folder_uri);
  g_value_unset (&uri);

  port = insanity_http_server_get_port (glob_server);
  ssl_port = insanity_http_server_get_ssl_port (glob_server);
  ssl_server = insanity_http_server_get_soup_ssl_server (glob_server);
  playlist = g_path_get_basename (source_folder);
  if (ssl_server) {
    hlsuri = g_strdup_printf ("http://127.0.0.1:%u/%s", ssl_port, playlist);
  } else {
    hlsuri = g_strdup_printf ("http://127.0.0.1:%u/%s", port, playlist);
  }
  g_free (source_folder);
  g_object_set (glob_pipeline, "uri", hlsuri, NULL);
  g_free (hlsuri);

  glob_validate_on_playing = NULL;
  glob_done_hls = FALSE;
  glob_duration = GST_CLOCK_TIME_NONE;

  return TRUE;
}

static gboolean
wait_and_start (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;

  /* Try getting duration */
  if (!GST_CLOCK_TIME_IS_VALID (glob_wait_time)) {
    insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_TIME, NULL);
  }

  /* If we have it, start; if not, we'll be called again */
  if (GST_CLOCK_TIME_IS_VALID (glob_wait_time) && glob_is_seekable) {
    glob_wait_time = hls_test_get_wait_time (INSANITY_TEST (ptest));
    glob_timer_id =
        g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, ptest);
    return FALSE;
  }
  return TRUE;
}

static void
hls_test_test (InsanityGstPipelineTest * ptest)
{
  glob_duration_timeout =
      g_timeout_add (5000, (GSourceFunc) & duration_timeout, ptest);
  glob_timer_id = g_timeout_add (250, (GSourceFunc) & wait_and_start, ptest);
}

static void
hls_test_duration (InsanityGstPipelineTest * ptest, GstFormat fmt,
    GstClockTime duration)
{
  gboolean start = FALSE;

  /* If we were waiting on it to start up, do it now */
  if (glob_duration_timeout) {
    g_source_remove (glob_duration_timeout);
    glob_duration_timeout = 0;
    start = TRUE;
  }

  insanity_test_printf (INSANITY_TEST (ptest),
      "Just got notified duration is %" GST_TIME_FORMAT "\n",
      GST_TIME_ARGS (duration));
  glob_duration = duration;
  insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
      "duration-known", TRUE, NULL);

  if (start && glob_is_seekable) {
    /* start now if we were waiting for the duration before doing so */
    glob_wait_time = hls_test_get_wait_time (INSANITY_TEST (ptest));
    glob_timer_id =
        g_timeout_add (250, (GSourceFunc) & wait_and_do_seek, ptest);
  }
}

static gboolean
hls_test_stop (InsanityTest * test)
{
  gint i;
  gboolean segments = TRUE, buffers = TRUE;

  if (glob_timer_id) {
    g_source_remove (glob_timer_id);
    glob_timer_id = 0;
  }

  if (glob_duration_timeout) {
    g_source_remove (glob_duration_timeout);
    glob_duration_timeout = 0;
  }

  if (glob_is_seekable) {
    for (i = 0; i < G_N_ELEMENTS (seek_targets); i++) {
      if (seek_targets[i].seeked == FALSE) {
        insanity_test_printf (test, "Didn't seek at %i%\n",
            seek_targets[i].perc);
        continue;
      }

      if (seek_targets[i].segment_received == FALSE) {
        if (seek_targets[i].perc != 100) {
          segments = FALSE;
          insanity_test_printf (test,
              "No segment received after seeking at %i%\n",
              seek_targets[i].perc);
        }
      }

      if (seek_targets[i].buffer_received == FALSE) {
        if (seek_targets[i].perc != 100) {
          insanity_test_printf (test,
              "No buffer received after seeking at %u%\n",
              seek_targets[i].perc);
          buffers = FALSE;
        }
      }
    }
  }

  insanity_test_validate_checklist_item (test, "segment-seek-time-correct",
      segments, NULL);

  insanity_test_validate_checklist_item (test, "buffer-seek-time-correct",
      buffers, NULL);

  insanity_test_validate_checklist_item (test, "play-in-time",
      glob_play_in_time, NULL);

  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  const gchar *uri, *ssl_cert_file, *ssl_key_file;

  uri = "";
  ssl_cert_file = NULL;
  ssl_key_file = NULL;

  g_type_init ();

  ptest =
      insanity_gst_pipeline_test_new ("hls-test", "Tests HTTP streaming", NULL);
  test = INSANITY_TEST (ptest);

  insanity_test_add_string_argument (test, "uri",
      "The uri to test on (file only)", NULL, FALSE, uri);
  insanity_test_add_string_argument (test, "ssl-cert-file",
      "Certificate file for SSL server", ssl_cert_file, TRUE, NULL);
  insanity_test_add_string_argument (test, "ssl-key-file",
      "Key file for SSL server", NULL, TRUE, ssl_key_file);
  insanity_test_add_checklist_item (test, "uri-is-file",
      "The URI is a file URI", NULL, FALSE);

  insanity_test_add_checklist_item (test, "seek", "A seek succeeded", NULL,
      FALSE);
  insanity_test_add_checklist_item (test, "duration-known",
      "Stream duration could be determined", NULL, FALSE);
  insanity_test_add_checklist_item (test, "protocol-is-hls",
      "The protocol in use is HLS", NULL, FALSE);
  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL, FALSE);
  insanity_test_add_checklist_item (test, "queried-live",
      "The stream is live", NULL, FALSE);
  insanity_test_add_checklist_item (test, "queried-seekable",
      "The stream is seekable", NULL, FALSE);
  insanity_test_add_checklist_item (test, "position-queried",
      "Stream position could be determined", NULL, FALSE);
  insanity_test_add_checklist_item (test, "done-buffering",
      "Got a buffering message", NULL, FALSE);
  insanity_test_add_checklist_item (test, "segment-seek-time-correct",
      "Segments were seen after a seek at or near the expected seek target",
      NULL, FALSE);
  insanity_test_add_checklist_item (test, "buffer-seek-time-correct",
      "Buffers were seen after a seek at or near the expected seek target",
      NULL, FALSE);
  insanity_test_add_checklist_item (test, "play-in-time",
      "Wether the playing time are accurate", NULL, FALSE);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &hls_test_create_pipeline, NULL, NULL);
  g_signal_connect_after (test, "setup", G_CALLBACK (&hls_test_setup), 0);
  g_signal_connect_after (test, "bus-message",
      G_CALLBACK (&hls_test_bus_message), 0);
  g_signal_connect (test, "start", G_CALLBACK (&hls_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&hls_test_stop), 0);
  g_signal_connect (test, "test", G_CALLBACK (&hls_test_test), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&hls_test_teardown), 0);
  g_signal_connect_after (ptest, "duration::time",
      G_CALLBACK (&hls_test_duration), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
