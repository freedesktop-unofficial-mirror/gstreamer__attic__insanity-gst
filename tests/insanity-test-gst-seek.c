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

/* Various bits and pieces taken/adapted from -base/tests/examples/seek/seek.c */

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (10 * GST_SECOND)

/* How far we allow a timestamp to be to match our target */
/* 3 quarters of a second for now. Seeking precision isn't
   very good it seems. Needs to be at the very least one
   frame's worth for low framerate video. */
#define SEEK_THRESHOLD (GST_SECOND * 3 / 4)

/* How much time we allow without receiving any buffer or event
   before deciding the pipeline is wedged. Second precision. */
#define IDLE_TIMEOUT (GST_SECOND*60)

typedef enum
{
  SEEK_TEST_STATE_FIRST,
  SEEK_TEST_STATE_FLUSHING = SEEK_TEST_STATE_FIRST,
  SEEK_TEST_STATE_FLUSHING_KEY,
  SEEK_TEST_STATE_FLUSHING_ACCURATE,
  SEEK_TEST_STATE_FLUSHING_KEY_ACCURATE,
  SEEK_TEST_NUM_STATES
} SeekTestState;

/* Only check timestamps/segment start for non-KEY seeks as for
 * these the segment start will be the position of the previous
 * keyframe. See part-seeking.txt */
#define CHECK_CORRECT_SEGMENT(state) (state == SEEK_TEST_STATE_FLUSHING || state == SEEK_TEST_STATE_FLUSHING_ACCURATE)

/* interesting places to seek to, in percent of the stream duration,
   with negative values being placeholders for randomly chosen locations. */
static int seek_targets[] = {
  0, 20, 50, 99, 100, 150, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1
};

/* Our state. Growing fast. Possibly not locked well enough. */
static GstElement *global_pipeline = NULL;
static GstClockTime global_target = 0;
static gboolean started = FALSE;
static char global_waiting[2] = { 0, 0 };

static unsigned global_nsinks = 0;
static gboolean global_bad_ts = FALSE;
static gboolean global_bad_segment_start = FALSE;
static gboolean global_seek_failed = FALSE;
static gboolean global_bad_segment_clipping = FALSE;
static GstClockTimeDiff global_max_diff = 0;
static SeekTestState global_state = SEEK_TEST_STATE_FIRST;
static int global_seek_target_index = 0;
static GstClockTime global_last_ts[2] =
    { GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE };
static GstPad *global_sinks[2] = { NULL, NULL };
static gulong global_probes[2] = { 0, 0 };

static GstClockTime global_seek_offset = 0;
static GstClockTime global_duration = GST_CLOCK_TIME_NONE;
static gboolean global_expecting_eos = FALSE;
static gint64 global_seek_start_time = 0;
static GstClockTime global_max_seek_time;
static guint global_duration_timeout = 0;
static guint global_idle_timeout = 0;
static gint64 global_last_probe = 0;
static GstSegment global_segment[2];
static gboolean appsink = FALSE;

static GStaticMutex global_mutex = G_STATIC_MUTEX_INIT;
#define SEEK_TEST_LOCK() g_static_mutex_lock (&global_mutex)
#define SEEK_TEST_UNLOCK() g_static_mutex_unlock (&global_mutex)

#define WAIT_STATE_SEGMENT 2
#define WAIT_STATE_BUFFER 1
#define WAIT_STATE_READY 0

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
    g_print ("Requested an appsrc uri but found %s instead!\n", pluginname);
    insanity_test_done (INSANITY_TEST (ptest));
    g_free (uri);
    return;
  }

  insanity_file_appsrc_prepare (appsrc, uri, INSANITY_TEST (ptest));

  g_free (uri);

}

