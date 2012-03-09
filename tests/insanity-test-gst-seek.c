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

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>

/* Various bits and pieces taken/adapted from -base/tests/examples/seek/seek.c */

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (40 * GST_MSECOND)

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

/* interesting places to seek to, in percent of the stream duration,
   with negative values being placeholders for randomly chosen locations. */
static int seek_targets[] = {
  0, 20, 50, 99, 100, 150, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* Our state. Growing fast. Possibly not locked well enough. */
static GstElement *global_pipeline = NULL;
static GstClockTime global_target = 0;
static gboolean started = FALSE;
static char global_waiting[2] = {0, 0};
static unsigned global_nsinks = 0;
static gboolean global_bad_ts = FALSE;
static gboolean global_bad_segment_start = FALSE;
static gboolean global_seek_failed = FALSE;
static GstClockTimeDiff global_max_diff = 0;
static SeekTestState global_state = SEEK_TEST_STATE_FIRST;
static int global_seek_target_index = 0;
static unsigned int global_seek_step = 0;
static GstClockTime global_last_ts[2] = {GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};
static gboolean global_probes_failed = FALSE;
static GstPad *global_sinks[2] = {NULL, NULL};
static gulong global_probes[2] = {0, 0};
static GstClockTime global_duration = GST_CLOCK_TIME_NONE;
static gboolean global_expecting_eos = FALSE;
static gboolean global_need_flush = FALSE;
static GDateTime *global_seek_start_time = NULL;
static GstClockTime global_max_seek_time;

static GStaticMutex global_mutex = G_STATIC_MUTEX_INIT;
#define SEEK_TEST_LOCK() g_static_mutex_lock (&global_mutex)
#define SEEK_TEST_UNLOCK() g_static_mutex_unlock (&global_mutex)

#define WAIT_STATE_SEGMENT 2
#define WAIT_STATE_BUFFER 1
#define WAIT_STATE_READY 0

static gboolean
do_seek (InsanityGstPipelineTest *ptest, GstElement *pipeline,
    GstClockTime t0, GstClockTime t1)
{
  GstEvent *event;
  gboolean res;
  GstSeekFlags flags = 0;

  SEEK_TEST_LOCK();

  /* We'll wait for all sinks to send a buffer */
  memset(global_waiting, WAIT_STATE_SEGMENT, global_nsinks);

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

  if (global_need_flush) {
    insanity_test_printf (INSANITY_TEST (ptest), "Special flush!\n");
    flags |= GST_SEEK_FLAG_FLUSH;
    global_need_flush = FALSE;
  }

  insanity_test_printf (INSANITY_TEST (ptest),
      "New seek to %"GST_TIME_FORMAT" with method %d\n", GST_TIME_ARGS (t0), global_state);
  GST_WARNING("New seek to %"GST_TIME_FORMAT" with method %d\n", GST_TIME_ARGS (t0), global_state);
  insanity_test_ping (INSANITY_TEST (ptest));
  event = gst_event_new_seek (1.0, GST_FORMAT_TIME, flags,
      GST_SEEK_TYPE_SET, t0, GST_CLOCK_TIME_IS_VALID (t1) ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE, t1);
  global_seek_start_time = g_date_time_new_now_utc ();
  SEEK_TEST_UNLOCK();

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
  GDateTime *now;
  gboolean next;

  SEEK_TEST_LOCK();

  if (global_seek_start_time) {
    GTimeSpan span;
    GstClockTime seek_time;
    now = g_date_time_new_now_utc ();
    span = g_date_time_difference (now, global_seek_start_time);
    g_date_time_unref (now);
    g_date_time_unref (global_seek_start_time);
    global_seek_start_time = NULL;
    seek_time = gst_util_uint64_scale(span, GST_SECOND, 1000000);
    if (seek_time > global_max_seek_time)
      global_max_seek_time = seek_time;
  }

  /* Switch to next target, or next method if we've done them all */
  global_seek_target_index++;
  next = FALSE;
  if (global_seek_target_index == sizeof(seek_targets)/sizeof(seek_targets[0]))
    next = TRUE;
  /* Don't try to seek past the end with a segment seek, as these
     will not give us buffers nor EOS events, only async SEGMENT_DONE
     messages, so we can't really check this is happening correctly. */
  if (global_state == SEEK_TEST_STATE_SEGMENT && seek_targets[global_seek_target_index] >= 100)
    next = TRUE;

  if (next) {
    /* Switch to 0 with next seeking method */
    global_state++;
    if (global_state == SEEK_TEST_NUM_STATES) {
      insanity_test_printf (INSANITY_TEST (ptest), "All seek methods tested, done\n");
      SEEK_TEST_UNLOCK();
      insanity_test_done (INSANITY_TEST (ptest));
      return G_SOURCE_REMOVE;
    }
    global_seek_target_index = 0;
    insanity_test_printf (INSANITY_TEST (ptest), "Switching to seek method %d\n", global_state);
  }
  global_target = gst_util_uint64_scale (global_duration, seek_targets[global_seek_target_index], 100);
  /* Note that when seeking to 99%, we may end up just against EOS and thus not
     actually get any buffer for short streams. So we accept EOS for that case
     as well as the >= 100% cases. */
  global_expecting_eos = (seek_targets[global_seek_target_index] >= 99);
  insanity_test_printf (INSANITY_TEST (ptest), "Next seek is to %d%%, time %"GST_TIME_FORMAT"%s\n",
      seek_targets[global_seek_target_index], GST_TIME_ARGS (global_target),
      global_expecting_eos ? ", expecting EOS" : "");
  SEEK_TEST_UNLOCK();
  do_seek(ptest, global_pipeline, global_target, GST_CLOCK_TIME_NONE);
  return G_SOURCE_REMOVE;
}

static gboolean
seek_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage *msg)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS) {
    /* ignore EOS, we'll want to not quit but continue seeking */
    return FALSE;
  }
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

