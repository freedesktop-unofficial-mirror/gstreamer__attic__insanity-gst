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

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>

/* Various bits and pieces taken/adapted from -base/tests/examples/seek/seek.c */

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (40 * GST_MSECOND)

/* Seek to all targets with this step, starting at zero */
/* This needs to be not to small, otherwise we might think a seek
   worked if streaming continues (without seeking) from a previous
   seek to an earlier location, due to buffering. So we need to
   seek far away enough for this not to become an issue. Typically,
   audio only can have small granularity, video only needs a little
   more (though that might depend on the format, and whether it's
   good enough with keyframes), and mixed A/V needs a fair bit. */
#define SEEK_GRANULARITY (30 * GST_SECOND)

/* How far we allow a timestamp to be to match our target */
/* 3 quarters of a second for now. Seeking precision isn't
   very good it seems. Needs to be at the very least one
   frame's worth for low framerate video. */
#define SEEK_THRESHOLD (GST_SECOND * 3 / 4)

typedef enum {
  SEEK_TEST_STATE_FIRST,
  SEEK_TEST_STATE_FLUSHING = SEEK_TEST_STATE_FIRST,
  SEEK_TEST_STATE_SEGMENT,
  SEEK_TEST_STATE_ZERO,
  SEEK_TEST_STATE_KEY,
  SEEK_TEST_STATE_ACCURATE,
  SEEK_TEST_STATE_KEY_ACCURATE,
  SEEK_TEST_NUM_STATES
} SeekTestState;

/* Our state. Nothing locked, some probably needs to. */
static GstElement *global_pipeline = NULL;
GstClockTime global_target = 0;
static gboolean started = FALSE;
static char global_waiting[2] = {1, 1};
static unsigned global_nsinks = 0;
static gboolean global_bad_ts = FALSE;
static gboolean global_seek_failed = FALSE;
static GstClockTimeDiff global_max_diff = 0;
static SeekTestState global_state = SEEK_TEST_STATE_FIRST;
static GstClockTime global_last_ts[2] = {GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};

static gboolean
do_seek (InsanityGstPipelineTest *ptest, GstElement *pipeline,
    GstClockTime t0, GstClockTime t1)
{
  GstEvent *event;
  gboolean res;
  GstSeekFlags flags = 0;

  /* We'll wait for all sinks to send a buffer */
  memset(global_waiting, 1, global_nsinks);

  switch (global_state) {
    case SEEK_TEST_STATE_ZERO:
      flags = 0;
      break;
    case SEEK_TEST_STATE_SEGMENT:
      flags = GST_SEEK_FLAG_SEGMENT;
      break;
    case SEEK_TEST_STATE_FLUSHING:
      flags = GST_SEEK_FLAG_FLUSH;
      break;
    case SEEK_TEST_STATE_KEY:
      flags = GST_SEEK_FLAG_KEY_UNIT;
      break;
    case SEEK_TEST_STATE_ACCURATE:
      flags = GST_SEEK_FLAG_ACCURATE;
      break;
    case SEEK_TEST_STATE_KEY_ACCURATE:
      flags = GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_ACCURATE;
      break;
    default:
      g_assert(0);
  }

  printf("New seek to %"GST_TIME_FORMAT" with method %d\n", GST_TIME_ARGS (t0), global_state);
  GST_WARNING("New seek to %"GST_TIME_FORMAT" with method %d\n", GST_TIME_ARGS (t0), global_state);
  insanity_test_ping (INSANITY_TEST (ptest));
  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, flags, GST_SEEK_TYPE_SET, t0, GST_SEEK_TYPE_SET, t1);
  res = gst_element_send_event (pipeline, event);
  if (!res) {
    insanity_test_validate_step (INSANITY_TEST (ptest), "seek", FALSE, "Failed to send seek event");
    global_seek_failed = TRUE;
    return FALSE;
  }
  if (flags & GST_SEEK_FLAG_FLUSH) {
    gst_element_get_state (pipeline, NULL, NULL, SEEK_TIMEOUT);
  }
  return TRUE;
}

static gboolean
do_next_seek (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;

  global_target += SEEK_GRANULARITY;
  if (global_target >= 50 * GST_SECOND) { // TODO
    /* Switch to 0 with next seeking method */
    global_state++;
    if (global_state == SEEK_TEST_NUM_STATES) {
      printf("All seek methods tested, done\n");
      insanity_test_done (INSANITY_TEST (ptest));
      return G_SOURCE_REMOVE;
    }
    global_target = 0;
    printf("Switching to seek method %d\n", global_state);
  }
  do_seek(ptest, global_pipeline, global_target, GST_CLOCK_TIME_NONE);
  return G_SOURCE_REMOVE;
}