static gboolean
do_seek (InsanityGstPipelineTest * ptest, GstElement * pipeline,
    GstClockTime t0)
{
  GstEvent *event;
  gboolean res;
  GstSeekFlags flags = 0;

  SEEK_TEST_LOCK ();

  /* We'll wait for all sinks to send a buffer */
  memset (global_waiting, WAIT_STATE_SEGMENT, global_nsinks);

  switch (global_state) {
    case SEEK_TEST_STATE_FLUSHING:
      flags = GST_SEEK_FLAG_FLUSH;
      break;
    case SEEK_TEST_STATE_FLUSHING_KEY:
      flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT;
      break;
    case SEEK_TEST_STATE_FLUSHING_ACCURATE:
      flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE;
      break;
    case SEEK_TEST_STATE_FLUSHING_KEY_ACCURATE:
      flags =
          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_ACCURATE;
      break;
    default:
      g_assert (0);
  }

  insanity_test_printf (INSANITY_TEST (ptest),
      "New seek to %" GST_TIME_FORMAT " with method %d\n", GST_TIME_ARGS (t0),
      global_state);
  GST_WARNING ("New seek to %" GST_TIME_FORMAT " with method %d\n",
      GST_TIME_ARGS (t0), global_state);
  insanity_test_ping (INSANITY_TEST (ptest));
  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, flags,
      GST_SEEK_TYPE_SET, t0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  global_seek_start_time = g_get_monotonic_time ();
  SEEK_TEST_UNLOCK ();

  res = gst_element_send_event (pipeline, event);
  if (!res) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest), "seek", FALSE,
        "Failed to send seek event");
    global_seek_failed = TRUE;
    return FALSE;
  }
  if (flags & GST_SEEK_FLAG_FLUSH) {
    gst_element_get_state (pipeline, NULL, NULL, SEEK_TIMEOUT);
  }
  global_last_probe = g_get_monotonic_time ();
  return TRUE;
}

static gboolean
do_next_seek (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;
  GValue v = { 0 };
  gboolean next, bounce = FALSE;

  SEEK_TEST_LOCK ();

  if (global_seek_start_time) {
    GstClockTime seek_time =
        gst_util_uint64_scale (g_get_monotonic_time () - global_seek_start_time,
        GST_SECOND, 1000000);
    if (seek_time > global_max_seek_time)
      global_max_seek_time = seek_time;
    global_seek_start_time = 0;
  }

  /* Switch to next target, or next method if we've done them all */
  global_seek_target_index++;
  next = FALSE;
  if (global_seek_target_index ==
      sizeof (seek_targets) / sizeof (seek_targets[0]))
    next = TRUE;

  if (next) {
    /* Switch to 0 with next seeking method */
    global_state++;
    if (global_state == SEEK_TEST_NUM_STATES) {
      insanity_test_printf (INSANITY_TEST (ptest),
          "All seek methods tested, done\n");
      SEEK_TEST_UNLOCK ();
      insanity_test_done (INSANITY_TEST (ptest));
      return FALSE;
    }
    global_seek_target_index = 0;

    insanity_test_get_argument (INSANITY_TEST (ptest), "all-modes-from-ready",
        &v);
    bounce = g_value_get_boolean (&v);
    g_value_unset (&v);

    insanity_test_printf (INSANITY_TEST (ptest),
        "Switching to seek method %d\n", global_state);
  }
  global_target = global_seek_offset +
      gst_util_uint64_scale (global_duration,
      seek_targets[global_seek_target_index], 100);
  /* Note that when seeking to 99%, we may end up just against EOS and thus not
     actually get any buffer for short streams. So we accept EOS for that case
     as well as the >= 100% cases. */
  global_expecting_eos = (seek_targets[global_seek_target_index] >= 99);
  insanity_test_printf (INSANITY_TEST (ptest),
      "Next seek is to %d%%, time %" GST_TIME_FORMAT
      ", method %d, step %d/%u%s\n", seek_targets[global_seek_target_index],
      GST_TIME_ARGS (global_target), global_state, global_seek_target_index + 1,
      (unsigned) (sizeof (seek_targets) / sizeof (seek_targets[0])),
      global_expecting_eos ? ", expecting EOS" : "");
  SEEK_TEST_UNLOCK ();

  if (bounce) {
    insanity_test_printf (INSANITY_TEST (ptest), "Bouncing through READY\n");
    gst_element_set_state (global_pipeline, GST_STATE_READY);
    gst_element_get_state (global_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
    gst_element_get_state (global_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  }

  do_seek (ptest, global_pipeline, global_target);
  return FALSE;
}

static gboolean
seek_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
    /* ignore EOS, we'll want to not quit but continue seeking */
    return FALSE;
  }

  return TRUE;
}