static const char *
get_waiting_string (int w)
{
  switch (w) {
    case WAIT_STATE_SEGMENT: return "waiting for segment";
    case WAIT_STATE_BUFFER: return "waiting for buffer";
    case WAIT_STATE_READY: return "ready";
    default: g_assert(0); return "UNKNOWN WAIT STATE";
  }
}

static gboolean
probe (GstPad *pad, GstMiniObject *object, gpointer userdata)
{
  InsanityGstPipelineTest *ptest = userdata;
  gboolean changed = FALSE, ready = FALSE;
  unsigned n;

  SEEK_TEST_LOCK();

  if (GST_IS_BUFFER (object)) {
    GstBuffer *buffer = GST_BUFFER (object);

    /* Should work in both 0.10 and 0.11 */
    GstClockTime ts = GST_BUFFER_TIMESTAMP (buffer);
    const GstStructure *s = gst_caps_get_structure (GST_BUFFER_CAPS (buffer), 0);
    int index = -1;
    for (n=0; n<global_nsinks; n++) {
      if (global_sinks[n] == pad) {
        index = n;
        break;
      }
    }

    /* Only care about A/V for now */
    if (index < 0) {
      SEEK_TEST_UNLOCK();
      return TRUE;
    }

    insanity_test_printf (INSANITY_TEST (ptest),
        "[%d] Got %s buffer at %"GST_TIME_FORMAT", %u bytes, %s, target %"GST_TIME_FORMAT"\n",
        index, gst_structure_get_name(gst_caps_get_structure(GST_BUFFER_CAPS (buffer),0)),
        GST_TIME_ARGS (ts), GST_BUFFER_SIZE (buffer),
        get_waiting_string(global_waiting[index]), GST_TIME_ARGS (global_target));

    /* drop if we need a segment first, or if we're already done */
    for (n=0; n<global_nsinks; ++n) {
      if (pad == global_sinks[n]) {
        if (global_waiting[n] == WAIT_STATE_READY) {
          insanity_test_printf (INSANITY_TEST (ptest), "[%d] Pad already ready, buffer ignored\n", index);
          SEEK_TEST_UNLOCK();
          return TRUE;
        }
        else if (global_waiting[n] == WAIT_STATE_SEGMENT) {
          insanity_test_printf (INSANITY_TEST (ptest), "[%d] Need segment, buffer ignored\n", index);
          SEEK_TEST_UNLOCK();
          return TRUE;
        }
      }
    }

    if (!GST_CLOCK_TIME_IS_VALID (ts)) {
      SEEK_TEST_UNLOCK();
      return TRUE;
    }

    if (GST_CLOCK_TIME_IS_VALID (ts)) {
      GstClockTimeDiff diff = GST_CLOCK_DIFF (ts, global_target);
      if (diff < 0)
        diff = -diff;
      if (diff > global_max_diff)
        global_max_diff = diff;

      if (diff <= SEEK_THRESHOLD) {
        if (global_waiting[index] == WAIT_STATE_BUFFER) {
          insanity_test_printf (INSANITY_TEST (ptest),
              "[%d] target %"GST_TIME_FORMAT", diff: %"GST_TIME_FORMAT" - GOOD\n",
              index, GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff));
          changed = TRUE;
          global_waiting[index] = WAIT_STATE_READY;
        }
      }
      else {
        if (global_waiting[index] == WAIT_STATE_BUFFER) {
          char *msg = g_strdup_printf ("Got timestamp %"GST_TIME_FORMAT", expected around %"GST_TIME_FORMAT
              ", off by %"GST_TIME_FORMAT", method %d",
              GST_TIME_ARGS (ts), GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff), global_state);
          insanity_test_validate_step (INSANITY_TEST (ptest), "buffer-seek-time-correct", FALSE, msg);
          g_free (msg);
          global_bad_ts = TRUE;
          insanity_test_printf (INSANITY_TEST (ptest),
              "[%d] target %"GST_TIME_FORMAT", diff: %"GST_TIME_FORMAT" - BAD\n",
              index, GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff));
          changed = TRUE;
          global_waiting[index] = WAIT_STATE_READY;
        }
      }
    }

  }
  else {
    GstEvent *event = GST_EVENT (object);
    if (GST_EVENT_TYPE (event) == GST_EVENT_NEWSEGMENT) {
      unsigned n;
      for (n=0; n<global_nsinks; ++n) {
        if (pad == global_sinks[n] && global_waiting[n] == WAIT_STATE_SEGMENT) {
          gint64 start;

          gst_event_parse_new_segment (event, NULL, NULL, NULL, &start, NULL, NULL);
          insanity_test_printf (INSANITY_TEST (ptest),
             "[%d] Got segment starting at %"GST_TIME_FORMAT", waiting for buffer\n",
             n, GST_TIME_ARGS (start));

          /* Only check segment start time against target if we're not expecting EOS,
             as segments will be pushed back in range when seeking off the existing
             range, and that's expected behavior. */
          if (!global_expecting_eos) {
            GstClockTimeDiff diff = GST_CLOCK_DIFF (start, global_target);
            if (diff < 0)
              diff = -diff;

            if (diff > SEEK_THRESHOLD) {
              char *msg = g_strdup_printf ("Got segment start %"GST_TIME_FORMAT", expected around %"GST_TIME_FORMAT
                  ", off by %"GST_TIME_FORMAT", method %d",
                  GST_TIME_ARGS (start), GST_TIME_ARGS (global_target), GST_TIME_ARGS (diff), global_state);
              insanity_test_validate_step (INSANITY_TEST (ptest), "segment-seek-time-correct", FALSE, msg);
              g_free (msg);
              global_bad_segment_start = TRUE;
            }
          }

          global_waiting[n] = WAIT_STATE_BUFFER;
        }
      }
    }
    else if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
      unsigned n;
      for (n=0; n<global_nsinks; ++n) {
        if (pad == global_sinks[n]) {
          insanity_test_printf (INSANITY_TEST (ptest), "[%d] Got EOS on sink, we are %s and are %s EOS\n",
              n, get_waiting_string (global_waiting[n]),
              global_expecting_eos ? "expecting" : "NOT expecting");
          if (global_waiting[n] != WAIT_STATE_READY) {
            insanity_test_printf (INSANITY_TEST (ptest),
                "[%d] Got expected EOS, now ready and marking flush needed\n", n);
            global_waiting[n] = WAIT_STATE_READY;
            changed = TRUE;
            /* We're at EOS, so we'll need to unwedge next time */
            global_need_flush = TRUE;
          }
        }
      }
    }
  }

  /* Seek again when we got a buffer or EOS for all our sinks */
  ready = FALSE;
  if (changed) {
    unsigned n;
    ready = TRUE;
    for (n=0;n<global_nsinks;++n) {
      if (global_waiting[n] != WAIT_STATE_READY) {
        ready = FALSE;
        break;
      }
    }
  }

  SEEK_TEST_UNLOCK();

  if (ready) {
    insanity_test_printf (INSANITY_TEST (ptest), "All sinks accounted for, preparing next seek\n");
    g_idle_add((GSourceFunc)&do_next_seek, ptest);
  }

  return TRUE;
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
          GstPad *pad = gst_element_get_pad (e, "sink");
          if (pad) {
            global_probes[nsinks] = gst_pad_add_data_probe (pad, (GCallback) &probe, ptest);
            if (global_probes[nsinks] != 0) {
              global_sinks[nsinks] = pad;
              nsinks++;
            }
            else {
              insanity_test_validate_step (INSANITY_TEST (ptest), "install-probes", FALSE,
                  "sink pad not found on fakesink");
              global_probes_failed = TRUE;
              gst_object_unref (pad);
            }
          }
          else {
            insanity_test_validate_step (INSANITY_TEST (ptest), "install-probes", FALSE,
                "sink pad not found on fakesink");
            global_probes_failed = TRUE;
          }
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
  insanity_test_printf (INSANITY_TEST (ptest), "%d sinks setup\n", global_nsinks);
}