static gboolean
seek_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage *msg)
{
/*
  //printf("MSG %s\n", GST_MESSAGE_TYPE_NAME (msg));
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
    printf("Got EOS\n");
  }
*/
  return TRUE;
}

static GstPipeline*
seek_test_create_pipeline (InsanityGstPipelineTest *ptest, gpointer userdata)
{
  GstElement *pipeline = NULL, *playbin2 = NULL;
  const char *launch_line = "playbin2 name=foo audio-sink=fakesink video-sink=fakesink";
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

static void
handoff (GstElement *e, GstBuffer *buffer, GstPad *pad, gpointer userdata)
{
  InsanityGstPipelineTest *ptest = userdata;
  GstClockTime ts = GST_BUFFER_TIMESTAMP (buffer);
  gboolean ignore = FALSE, changed = FALSE;

  /* Should work in both 0.10 and 0.11 */
  const GstStructure *s = gst_caps_get_structure (GST_BUFFER_CAPS (buffer), 0);
  int audio = s && g_str_has_prefix (gst_structure_get_name (s), "audio/x-raw");
  int video = s && g_str_has_prefix (gst_structure_get_name (s), "video/x-raw");
  int index = global_nsinks == 1 ? 0 : audio ? 0 : video ? 1 : -1;

  /* Only care about A/V for now */
  if (index < 0)
    return;

  printf("Got %s buffer on %s at %"GST_TIME_FORMAT", %u bytes, %s, target %"GST_TIME_FORMAT"\n",
      gst_structure_get_name(gst_caps_get_structure(GST_BUFFER_CAPS (buffer),0)),
      gst_element_get_name (e), GST_TIME_ARGS (ts), GST_BUFFER_SIZE (buffer),
      global_waiting[index]?"waiting":"ready", GST_TIME_ARGS (global_target));

  if (!GST_CLOCK_TIME_IS_VALID (ts))
    return;

  /* Now we have an annoying problem.
     Once we issue a seek, it will be acted upon in the near future,
     but in the meantime we will still get an unknown number of buffers
     from the previously playing segment, so we can't just test for
     timestamps from the next buffer, we need a heuristic to tell
     whether a buffer is from before or after the seek.
     Here, since we know we seek in at least 5 seconds steps, we deem
     a timestamp that is after the previous one by less than 1 second
     to be a leftover. This will fail if we try to seek less than a few
     seconds ahead, if the seeking isn't very precise.
     We keep track of audio and video timestamps separately as they're
     not going to be quite in sync. */
  if (index == 0 || index == 1) {
    GstClockTime last = global_last_ts[index];
    if (!GST_CLOCK_TIME_IS_VALID (last) || (ts >= last && ts < last + GST_SECOND)) {
      ignore = TRUE;
    }
  }
  else {
    ignore = TRUE;
  }
  global_last_ts[index] = ts;

  if (GST_CLOCK_TIME_IS_VALID (ts)) {
    GstClockTimeDiff diff = GST_CLOCK_DIFF (ts, global_target);
    if (diff < 0)
      diff = -diff;
    if (diff > global_max_diff)
      global_max_diff = diff;

    if (diff <= SEEK_THRESHOLD) {
      if (global_waiting[index]) {
        printf("[%d] target %"GST_TIME_FORMAT", diff: %"GST_TIME_FORMAT" - GOOD\n",
            index, GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff));
        changed = TRUE;
        global_waiting[index] = 0;
      }
    }
    else if (!ignore) {
      if (global_waiting[index]) {
        char *msg = g_strdup_printf ("Got timestamp %"GST_TIME_FORMAT", expected around %"GST_TIME_FORMAT
            ", off by %"GST_TIME_FORMAT", method %d",
            GST_TIME_ARGS (ts), GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff), global_state);
        insanity_test_validate_step (INSANITY_TEST (ptest), "seek-time-correct", FALSE, msg);
        g_free (msg);
        global_bad_ts = TRUE;
        printf("[%d] target %"GST_TIME_FORMAT", diff: %"GST_TIME_FORMAT" - BAD\n",
            index, GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff));
        changed = TRUE;
        global_waiting[index] = 0;
      }
    }
    else {
      //printf("ignored\n");
    }
  }

  /* Seek again when we got a buffer for all our sinks */
  if (changed && !memchr(global_waiting, 1, global_nsinks)) {
    printf("All sinks accounted for, preparing next seek\n");
    g_idle_add((GSourceFunc)&do_next_seek, ptest);
  }
}