static GstPipeline *
seek_test_create_pipeline (InsanityGstPipelineTest * ptest, gpointer userdata)
{
  GstElement *playbin = NULL;
  GstElement *audiosink;
  GstElement *videosink;

  /* Just try to get the argument, use default if not found */
  insanity_test_get_boolean_argument (INSANITY_TEST (ptest), "appsink",
      &appsink);

  playbin = gst_element_factory_make ("playbin2", "playbin2");
  global_pipeline = playbin;

  if (appsink) {
    audiosink = insanity_fake_appsink_new ("asink", INSANITY_TEST (ptest));
    videosink = insanity_fake_appsink_new ("vsink", INSANITY_TEST (ptest));
  } else {
    audiosink = gst_element_factory_make ("fakesink", "asink");
    videosink = gst_element_factory_make ("fakesink", "vsink");
  }

  if (!playbin || !audiosink || !videosink) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, NULL);
    return NULL;
  }

  g_object_set (playbin, "video-sink", videosink, NULL);
  g_object_set (playbin, "audio-sink", audiosink, NULL);

  g_signal_connect (playbin, "source-setup", (GCallback) found_source, ptest);

  return GST_PIPELINE (playbin);
}

static const char *
get_waiting_string (int w)
{
  switch (w) {
    case WAIT_STATE_SEGMENT:
      return "waiting for segment";
    case WAIT_STATE_BUFFER:
      return "waiting for buffer";
    case WAIT_STATE_READY:
      return "ready";
    default:
      g_assert (0);
      return "UNKNOWN WAIT STATE";
  }
}

