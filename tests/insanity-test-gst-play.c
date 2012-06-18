/* Insanity QA system

 Copyright (c) 2012, Collabora Ltd <vincent@collabora.co.uk>

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>
#include "insanity-file-appsrc.h"
#include "insanity-fake-appsink.h"

#define LOG(format, args...) \
  INSANITY_LOG (INSANITY_TEST (ptest), "play", INSANITY_LOG_LEVEL_DEBUG, format, ##args)

static GstElement *global_pipeline = NULL;
static guint check_position_id = 0;
static GstClockTime first_position = GST_CLOCK_TIME_NONE;
static GstClockTime last_position = GST_CLOCK_TIME_NONE;
static GstClockTime playback_duration = GST_CLOCK_TIME_NONE;
static gboolean appsink = FALSE, progressive_download = FALSE;

static void
found_source (GstElement * playbin, GstElement * appsrc, gpointer ptest)
{

  gchar *uri;
  const gchar *pluginname;
  GstElementFactory *factory;

  g_object_get (global_pipeline, "uri", &uri, NULL);

  if (!g_str_has_prefix (uri, "appsrc")) {
    g_free (uri);
    return;
  }

  factory = gst_element_get_factory (appsrc);

  pluginname = gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory));

  if (!g_str_equal (pluginname, "appsrc")) {
    /* Oops - something went very wrong! */
    LOG ("Requested an appsrc uri but found %s instead!\n", pluginname);
    insanity_test_done (INSANITY_TEST (ptest));
    g_free (uri);
    return;
  }

  insanity_file_appsrc_prepare (appsrc, uri, INSANITY_TEST (ptest));

  g_free (uri);

}

static GstPipeline *
play_gst_test_create_pipeline (InsanityGstPipelineTest * ptest,
    gpointer userdata)
{
  GstElement *playbin;
  GstElement *audiosink;
  GstElement *videosink;
  guint flags;

  /* Just try to get the argument, use default if not found */
  insanity_test_get_boolean_argument (INSANITY_TEST (ptest), "appsink",
      &appsink);

  insanity_test_get_boolean_argument (INSANITY_TEST (ptest),
      "progressive-download", &progressive_download);

  playbin = gst_element_factory_make ("playbin2", "playbin2");
  global_pipeline = playbin;

  if (appsink) {
    audiosink = insanity_fake_appsink_new ("audiosink", INSANITY_TEST (ptest));
    videosink = insanity_fake_appsink_new ("videosink", INSANITY_TEST (ptest));
  } else {
    audiosink = gst_element_factory_make ("fakesink", "audiosink");
    videosink = gst_element_factory_make ("fakesink", "videosink");
  }

  if (!playbin || !audiosink || !videosink) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, NULL);
    return NULL;
  }

  g_print ("Connecting signal\n");

  g_object_set (playbin, "video-sink", videosink, NULL);
  g_object_set (playbin, "audio-sink", audiosink, NULL);

  if (progressive_download) {
    g_object_get (playbin, "flags", &flags, NULL);
    flags |= 0x00000080;
    g_object_set (playbin, "flags", flags, NULL);
  }

  g_signal_connect (playbin, "source-setup", (GCallback) found_source, ptest);

  return GST_PIPELINE (playbin);
}

static gboolean
check_position (InsanityTest * test)
{
  GstFormat fmt;
  gint64 position;

  /* Check if we're changing the position and if we do
   * the test is not dead yet */
  fmt = GST_FORMAT_TIME;
  if (gst_element_query_position (global_pipeline, &fmt, &position) &&
      fmt == GST_FORMAT_TIME && position != -1) {
    if (first_position == GST_CLOCK_TIME_NONE)
      first_position = position;
    last_position = position;

    if (last_position != position) {
      insanity_test_ping (test);
    }

    if (GST_CLOCK_TIME_IS_VALID (playback_duration) &&
        position - first_position >= playback_duration) {
      gst_element_send_event (global_pipeline, gst_event_new_eos ());
    }
  }

  return TRUE;
}

static gboolean
play_test_start (InsanityTest * test)
{
  gchar *uri;

  if (!insanity_test_get_string_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (uri, "")) {
    insanity_test_validate_checklist_item (test, "valid-pipeline", FALSE,
        "No URI to test on");
    return FALSE;
  }

  g_object_set (global_pipeline, "uri", uri, NULL);

  if (!insanity_test_get_uint64_argument (test, "playback-duration",
          &playback_duration))
    return FALSE;

  first_position = GST_CLOCK_TIME_NONE;
  last_position = GST_CLOCK_TIME_NONE;
  check_position_id = g_timeout_add (1000, (GSourceFunc) check_position, test);

  return TRUE;
}

static gboolean
play_test_stop (InsanityTest * test)
{
  if (check_position_id)
    g_source_remove (check_position_id);
  check_position_id = 0;
  if (appsink) {
    GstElement *audiosink;
    GstElement *videosink;
    g_object_get (global_pipeline, "audio-sink", &audiosink, NULL);
    g_object_get (global_pipeline, "video-sink", &videosink, NULL);
    if (insanity_fake_appsink_check_bufcount (audiosink) &&
        insanity_fake_appsink_check_bufcount (videosink)) {
      insanity_test_validate_checklist_item (test, "all-buffers-received",
          TRUE, "All sinks received all their buffers");
    } else {
      insanity_test_validate_checklist_item (test, "all-buffers-received",
          FALSE, "Sinks didn't receive all buffers");
    }
    gst_object_unref (audiosink);
    gst_object_unref (videosink);
  } else {
    insanity_test_validate_checklist_item (test, "all-buffers-received",
        TRUE, "All sinks received all their buffers");
  }
  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityTest *test;
  gboolean ret;
  GValue vdef = { 0 };

  g_type_init ();

  test =
      INSANITY_TEST (insanity_gst_pipeline_test_new ("play-test",
          "Plays a stream throughout", NULL));

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, "");
  insanity_test_add_argument (test, "uri", "The file to test seeking on", NULL,
      FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_UINT64);
  g_value_set_uint64 (&vdef, GST_CLOCK_TIME_NONE);
  insanity_test_add_argument (test, "playback-duration",
      "Stop playback after this many nanoseconds", NULL, FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vdef, FALSE);
  insanity_test_add_argument (test, "appsink",
      "Use appsink instead of fakesink", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vdef, FALSE);
  insanity_test_add_argument (test, "progressive-download",
      "Enable progressive download mode", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "all-buffers-received",
      "Appsinks (if used) received all buffers", NULL, FALSE);

  insanity_gst_pipeline_test_set_create_pipeline_function
      (INSANITY_GST_PIPELINE_TEST (test), &play_gst_test_create_pipeline, NULL,
      NULL);
  g_signal_connect (test, "start", G_CALLBACK (play_test_start), test);
  g_signal_connect_after (test, "stop", G_CALLBACK (play_test_stop), test);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