static void
connect_sinks (InsanityGstPipelineTest *ptest)
{
  GstIterator *it;
  gboolean done = FALSE;
  gpointer data;
  GstElement *e;
  char *name;
  unsigned nsinks = 0;

  it = gst_bin_iterate_recurse (GST_BIN (global_pipeline));
  while (!done) {
    switch (gst_iterator_next (it, &data)) {
      case GST_ITERATOR_OK:
        e = GST_ELEMENT_CAST (data);
        name = gst_element_get_name (e);
        if (g_str_has_prefix (name, "fakesink")) {
          g_signal_connect_after (e, "handoff", G_CALLBACK (&handoff), ptest);
          g_signal_connect_after (e, "preroll-handoff", G_CALLBACK (&handoff), ptest);
          g_object_set (e, "signal-handoffs", TRUE, NULL);
          nsinks++;
        }
        g_free (name);
        gst_object_unref (e);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_DONE:
      default:
        done = TRUE;
        break;
    }
  }
  gst_iterator_free (it);

  global_nsinks = nsinks;
  printf("%d sinks setup\n", global_nsinks);
}

static gboolean
seek_test_start(InsanityTest *test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  GValue uri = {0};

  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (g_value_get_string (&uri), "")) {
    insanity_test_validate_step (test, "valid-pipeline", FALSE, "No URI to test on");
    g_value_unset (&uri);
    return FALSE;
  }

  g_object_set (global_pipeline, "uri", g_value_get_string (&uri), NULL);
  g_value_unset (&uri);

  global_bad_ts = FALSE;
  global_seek_failed = FALSE;
  global_max_diff = 0;
  global_target = 0;
  global_state = SEEK_TEST_STATE_FIRST;
  global_last_ts[0] = global_last_ts[1] = GST_CLOCK_TIME_NONE;

  /* Set to PAUSED so we get everything autoplugged */
  gst_element_set_state (global_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (global_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  /* Look for sinks and connect to handoff signal */
  connect_sinks (ptest);
  memset(global_waiting, 1, global_nsinks);

  /* Start first seek to start */
  gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
  do_seek(ptest, global_pipeline, 0, GST_CLOCK_TIME_NONE);

  started = TRUE;

  return TRUE;
}

static gboolean
seek_test_stop(InsanityTest *test)
{
  GValue v = {0};

  g_value_init (&v, G_TYPE_INT64);
  g_value_set_int64 (&v, global_max_diff);
  insanity_test_set_extra_info (test, "max-seek-error", &v);
  g_value_unset (&v);

  /* If we've not invalidated these, validate them now */
  if (!global_seek_failed) {
    insanity_test_validate_step (test, "seek", TRUE, NULL);
  }
  if (!global_bad_ts) {
    insanity_test_validate_step (test, "seek-time-correct", TRUE, NULL);
  }

  started = FALSE;
  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  GValue empty_string = {0};

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("seek-test",
      "Tests various seeking methods", NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&empty_string, G_TYPE_STRING);
  g_value_set_string (&empty_string, "");
  insanity_test_add_argument (test, "uri", "The file to test seeking on", NULL, FALSE, &empty_string);
  g_value_unset (&empty_string);

  insanity_test_add_checklist_item (test, "seek", "Seek events were accepted by the pipeline", NULL);
  insanity_test_add_checklist_item (test, "seek-time-correct", "Buffers were seen after a seek at or near the expected seek target", NULL);

  insanity_test_add_extra_info (test, "max-seek-error", "The maximum timestamp difference between a seek target and the buffer received after the seek (absolute value in nanoseconds)");

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &seek_test_create_pipeline, NULL, NULL);
  insanity_gst_pipeline_test_set_initial_state (ptest, GST_STATE_READY);
  g_signal_connect_after (test, "bus-message", G_CALLBACK (&seek_test_bus_message), 0);
  g_signal_connect_after (test, "start", G_CALLBACK (&seek_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&seek_test_stop), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