static gboolean
probe (InsanityGstTest * ptest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  gboolean changed = FALSE, ready = FALSE;
  unsigned n;
  int index = -1;

  SEEK_TEST_LOCK ();

  for (n = 0; n < global_nsinks; n++) {
    if (global_sinks[n] == pad) {
      index = n;
      break;
    }
  }

  /* Only care about A/V for now */
  if (index < 0) {
    SEEK_TEST_UNLOCK ();
    return TRUE;
  }

  global_last_probe = g_get_monotonic_time ();

  if (GST_IS_BUFFER (object)) {
    GstBuffer *buffer = GST_BUFFER (object);

    /* Should work in both 0.10 and 0.11 */
    GstClockTime ts = GST_BUFFER_TIMESTAMP (buffer);

    insanity_test_printf (INSANITY_TEST (ptest),
        "[%d] Got %s buffer at %" GST_TIME_FORMAT ", %u bytes, %s, target %"
        GST_TIME_FORMAT "\n", index,
        gst_structure_get_name (gst_caps_get_structure (GST_BUFFER_CAPS
                (buffer), 0)), GST_TIME_ARGS (ts), GST_BUFFER_SIZE (buffer),
        get_waiting_string (global_waiting[index]),
        GST_TIME_ARGS (global_target));

    /* drop if we need a segment first, or if we're already done */
    for (n = 0; n < global_nsinks; ++n) {
      if (pad == global_sinks[n]) {
        if (global_waiting[n] == WAIT_STATE_READY) {
          insanity_test_printf (INSANITY_TEST (ptest),
              "[%d] Pad already ready, buffer ignored\n", index);
          SEEK_TEST_UNLOCK ();
          return TRUE;
        } else if (global_waiting[n] == WAIT_STATE_SEGMENT) {
          insanity_test_printf (INSANITY_TEST (ptest),
              "[%d] Need segment, buffer ignored\n", index);
          SEEK_TEST_UNLOCK ();
          return TRUE;
        }
      }
    }

    if (!GST_CLOCK_TIME_IS_VALID (ts)) {
      SEEK_TEST_UNLOCK ();
      return TRUE;
    }

    if (GST_CLOCK_TIME_IS_VALID (ts)) {
      gint64 stime_ts;
      GstClockTimeDiff diff;
      gint64 ts_end, cstart, cstop;

      /* Check if buffer is completely outside the segment */
      ts_end = ts;
      if (GST_BUFFER_DURATION_IS_VALID (buffer))
        ts_end += GST_BUFFER_DURATION (buffer);

      if (!gst_segment_clip (&global_segment[index],
              global_segment[index].format, ts, ts_end, &cstart, &cstop)) {
        char *msg =
            g_strdup_printf ("Got timestamp %" GST_TIME_FORMAT " -- %"
            GST_TIME_FORMAT ", outside configured segment (%" GST_TIME_FORMAT
            " -- %" GST_TIME_FORMAT "), method %d",
            GST_TIME_ARGS (ts), GST_TIME_ARGS (ts_end),
            GST_TIME_ARGS (global_segment[index].start),
            GST_TIME_ARGS (global_segment[index].stop),
            global_state);
        insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
            "segment-clipping", FALSE, msg);
        g_free (msg);
        global_bad_segment_clipping = TRUE;
        SEEK_TEST_UNLOCK ();
        return TRUE;
      }

      if (CHECK_CORRECT_SEGMENT (global_state)) {
        ts = cstart;
        stime_ts =
            gst_segment_to_stream_time (&global_segment[index],
            global_segment[index].format, ts);
        diff = GST_CLOCK_DIFF (stime_ts, global_target);
        if (diff < 0)
          diff = -diff;
        if (diff > global_max_diff)
          global_max_diff = diff;

        if (diff <= SEEK_THRESHOLD) {
          if (global_waiting[index] == WAIT_STATE_BUFFER) {
            insanity_test_printf (INSANITY_TEST (ptest),
                "[%d] target %" GST_TIME_FORMAT ", diff: %" GST_TIME_FORMAT
                " - GOOD\n", index, GST_TIME_ARGS (global_target),
                GST_TIME_ARGS (diff));
            changed = TRUE;
            global_waiting[index] = WAIT_STATE_READY;
          }
        } else {
          if (global_waiting[index] == WAIT_STATE_BUFFER) {
            char *msg =
                g_strdup_printf ("Got timestamp %" GST_TIME_FORMAT
                ", expected around %" GST_TIME_FORMAT ", off by %"
                GST_TIME_FORMAT ", method %d",
                GST_TIME_ARGS (stime_ts), GST_TIME_ARGS (global_target),
                GST_TIME_ARGS (diff), global_state);
            insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
                "buffer-seek-time-correct", FALSE, msg);
            g_free (msg);
            global_bad_ts = TRUE;
            insanity_test_printf (INSANITY_TEST (ptest),
                "[%d] target %" GST_TIME_FORMAT ", diff: %" GST_TIME_FORMAT
                " - BAD\n", index, GST_TIME_ARGS (global_target),
                GST_TIME_ARGS (diff));
            changed = TRUE;
            global_waiting[index] = WAIT_STATE_READY;
          }
        }
      } else {
        if (global_waiting[index] == WAIT_STATE_BUFFER) {
          changed = TRUE;
          global_waiting[index] = WAIT_STATE_READY;
        }
      }
    }
  } else {
    GstEvent *event = GST_EVENT (object);

    insanity_test_printf (INSANITY_TEST (ptest), "[%d] %s event\n", index,
        GST_EVENT_TYPE_NAME (event));
    if (GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT) {
      GstFormat fmt;
      gint64 start, stop, position;
      gdouble rate, applied_rate;
      gboolean update;

      gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,
          &fmt, &start, &stop, &position);
      gst_segment_set_newsegment_full (&global_segment[index], update, rate,
          applied_rate, fmt, start, stop, position);

      /* ignore segment updates */
      if (update)
        goto ignore_segment;

      if (global_waiting[index] != WAIT_STATE_SEGMENT) {
        insanity_test_printf (INSANITY_TEST (ptest),
            "[%d] Got segment starting at %" GST_TIME_FORMAT
            ", but we are not waiting for segment\n", index,
            GST_TIME_ARGS (start));
        goto ignore_segment;
      }

      insanity_test_printf (INSANITY_TEST (ptest),
          "[%d] Got segment starting at %" GST_TIME_FORMAT ", %s\n",
          index, GST_TIME_ARGS (start),
          get_waiting_string (global_waiting[index]));

      /* Only check segment start time against target if we're not expecting EOS,
         as segments will be pushed back in range when seeking off the existing
         range, and that's expected behavior. */
      if (!global_expecting_eos && CHECK_CORRECT_SEGMENT (global_state)) {
        gint64 stime_start;
        GstClockTimeDiff diff;

        stime_start =
            gst_segment_to_stream_time (&global_segment[index],
            global_segment[index].format, start);

        diff = GST_CLOCK_DIFF (stime_start, global_target);
        if (diff < 0)
          diff = -diff;

        if (diff > SEEK_THRESHOLD) {
          char *msg =
              g_strdup_printf ("Got segment start %" GST_TIME_FORMAT
              ", expected around %" GST_TIME_FORMAT ", off by %" GST_TIME_FORMAT
              ", method %d",
              GST_TIME_ARGS (stime_start), GST_TIME_ARGS (global_target),
              GST_TIME_ARGS (diff), global_state);
          insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
              "segment-seek-time-correct", FALSE, msg);
          g_free (msg);
          global_bad_segment_start = TRUE;
        }
      }

      global_waiting[index] = WAIT_STATE_BUFFER;
    } else if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
      insanity_test_printf (INSANITY_TEST (ptest),
          "[%d] Got EOS on sink, we are %s and are %s EOS\n", index,
          get_waiting_string (global_waiting[index]),
          global_expecting_eos ? "expecting" : "NOT expecting");
      if (global_waiting[index] != WAIT_STATE_READY) {
        insanity_test_printf (INSANITY_TEST (ptest),
            "[%d] Got expected EOS, now ready and marking flush needed\n",
            index);
        global_waiting[index] = WAIT_STATE_READY;
        changed = TRUE;
      }
    } else if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
      gst_segment_init (&global_segment[index], GST_FORMAT_UNDEFINED);
    }
  }