static gboolean
seek_test_setup(InsanityTest *test)
{
  GValue v = {0};
  unsigned n;
  guint32 seed;
  GRand *prg;

  /* Retrieve seed */
  insanity_test_get_argument (test, "seed", &v);
  seed = g_value_get_uint(&v);
  g_value_unset (&v);

  /* Generate one if zero */
  seed = g_random_int();
  if (seed == 0) /* we don't really care for bias, we just don't want 0 */
    seed = 1;

  /* save that seed as extra-info */
  g_value_init (&v, G_TYPE_UINT);
  g_value_set_uint (&v, seed);
  insanity_test_set_extra_info (test, "seed", &v);
  g_value_unset (&v);

  /* Generate random seek targets from that seed */
  prg = g_rand_new_with_seed(seed);
  for (n=0; n<sizeof(seek_targets)/sizeof(seek_targets[0]); n++) {
    if (seek_targets[n] < 0) {
      seek_targets[n] = g_rand_int_range(prg, 0, 100);
    }
  }
  g_rand_free (prg);

  return TRUE;
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
  global_bad_segment_start = FALSE;
  global_seek_failed = FALSE;
  global_max_diff = 0;
  global_target = 0;
  global_state = SEEK_TEST_STATE_FIRST;
  global_seek_target_index = 0;
  global_last_ts[0] = global_last_ts[1] = GST_CLOCK_TIME_NONE;
  global_probes_failed = FALSE;
  global_probes[0] = global_probes[1] = 0;
  global_sinks[0] = global_sinks[1] = NULL;
  global_duration = GST_CLOCK_TIME_NONE;
  global_need_flush = FALSE;
  global_max_seek_time = 0;

  /* Set to PAUSED so we get everything autoplugged */
  gst_element_set_state (global_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (global_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  /* Look for sinks and connect to handoff signal */
  connect_sinks (ptest);
  memset(global_waiting, WAIT_STATE_READY, global_nsinks);

  /* If we don't have duration yet, ask for it, it will call our signal
     if it can be determined */
  if (!GST_CLOCK_TIME_IS_VALID (insanity_gst_pipeline_test_query_duration (ptest))) {
    insanity_test_validate_step (test, "duration-known", FALSE, NULL);
    return FALSE;
  }

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
  unsigned n;

  SEEK_TEST_LOCK();
  for (n=0; n<global_nsinks; n++) {
    GstPad *pad = global_sinks[n];
    if (pad) {
      if (global_probes[n] != 0) {
        gst_pad_remove_data_probe (pad, global_probes[n]);
        global_probes[n] = 0;
      }
      gst_object_unref (global_sinks[n]);
      global_sinks[n] = NULL;
    }
  }

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
    insanity_test_validate_step (test, "seek", TRUE, NULL);
  }
  if (!global_bad_ts) {
    insanity_test_validate_step (test, "buffer-seek-time-correct", TRUE, NULL);
  }
  if (!global_bad_segment_start) {
    insanity_test_validate_step (test, "segment-seek-time-correct", TRUE, NULL);
  }
  if (!global_probes_failed) {
    insanity_test_validate_step (test, "install-probes", TRUE, NULL);
  }

  started = FALSE;

  SEEK_TEST_UNLOCK();
  return TRUE;
}

static void
seek_test_duration (InsanityGstPipelineTest *ptest, GstClockTime duration)
{
  insanity_test_printf (INSANITY_TEST (ptest),
      "Just got notified duration is %"GST_TIME_FORMAT"\n", GST_TIME_ARGS (duration));
  global_duration = duration;
  insanity_test_validate_step (INSANITY_TEST (ptest), "duration-known", TRUE, NULL);
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  GValue vdef = {0};

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("seek-test",
      "Tests various seeking methods", NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&vdef, G_TYPE_STRING);
  g_value_set_string (&vdef, "");
  insanity_test_add_argument (test, "uri", "The file to test seeking on", NULL, FALSE, &vdef);
  g_value_unset (&vdef);

  g_value_init (&vdef, G_TYPE_UINT);
  g_value_set_uint (&vdef, 0);
  insanity_test_add_argument (test, "seed", "A random seed to generate random seek targets", "0 means a randomly chosen seed; the seed will be saved as extra-info", TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "install-probes", "Probes were installed on the sinks", NULL);
  insanity_test_add_checklist_item (test, "duration-known", "Stream duration could be determined", NULL);
  insanity_test_add_checklist_item (test, "seek", "Seek events were accepted by the pipeline", NULL);
  insanity_test_add_checklist_item (test, "buffer-seek-time-correct", "Buffers were seen after a seek at or near the expected seek target", NULL);
  insanity_test_add_checklist_item (test, "segment-seek-time-correct", "Segments were seen after a seek at or near the expected seek target", NULL);

  insanity_test_add_extra_info (test, "max-seek-error", "The maximum timestamp difference between a seek target and the buffer received after the seek (absolute value in nanoseconds)");
  insanity_test_add_extra_info (test, "max-seek-time", "The maximum amount of time taken to perform a seek (in nanoseconds)");
  insanity_test_add_extra_info (test, "seed", "The seed used to generate random seek targets");

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &seek_test_create_pipeline, NULL, NULL);
  insanity_gst_pipeline_test_set_initial_state (ptest, GST_STATE_READY);
  g_signal_connect_after (test, "bus-message", G_CALLBACK (&seek_test_bus_message), 0);
  g_signal_connect_after (test, "setup", G_CALLBACK (&seek_test_setup), 0);
  g_signal_connect_after (test, "start", G_CALLBACK (&seek_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&seek_test_stop), 0);
  g_signal_connect_after (ptest, "duration", G_CALLBACK (&seek_test_duration), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