ignore_segment:

  /* Seek again when we got a buffer or EOS for all our sinks */
  ready = FALSE;
  if (changed) {
    unsigned n;
    ready = TRUE;
    for (n = 0; n < global_nsinks; ++n) {
      if (global_waiting[n] != WAIT_STATE_READY) {
        ready = FALSE;
        break;
      }
    }
  }

  SEEK_TEST_UNLOCK ();

  if (ready) {
    global_last_probe = 0;
    insanity_test_printf (INSANITY_TEST (ptest),
        "All sinks accounted for, preparing next seek\n");
    g_idle_add ((GSourceFunc) & do_next_seek, ptest);
  }

  return TRUE;
}

static gboolean
seek_test_setup (InsanityTest * test)
{
  GValue v = { 0 };
  unsigned n;
  guint32 seed;
  GRand *prg;

  /* Retrieve seed */
  insanity_test_get_argument (test, "seed", &v);
  seed = g_value_get_uint (&v);
  g_value_unset (&v);

  /* Generate one if zero */
  if (seed == 0) {
    seed = g_random_int ();
    if (seed == 0)              /* we don't really care for bias, we just don't want 0 */
      seed = 1;
  }

  /* save that seed as extra-info */
  g_value_init (&v, G_TYPE_UINT);
  g_value_set_uint (&v, seed);
  insanity_test_set_extra_info (test, "seed", &v);
  g_value_unset (&v);

  /* Generate random seek targets from that seed */
  prg = g_rand_new_with_seed (seed);
  for (n = 0; n < sizeof (seek_targets) / sizeof (seek_targets[0]); n++) {
    if (seek_targets[n] < 0) {
      seek_targets[n] = g_rand_int_range (prg, 0, 100);
    }
  }
  g_rand_free (prg);

  gst_segment_init (&global_segment[0], GST_FORMAT_UNDEFINED);
  gst_segment_init (&global_segment[1], GST_FORMAT_UNDEFINED);

  return TRUE;
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
check_wedged (gpointer data)
{
  InsanityTest *test = data;
  gint64 idle;

  SEEK_TEST_LOCK ();
  idle =
      (global_last_probe <=
      0) ? 0 : 1000 * (g_get_monotonic_time () - global_last_probe);
  if (idle >= IDLE_TIMEOUT) {
    insanity_test_printf (test, "Wedged, kicking\n");
    insanity_test_validate_checklist_item (test, "buffer-seek-time-correct",
        FALSE, "No buffers or events were seen for a while");
    g_idle_add ((GSourceFunc) & do_next_seek, test);
  }
  SEEK_TEST_UNLOCK ();

  return TRUE;
}

static gboolean
seek_test_start (InsanityTest * test)
{
  GValue uri = { 0 };
  int n;

  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!strcmp (g_value_get_string (&uri), "")) {
    insanity_test_validate_checklist_item (test, "valid-pipeline", FALSE,
        "No URI to test on");
    g_value_unset (&uri);
    return FALSE;
  }

  g_object_set (global_pipeline, "uri", g_value_get_string (&uri), NULL);
  g_value_unset (&uri);

  global_bad_ts = FALSE;
  global_bad_segment_start = FALSE;
  global_seek_failed = FALSE;
  global_bad_segment_clipping = FALSE;
  global_max_diff = 0;
  global_target = 0;
  global_state = SEEK_TEST_STATE_FIRST;
  global_seek_target_index = 0;
  global_last_ts[0] = global_last_ts[1] = GST_CLOCK_TIME_NONE;
  for (n = 0; n < 2; ++n) {
    global_probes[n] = 0;
    global_sinks[n] = NULL;
  }
  global_duration = GST_CLOCK_TIME_NONE;
  global_max_seek_time = 0;

  return TRUE;
}

static gboolean
seek_test_reached_initial_state (InsanityThreadedTest * ttest)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (ttest);
  GstElement *e;
  gboolean error = FALSE;
  size_t n;
  static const char *const sink_names[] = { "asink", "vsink" };

  /* Set to PAUSED so we get everything autoplugged */
  gst_element_set_state (global_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (global_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  /* Look for sinks and add probes */
  global_nsinks = 0;
  for (n = 0; n < G_N_ELEMENTS (sink_names); n++) {
    e = gst_bin_get_by_name (GST_BIN (global_pipeline), sink_names[n]);
    if (e) {
      gboolean ok = insanity_gst_test_add_data_probe (INSANITY_GST_TEST (ptest),
          GST_BIN (global_pipeline), sink_names[n], "sink",
          &global_sinks[global_nsinks], &global_probes[global_nsinks],
          &probe, NULL, NULL);
      if (ok) {
        global_nsinks++;
      } else {
        insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
            "install-probes", FALSE, "Failed to attach probe to fakesink");
        error = TRUE;
      }
      gst_object_unref (e);
    }
  }

  if (!error) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ttest),
        "install-probes", global_nsinks > 0, NULL);
  }

  if (global_nsinks == 0) {
    insanity_test_done (INSANITY_TEST (ttest));
    return FALSE;
  }

  memset (global_waiting, WAIT_STATE_READY, global_nsinks);

  /* If we don't have duration yet, ask for it, it will call our signal
     if it can be determined */
  if (insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_TIME, NULL)) {
    /* Belt and braces code from gst-discoverer adapted here, but async
       so we can let insanity test continue initializing properly. */
    GstStateChangeReturn sret;

    /* Some parsers may not even return a rough estimate right away, e.g.
     * because they've only processed a single frame so far, so if we
     * didn't get a duration the first time, spin a bit and try again.
     * Ugly, but still better than making parsers or other elements return
     * completely bogus values. We need some API extensions to solve this
     * better. */
    insanity_test_printf (INSANITY_TEST (ptest),
        "No duration yet, try a bit harder\n");
    sret = gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
      insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
          "duration-known", FALSE,
          "No duration, and failed to switch to PLAYING in hope we might get it then");
      insanity_test_done (INSANITY_TEST (ttest));
      return FALSE;
    }

    /* Start off, claim we're ready, but do not start seeking yet,
       we'll do that when we get a duration callback, or fail on timeout */
    global_duration_timeout =
        g_timeout_add (1000, (GSourceFunc) & duration_timeout, ptest);
    started = TRUE;
    return TRUE;
  } else if (global_duration == GST_CLOCK_TIME_NONE) {
    /* Reset to NONE if the stream is not seekable. Explode then */
    insanity_test_done (INSANITY_TEST (ptest));
    return FALSE;
  }

  /* Start first seek to start */
  gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
  do_seek (ptest, global_pipeline, 0);

  /* and install wedged timeout */
  global_idle_timeout = g_timeout_add (1000, (GSourceFunc) & check_wedged,
      (gpointer) ptest);

  started = TRUE;
  return TRUE;
}

static gboolean
seek_test_stop (InsanityTest * test)
{
  GValue v = { 0 };
  int n;

  SEEK_TEST_LOCK ();
  for (n = 0; n < global_nsinks; n++) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        global_sinks[n], global_probes[n]);
    gst_object_unref (global_sinks[n]);
    global_probes[n] = 0;
    global_sinks[n] = NULL;
  }
  global_nsinks = 0;

  g_value_init (&v, G_TYPE_INT64);
  g_value_set_int64 (&v, global_max_diff);
  insanity_test_set_extra_info (test, "max-seek-error", &v);
  g_value_unset (&v);

  g_value_init (&v, G_TYPE_UINT64);
  g_value_set_uint64 (&v, global_max_seek_time);
  insanity_test_set_extra_info (test, "max-seek-time", &v);
  g_value_unset (&v);

  /* If we've not invalidated these, validate them now */
  if (!global_seek_failed) {
    insanity_test_validate_checklist_item (test, "seek", TRUE, NULL);
  }
  if (!global_bad_ts) {
    insanity_test_validate_checklist_item (test, "buffer-seek-time-correct",
        TRUE, NULL);
  }
  if (!global_bad_segment_start) {
    insanity_test_validate_checklist_item (test, "segment-seek-time-correct",
        TRUE, NULL);
  }
  if (!global_bad_segment_clipping) {
    insanity_test_validate_checklist_item (test, "segment-clipping", TRUE,
        NULL);
  }

  if (appsink) {
    GstElement *audiosink;
    GstElement *videosink;
    g_object_get (global_pipeline, "audio-sink", &audiosink, NULL);
    g_object_get (global_pipeline, "video-sink", &videosink, NULL);
    if (insanity_fake_appsink_get_buffers_received (audiosink) +
        insanity_fake_appsink_get_buffers_received (videosink) > 0) {
      insanity_test_validate_checklist_item (test, "buffers-received",
          TRUE, "Sinks received buffers");
    } else {
      insanity_test_validate_checklist_item (test, "buffers-received",
          FALSE, "Sinks received no buffers");
    }
    gst_object_unref (audiosink);
    gst_object_unref (videosink);
  }

  started = FALSE;

  SEEK_TEST_UNLOCK ();
  return TRUE;
}

static void
seek_test_duration (InsanityGstPipelineTest * ptest, GstFormat fmt,
    GstClockTime duration)
{
  gboolean seek = FALSE;
  GstQuery *q;
  gboolean ret;

  /* If we were waiting on it to start up, do it now */
  if (global_duration_timeout) {
    g_source_remove (global_duration_timeout);
    global_duration_timeout = 0;
    seek = TRUE;
  }

  insanity_test_printf (INSANITY_TEST (ptest),
      "Just got notified duration is %" GST_TIME_FORMAT "\n",
      GST_TIME_ARGS (duration));
  global_duration = duration;
  insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
      "duration-known", TRUE, NULL);

  global_seek_offset = 0;
  q = gst_query_new_seeking (GST_FORMAT_TIME);
  ret = gst_element_query (global_pipeline, q);
  if (ret) {
    GstFormat fmt;
    gboolean seekable;
    gint64 sstart, send;

    gst_query_parse_seeking (q, &fmt, &seekable, &sstart, &send);
    insanity_test_printf (INSANITY_TEST (ptest),
        "Seeking query: %s seekable, %" GST_TIME_FORMAT
        " -- %" GST_TIME_FORMAT "\n", (seekable ? "" : "not"),
        GST_TIME_ARGS (sstart), GST_TIME_ARGS (send));
    if (seekable && fmt == GST_FORMAT_TIME && sstart != -1 && sstart != send) {
      global_seek_offset = sstart;
      if (send != -1)
        global_duration = send - sstart;
      insanity_test_validate_checklist_item (INSANITY_TEST (ptest), "seekable",
          TRUE, NULL);
    } else {
      insanity_test_validate_checklist_item (INSANITY_TEST (ptest), "seekable",
          FALSE, "not seekable");
      global_duration = GST_CLOCK_TIME_NONE;
    }
  } else {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest), "seekable",
        FALSE, "seeking query failed");
    global_duration = GST_CLOCK_TIME_NONE;
  }
  gst_query_unref (q);

  if (global_duration == GST_CLOCK_TIME_NONE) {
    /* Only handle seekable files */
    insanity_test_done (INSANITY_TEST (ptest));
  }

  /* Do first test now if we were waiting to do it */
  if (seek && global_duration != GST_CLOCK_TIME_NONE) {
    insanity_test_printf (INSANITY_TEST (ptest),
        "We can now start seeking, since we have duration\n");
    do_seek (ptest, global_pipeline, 0);
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

  ptest = insanity_gst_pipeline_test_new ("seek-test",
      "Tests various seeking methods", NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, "");
  insanity_test_add_argument (test, "uri", "The file to test seeking on", NULL,
      FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_UINT);
  g_value_set_uint (&vdef, 0);
  insanity_test_add_argument (test, "seed",
      "A random seed to generate random seek targets",
      "0 means a randomly chosen seed; the seed will be saved as extra-info",
      TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vdef, TRUE);
  insanity_test_add_argument (test, "all-modes-from-ready",
      "Whether to bring the pipeline back to READY before testing each new seek mode",
      NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vdef, FALSE);
  insanity_test_add_argument (test, "appsink",
      "Use appsink instead of fakesink", NULL, TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL);
  insanity_test_add_checklist_item (test, "duration-known",
      "Stream duration could be determined", NULL);
  insanity_test_add_checklist_item (test, "seekable",
      "Stream detected as seekable", NULL);
  insanity_test_add_checklist_item (test, "seek",
      "Seek events were accepted by the pipeline", NULL);
  insanity_test_add_checklist_item (test, "buffer-seek-time-correct",
      "Buffers were seen after a seek at or near the expected seek target",
      NULL);
  insanity_test_add_checklist_item (test, "segment-seek-time-correct",
      "Segments were seen after a seek at or near the expected seek target",
      NULL);
  insanity_test_add_checklist_item (test, "segment-clipping",
      "Buffers were correctly clipped to the configured segment", NULL);
  insanity_test_add_checklist_item (test, "buffers-received",
      "Appsinks (if used) received some buffers", NULL);

  insanity_test_add_extra_info (test, "max-seek-error",
      "The maximum timestamp difference between a seek target and the buffer received after the seek (absolute value in nanoseconds)");
  insanity_test_add_extra_info (test, "max-seek-time",
      "The maximum amount of time taken to perform a seek (in nanoseconds)");
  insanity_test_add_extra_info (test, "seed",
      "The seed used to generate random seek targets");

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &seek_test_create_pipeline, NULL, NULL);
  insanity_gst_pipeline_test_set_initial_state (ptest, GST_STATE_PAUSED);

  g_signal_connect_after (test, "bus-message",
      G_CALLBACK (&seek_test_bus_message), 0);
  g_signal_connect_after (test, "setup", G_CALLBACK (&seek_test_setup), 0);
  g_signal_connect (test, "start", G_CALLBACK (&seek_test_start), 0);
  g_signal_connect_after (test, "reached-initial-state",
      G_CALLBACK (&seek_test_reached_initial_state), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&seek_test_stop), 0);
  g_signal_connect_after (ptest, "duration::time",
      G_CALLBACK (&seek_test_duration), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
