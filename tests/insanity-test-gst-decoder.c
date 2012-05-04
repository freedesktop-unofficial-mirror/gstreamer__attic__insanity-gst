/**
 * Gstreamer
 *
 * Copyright (c) 2012, Collabora Ltd.
 * Author: Thibault Saunier <thibault.saunier@collabora.com>
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

#include <insanity-gst/insanity-gst.h>
#include "media-descriptor-parser.h"

#define LOG(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "demuxer", INSANITY_LOG_LEVEL_DEBUG, format "\n", ##args)
#define ERROR(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "demuxer", INSANITY_LOG_LEVEL_SPAM, format "\n", ##args)

static GStaticMutex glob_mutex = G_STATIC_MUTEX_INIT;
#define DECODER_TEST_LOCK() g_static_mutex_lock (&glob_mutex)
#define DECODER_TEST_UNLOCK() g_static_mutex_unlock (&glob_mutex)

/* Maximum difference of between the expected duration in the stream and
 * the real position, will let a third ofsecond as various factors can create
 * latency in the position query */
#define POSITION_THRESHOLD (GST_SECOND * 1 / 3)
#define SEEK_THRESHOLD (GST_SECOND * 3 / 4)

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (10 * GST_SECOND)
#define FAST_FORWARD_PLAYING_THRESHOLD (G_USEC_PER_SEC / 3)

/* How much time we allow without receiving any buffer or event
   before deciding the pipeline is wedged. Second precision. */
#define IDLE_TIMEOUT (GST_SECOND*20)
#define WAIT_SEGMENT_TIMEOUT (GST_SECOND*3)

typedef struct
{
  GstElement *decoder;
  GstElement *fakesink;
  GstPad *pad;
  gulong probe_id;
  InsanityTest *test;

} ProbeContext;

typedef enum
{
  TEST_NONE,
  TEST_QUERIES,
  TEST_POSITION,

  /* Start seeking */
  TEST_FAST_FORWARD,            /* Always first seeking test */

  /* Backward */
  TEST_BACKWARD_PLAYBACK,
  TEST_FAST_BACKWARD,           /* Always last seeking test */

} TestInProgress;

/* Global GstElement-s */
static GstElement *glob_src = NULL;
static GstElement *glob_typefinder = NULL;
static GstElement *glob_demuxer = NULL;
static GstElement *glob_decoder = NULL;
static GstElement *glob_pipeline = NULL;
static GstElement *glob_multiqueue = NULL;

static gboolean glob_testing_parser = FALSE;

/* Gloabl fields */

static ProbeContext *glob_prob_ctx = NULL;
static MediaDescriptorParser *glob_parser = NULL;
static GstClockTime glob_playback_duration = GST_CLOCK_TIME_NONE;
static gboolean glob_push_mode = FALSE;
static GstBuffer *glob_parsing_buf = NULL;

static TestInProgress glob_in_progress = TEST_NONE;
static GstSegment glob_last_segment;

/* checking Checking wedge */
static gint64 global_last_probe = 0;
static gint64 global_last_seek = 0;
static guint global_idle_timeout = 0;

/* Use in None and seek modes */
static gboolean glob_waiting_segment = TRUE;

/* Check position test */
static GstClockTime glob_first_pos_point = GST_CLOCK_TIME_NONE;
static GstClockTime glob_expected_pos = GST_CLOCK_TIME_NONE;

/* Seeking, and duration queries tests */
static GstClockTime glob_seekable = FALSE;
static GstClockTime glob_duration = GST_CLOCK_TIME_NONE;

/* First segment test */
static gboolean glob_waiting_first_segment = TRUE;
static GstClockTime glob_last_segment_start_time = GST_CLOCK_TIME_NONE;

/* Seek modes test */
static gdouble glob_seek_rate = 0;
static gboolean glob_seek_got_segment = FALSE;
static GstClockTime glob_seek_segment_seektime = GST_CLOCK_TIME_NONE;
static GstClockTime glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;
static GstClockTime glob_seek_stop_ts = GST_CLOCK_TIME_NONE;

/* Sequence number test */
static guint glob_seqnum = 0;
static gboolean glob_seqnum_found = FALSE;
static gboolean glob_wrong_seqnum = FALSE;

/* Segment clipping test */
static gboolean glob_bad_segment_clipping = FALSE;

static gboolean next_test (InsanityTest * test);

static void
clean_test (InsanityTest * test)
{
  glob_demuxer = NULL;
  glob_pipeline = NULL;
  glob_multiqueue = NULL;
  glob_src = NULL;
  glob_seekable = FALSE;
  glob_duration = GST_CLOCK_TIME_NONE;
  glob_waiting_first_segment = TRUE;
  glob_waiting_segment = TRUE;
  glob_testing_parser = FALSE;
  glob_seek_rate = 0;
  glob_seek_segment_seektime = GST_CLOCK_TIME_NONE;
  glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;

  glob_in_progress = TEST_NONE;

  if (glob_prob_ctx != NULL) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        glob_prob_ctx->pad, glob_prob_ctx->probe_id);

    g_slice_free (ProbeContext, glob_prob_ctx);
    glob_prob_ctx = NULL;
  }

  if (glob_parser != NULL) {
    g_object_unref (glob_parser);
    glob_parser = NULL;
  }


  global_idle_timeout = 0;
  global_last_probe = 0;
  global_last_seek = 0;
  glob_expected_pos = GST_CLOCK_TIME_NONE;
  glob_first_pos_point = GST_CLOCK_TIME_NONE;

  glob_seqnum = 0;
  glob_seqnum_found = FALSE;
  glob_wrong_seqnum = FALSE;

  glob_bad_segment_clipping = FALSE;

}

static const gchar *
test_get_name (TestInProgress in_progress)
{
  switch (in_progress) {
    case TEST_NONE:
      return "None";
    case TEST_QUERIES:
      return "Queries";
    case TEST_POSITION:
      return "Postion";
    case TEST_FAST_FORWARD:
      return "Fast forward";
    case TEST_BACKWARD_PLAYBACK:
      return "Backward playback";
    case TEST_FAST_BACKWARD:
      return "Fast backward";
  }

  return NULL;
}

static void
test_position (InsanityTest * test, GstBuffer * buf)
{
  GstQuery *query;
  GstClockTimeDiff diff;

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf) == FALSE)
    return;

  if (GST_CLOCK_TIME_IS_VALID (glob_first_pos_point) == FALSE) {
    glob_first_pos_point = gst_segment_to_stream_time (&glob_last_segment,
        glob_last_segment.format, GST_BUFFER_TIMESTAMP (buf));
  }

  glob_expected_pos = gst_segment_to_stream_time (&glob_last_segment,
      glob_last_segment.format, GST_BUFFER_TIMESTAMP (buf));

  diff = ABS (GST_CLOCK_DIFF (glob_expected_pos, glob_first_pos_point));

  if (diff < glob_playback_duration * GST_SECOND)
    return;

  query = gst_query_new_position (GST_FORMAT_TIME);

  if (gst_element_query (glob_decoder, query)) {
    gint64 pos;
    GstFormat fmt;
    GstClockTimeDiff diff;

    gst_query_parse_position (query, &fmt, &pos);
    diff = ABS (GST_CLOCK_DIFF (glob_expected_pos, pos));

    if (diff <= POSITION_THRESHOLD) {
      insanity_test_validate_checklist_item (test, "position-detection", TRUE,
          NULL);
    } else {
      gchar *validate_msg = g_strdup_printf ("Found position: %" GST_TIME_FORMAT
          " expected: %" GST_TIME_FORMAT, GST_TIME_ARGS (pos),
          GST_TIME_ARGS (glob_expected_pos));

      insanity_test_validate_checklist_item (test, "position-detection",
          FALSE, validate_msg);

      g_free (validate_msg);
    }
  } else {
    LOG (test,
        "%s Does not handle position queries (position-detection \"SKIP\")",
        gst_element_factory_get_longname (gst_element_get_factory
            (glob_demuxer)));
  }

  next_test (test);
}

static void
validate_current_test (InsanityTest * test, gboolean validate,
    const gchar * msg)
{
  switch (glob_in_progress) {
    case TEST_BACKWARD_PLAYBACK:
      insanity_test_validate_checklist_item (test, "backward-playback",
          validate, msg);
      break;
    case TEST_FAST_FORWARD:
      insanity_test_validate_checklist_item (test, "fast-forward",
          validate, msg);
      break;
    case TEST_FAST_BACKWARD:
      glob_seqnum = 0;
      insanity_test_validate_checklist_item (test, "fast-backward",
          validate, msg);
      break;
    default:
      ERROR (test, "Could not validate mode %i", glob_in_progress);
      return;
  }
}

static gboolean
seek_mode_testing (InsanityTest * test)
{
  gboolean res;
  GstEvent *event;
  GstSeekFlags flags = GST_SEEK_FLAG_FLUSH;
  GstSeekType stop_type = GST_SEEK_TYPE_NONE;

  /* Reset global seek props */
  glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;
  glob_seek_stop_ts = GST_CLOCK_TIME_NONE;
  glob_seek_segment_seektime = 0;

  /* Set seeking arguments */
  switch (glob_in_progress) {
    case TEST_BACKWARD_PLAYBACK:
      glob_seek_rate = -1;
      glob_seek_stop_ts = glob_duration;
      stop_type = GST_SEEK_TYPE_SET;
      break;
    case TEST_FAST_FORWARD:
      glob_seek_rate = 2;
      glob_seek_stop_ts = glob_duration / 2;
      break;
    case TEST_FAST_BACKWARD:
      glob_seek_rate = -2;
      glob_seek_stop_ts = glob_duration;
      stop_type = GST_SEEK_TYPE_SET;
      break;
    default:
      return FALSE;
  }

  glob_seek_got_segment = FALSE;
  event = gst_event_new_seek (glob_seek_rate, GST_FORMAT_TIME,
      flags, GST_SEEK_TYPE_SET, glob_seek_segment_seektime,
      stop_type, glob_seek_stop_ts);

  /* We didn't find any event/message with the seqnum we previously set */
  if (glob_seqnum != 0 && glob_seqnum_found == FALSE)
    glob_wrong_seqnum = TRUE;

  glob_seqnum_found = FALSE;
  glob_seqnum = gst_util_seqnum_next ();
  gst_event_set_seqnum (event, glob_seqnum);
  res = gst_element_send_event (glob_pipeline, event);
  global_last_seek = g_get_monotonic_time ();

  if (!res) {
    validate_current_test (test, FALSE, "Could not send seek event");
    glob_seek_rate = 0;
    glob_seqnum = 0;

    /* ... Next test */
    next_test (test);
  }

  return FALSE;
}

static void
test_queries (InsanityTest * test)
{
  GstQuery *query = gst_query_new_seeking (GST_FORMAT_TIME);

  if (gst_element_query (glob_demuxer, query)) {
    GstFormat fmt;
    gboolean seekable, known_seekable;

    gst_query_parse_seeking (query, &fmt, &seekable, NULL, NULL);
    if (glob_parser == NULL) {
      insanity_test_validate_checklist_item (test, "seekable-detection",
          TRUE, "No media-descriptor file, result not verified against it");

      glob_seekable = seekable;
    } else {
      known_seekable = media_descriptor_parser_get_seekable (glob_parser);

      insanity_test_validate_checklist_item (test, "seekable-detection",
          known_seekable == seekable, NULL);
      glob_seekable = known_seekable;
    }
  } else {
    if (glob_parser != NULL)
      glob_seekable = media_descriptor_parser_get_seekable (glob_parser);

    LOG (test,
        "%s Does not handle seeking queries (seekable-detection \"SKIP\")",
        gst_element_factory_get_longname (gst_element_get_factory
            (glob_demuxer)));
  }

  gst_query_unref (query);
  query = gst_query_new_duration (GST_FORMAT_TIME);
  if (gst_element_query (glob_demuxer, query)) {
    GstFormat fmt;
    gchar *validate_msg = NULL;
    gint64 duration;

    if (glob_parser == NULL) {
      gst_query_parse_duration (query, &fmt, &duration);
      validate_msg =
          g_strdup_printf ("Found duration %" GST_TIME_FORMAT
          " No media-descriptor file, result not verified against it",
          GST_TIME_ARGS (duration));
      insanity_test_validate_checklist_item (test, "duration-detection",
          TRUE, validate_msg);

      g_free (validate_msg);

      glob_duration = duration;
    } else {
      glob_duration = media_descriptor_parser_get_duration (glob_parser);
      gst_query_parse_duration (query, &fmt, &duration);

      if (glob_duration != duration) {
        validate_msg =
            g_strdup_printf ("Found time %" GST_TIME_FORMAT "-> %"
            GST_TIME_FORMAT, GST_TIME_ARGS (duration),
            GST_TIME_ARGS (glob_duration));

        insanity_test_validate_checklist_item (test, "duration-detection",
            glob_duration == duration, validate_msg);

        g_free (validate_msg);
      } else {
        insanity_test_validate_checklist_item (test, "duration-detection",
            TRUE, NULL);
      }
    }

  } else {
    if (glob_parser != NULL)
      glob_duration = media_descriptor_parser_get_seekable (glob_parser);

    LOG (test, "%s Does not handle duration queries "
        "(duration-detection \"SKIP\")",
        gst_element_factory_get_longname (gst_element_get_factory
            (glob_demuxer)));
  }

  if (GST_CLOCK_TIME_IS_VALID (glob_duration) &&
      glob_playback_duration > glob_duration) {
    LOG (test, "playback_duration > media duration, setting it"
        "to media_duration != 2");

    glob_playback_duration = glob_duration / 2;
  }
  gst_query_unref (query);


  next_test (test);
}

static const gchar *
pipeline_mode_get_name (gboolean push_mode)
{
  if (push_mode == TRUE)
    return "push";

  return "pull";
}

static void
unvalidate_seeking_tests (InsanityTest * test)
{
  gchar *message = g_strdup_printf ("%s not seekable in %s mode",
      gst_element_factory_get_longname (gst_element_get_factory
          (glob_demuxer)), pipeline_mode_get_name (glob_push_mode));

  insanity_test_validate_checklist_item (test, "fast-forward", TRUE, message);
  insanity_test_validate_checklist_item (test, "fast-backward", TRUE, message);
  insanity_test_validate_checklist_item (test, "backward-playback", TRUE,
      message);

  g_free (message);
}

static gboolean
next_test (InsanityTest * test)
{
  switch (glob_in_progress) {
    case TEST_NONE:
      glob_in_progress = TEST_QUERIES;
      test_queries (test);
      break;
    case TEST_QUERIES:
      glob_in_progress = TEST_POSITION;
      break;
    case TEST_POSITION:
      if (glob_seekable == FALSE) {
        /* Do not enter seek mode tests and finnish the test */
        unvalidate_seeking_tests (test);

        insanity_test_done (test);
        return FALSE;
      }

      glob_in_progress = TEST_BACKWARD_PLAYBACK;
      glob_waiting_segment = TRUE;
      g_timeout_add (1000, (GSourceFunc) & seek_mode_testing, test);
      break;
    case TEST_BACKWARD_PLAYBACK:
      glob_in_progress = TEST_FAST_FORWARD;
      glob_waiting_segment = TRUE;
      g_timeout_add (1000, (GSourceFunc) & seek_mode_testing, test);
      break;
    case TEST_FAST_FORWARD:
      glob_in_progress = TEST_FAST_BACKWARD;
      glob_waiting_segment = TRUE;
      g_timeout_add (1000, (GSourceFunc) & seek_mode_testing, test);
      break;
    default:
      insanity_test_done (test);
      return FALSE;
  }

  LOG (test, "%s in progress", test_get_name (glob_in_progress));

  return FALSE;
}

static gboolean
check_wedged (gpointer data)
{
  InsanityTest *test = data;
  gboolean wedged = FALSE;
  gint64 idle;

  DECODER_TEST_LOCK ();
  idle = (global_last_probe <= 0) ?
      0 : 1000 * (g_get_monotonic_time () - global_last_probe);

  if (idle >= IDLE_TIMEOUT) {
    wedged = TRUE;
    LOG (test, "Nothing probed in too long");
  } else if (glob_waiting_segment == TRUE) {
    idle = (global_last_seek <= 0) ?
        0 : 1000 * (g_get_monotonic_time () - global_last_seek);

    if (idle >= WAIT_SEGMENT_TIMEOUT) {
      LOG (test, "Waited segment for too much time");
      wedged = TRUE;
    }
  }

  if (wedged) {
    LOG (test, "Wedged, kicking");

    switch (glob_in_progress) {
      case TEST_NONE:
        break;
      case TEST_QUERIES:
        insanity_test_validate_checklist_item (test, "seekable-detection",
            FALSE, "No buffers or events were seen for a while");
        insanity_test_validate_checklist_item (test, "duration-detection",
            FALSE, "No buffers or events were seen for a while");
        break;
      case TEST_POSITION:
        insanity_test_validate_checklist_item (test, "position-detection",
            FALSE, "No buffers or events were seen for a while");
      case TEST_FAST_FORWARD:
        insanity_test_validate_checklist_item (test, "fast-forward", FALSE,
            "No buffers or events were seen for a while");
        break;
      case TEST_BACKWARD_PLAYBACK:
        insanity_test_validate_checklist_item (test, "backward-playback", FALSE,
            "No buffers or events were seen for a while");
        break;
      case TEST_FAST_BACKWARD:
        insanity_test_validate_checklist_item (test, "fast-backward", FALSE,
            "No buffers or events were seen for a while");
        break;
    }

    global_last_probe = g_get_monotonic_time ();
    g_idle_add ((GSourceFunc) & next_test, test);
  }

  DECODER_TEST_UNLOCK ();

  return TRUE;
}

/* Pipeline Callbacks */
static gboolean
probe_cb (InsanityGstTest * ptest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  global_last_probe = g_get_monotonic_time ();

  DECODER_TEST_LOCK ();
  if (GST_IS_BUFFER (object)) {
    GstBuffer *buf;
    GstClockTime ts;

    buf = GST_BUFFER (object);
    ts = GST_BUFFER_TIMESTAMP (buf);

    /* First check clipping */
    if (glob_testing_parser == FALSE && GST_CLOCK_TIME_IS_VALID (ts) &&
        glob_waiting_segment == FALSE) {
      gint64 ts_end, cstart, cstop;

      /* Check if buffer is completely outside the segment */
      ts_end = ts;
      if (GST_BUFFER_DURATION_IS_VALID (buf))
        ts_end += GST_BUFFER_DURATION (buf);

      /* Check if buffer is completely outside the segment */
      ts_end = ts;
      if (!gst_segment_clip (&glob_last_segment,
              glob_last_segment.format, ts, ts_end, &cstart, &cstop)) {
        char *msg = g_strdup_printf ("Got timestamp %" GST_TIME_FORMAT " -- %"
            GST_TIME_FORMAT ", outside configured segment (%" GST_TIME_FORMAT
            " -- %" GST_TIME_FORMAT "), method %s",
            GST_TIME_ARGS (ts), GST_TIME_ARGS (ts_end),
            GST_TIME_ARGS (glob_last_segment.start),
            GST_TIME_ARGS (glob_last_segment.stop),
            test_get_name (glob_in_progress));
        insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
            "segment-clipping", FALSE, msg);
        g_free (msg);
        glob_bad_segment_clipping = TRUE;
      }
    }

    switch (glob_in_progress) {
      case TEST_NONE:
        if (glob_waiting_first_segment == TRUE)
          insanity_test_validate_checklist_item (test, "first-segment",
              FALSE, "Got a buffer before the first segment");

        /* Got the first buffer, starting testing dance */
        next_test (test);
        break;
      case TEST_POSITION:
        test_position (test, buf);
        break;
      case TEST_FAST_FORWARD:
      case TEST_BACKWARD_PLAYBACK:
      case TEST_FAST_BACKWARD:
      {
        gint64 stime_ts;

        if (GST_CLOCK_TIME_IS_VALID (ts) == FALSE ||
            glob_waiting_segment == TRUE) {
          break;
        }

        stime_ts = gst_segment_to_stream_time (&glob_last_segment,
            glob_last_segment.format, ts);

        if (GST_CLOCK_TIME_IS_VALID (glob_seek_first_buf_ts) == FALSE) {
          GstClockTime expected_ts =
              gst_segment_to_stream_time (&glob_last_segment,
              glob_last_segment.format,
              glob_seek_rate <
              0 ? glob_seek_stop_ts : glob_seek_segment_seektime);

          GstClockTimeDiff diff = ABS (GST_CLOCK_DIFF (stime_ts, expected_ts));

          if (diff > SEEK_THRESHOLD) {
            gchar *valmsg =
                g_strdup_printf ("Received buffer timestamp %" GST_TIME_FORMAT
                " Seeek wanted %" GST_TIME_FORMAT "",
                GST_TIME_ARGS (stime_ts),
                GST_TIME_ARGS (expected_ts));

            validate_current_test (test, FALSE, valmsg);
            next_test (test);

            g_free (valmsg);
          } else
            glob_seek_first_buf_ts = stime_ts;

        } else {
          GstClockTimeDiff diff =
              GST_CLOCK_DIFF (stime_ts, glob_seek_first_buf_ts);

          if (diff < 0)
            diff = -diff;

          if (diff >= glob_playback_duration * GST_SECOND) {
            validate_current_test (test, TRUE, NULL);
            next_test (test);
          }
        }
        break;
      }
      default:
        break;
    }

  } else if (GST_IS_EVENT (object)) {
    GstEvent *event = GST_EVENT (object);
    guint seqnum = gst_event_get_seqnum (event);

    if (G_LIKELY (glob_seqnum_found == FALSE) && seqnum == glob_seqnum)
      glob_seqnum_found = TRUE;

    if (glob_seqnum_found == TRUE && seqnum != glob_seqnum) {
      gchar *message = g_strdup_printf ("Current seqnum %i != "
          "received %i", glob_seqnum, seqnum);

      insanity_test_validate_checklist_item (test, "seqnum-management",
          FALSE, message);

      glob_wrong_seqnum = TRUE;
      g_free (message);
    }

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_NEWSEGMENT:
      {
        GstFormat fmt;
        gint64 start, stop, position;
        gdouble rate, applied_rate;
        gboolean update;

        gst_event_parse_new_segment_full (event, &update, &rate,
            &applied_rate, &fmt, &start, &stop, &position);
        gst_segment_set_newsegment_full (&glob_last_segment, update, rate,
            applied_rate, fmt, start, stop, position);

        if (glob_waiting_segment == FALSE)
          /* Cache the segment as it will be our reference but don't look
           * further */
          goto done;

        glob_last_segment_start_time = start;
        if (glob_waiting_first_segment == TRUE) {
          insanity_test_validate_checklist_item (test, "first-segment", TRUE,
              NULL);

          glob_waiting_first_segment = FALSE;
        } else if (glob_in_progress >= TEST_FAST_FORWARD &&
            glob_in_progress <= TEST_FAST_BACKWARD) {
          GstClockTimeDiff diff;
          gboolean valid_stop = TRUE;
          GstClockTimeDiff wdiff, rdiff;

          rdiff =
              ABS (GST_CLOCK_DIFF (stop, start)) * ABS (rate * applied_rate);
          wdiff = ABS (GST_CLOCK_DIFF (glob_seek_stop_ts,
                  glob_seek_segment_seektime));

          diff = GST_CLOCK_DIFF (position, glob_seek_segment_seektime);
          if (diff < 0)
            diff = -diff;

          /* Now compare with the expected segment */
          if ((rate * applied_rate) == glob_seek_rate && diff <= SEEK_THRESHOLD
              && valid_stop) {
            glob_seek_got_segment = TRUE;
          } else {
            GstClockTime stopdiff = ABS (GST_CLOCK_DIFF (rdiff, wdiff));

            gchar *validate_msg =
                g_strdup_printf ("Wrong segment received, Rate %f expected "
                "%f, start time diff %" GST_TIME_FORMAT " stop diff %"
                GST_TIME_FORMAT, (rate * applied_rate), glob_seek_rate,
                GST_TIME_ARGS (diff), GST_TIME_ARGS (stopdiff));

            validate_current_test (test, FALSE, validate_msg);
            next_test (test);
            g_free (validate_msg);
          }
        }

        glob_waiting_segment = FALSE;
        break;
      }
      default:
        break;
    }
  }

done:
  DECODER_TEST_UNLOCK ();
  return TRUE;
}

static gboolean
pad_added_cb (GstElement * element, GstPad * new_pad, InsanityTest * test)
{
  GstElement *fakesink;
  GstPadTemplate *mqsinktmpl;
  GstPadLinkReturn linkret;

  GstIterator *it = NULL;
  GstCaps *caps = NULL;
  gboolean ret = TRUE;

  gulong probe_id;
  GstPad *mqsinkpad = NULL, *mqsrcpad = NULL, *ssinkpad = NULL, *decodesinkpad =
      NULL, *decodesrcpad = NULL, *tmppad;

  DECODER_TEST_LOCK ();

  /* First check if the pad caps are compatible with the decoder */
  caps = gst_pad_get_caps (new_pad);
  decodesinkpad = gst_element_get_compatible_pad (glob_decoder, new_pad, caps);

  if (decodesinkpad == NULL)
    goto error;

  mqsinktmpl =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
      (glob_multiqueue), "sink%d");

  if (mqsinktmpl == NULL)
    goto error;

  mqsinkpad = gst_element_request_pad (glob_multiqueue, mqsinktmpl, NULL, NULL);

  it = gst_pad_iterate_internal_links (mqsinkpad);
  if (!it || (gst_iterator_next (it, (gpointer) & mqsrcpad)) != GST_ITERATOR_OK
      || mqsrcpad == NULL) {
    ERROR (test, "Couldn't get srcpad from multiqueue for sinkpad %"
        GST_PTR_FORMAT, mqsinkpad);

    goto error;
  }

  /* Finnish creating and add to bin */
  fakesink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add (GST_BIN (glob_pipeline), fakesink);
  gst_element_sync_state_with_parent (fakesink);
  gst_element_sync_state_with_parent (glob_decoder);

  linkret = gst_pad_link (new_pad, mqsinkpad);
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT,
        new_pad, mqsinkpad);
    goto error;
  }

  /* Link to the decoder */
  linkret = gst_pad_link (mqsrcpad, decodesinkpad);
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT,
        mqsrcpad, decodesinkpad);
    goto error;
  }

  /* Now link to the faksink */
  decodesrcpad = gst_element_get_static_pad (glob_decoder, "src");
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting decoder srcpad");
    goto error;
  }

  ssinkpad = gst_element_get_static_pad (fakesink, "sink");
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting fakesink sinkpad");
    goto error;
  }

  linkret = gst_pad_link (decodesrcpad, ssinkpad);
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT,
        decodesrcpad, ssinkpad);
    goto error;
  }

  /* And install a probe to the decoder src pad */
  if (insanity_gst_test_add_data_probe (INSANITY_GST_TEST (test),
          GST_BIN (glob_pipeline), GST_OBJECT_NAME (glob_decoder),
          GST_ELEMENT_NAME (decodesrcpad), &tmppad, &probe_id,
          &probe_cb, NULL, NULL) == TRUE) {

    glob_prob_ctx = g_slice_new0 (ProbeContext);
    glob_prob_ctx->probe_id = probe_id;
    glob_prob_ctx->pad = tmppad;
    glob_prob_ctx->decoder = glob_decoder;
    glob_prob_ctx->fakesink = fakesink;
    glob_prob_ctx->test = test;

    insanity_test_validate_checklist_item (test, "install-probes", TRUE, NULL);
  } else {
    insanity_test_validate_checklist_item (test,
        "install-probes", FALSE, "Failed to attach probe to fakesink");

    /* No reason to keep the test alive if there is a probe we can't add */
    insanity_test_done (test);
    goto error;
  }

  if (glob_parser)
    media_descriptor_parser_add_stream (glob_parser, new_pad);

done:
  DECODER_TEST_UNLOCK ();

  if (it)
    gst_iterator_free (it);

  if (decodesinkpad)
    gst_object_unref (decodesinkpad);

  if (caps)
    gst_caps_unref (caps);

  if (mqsinkpad)
    gst_object_unref (mqsinkpad);

  if (ssinkpad)
    gst_object_unref (ssinkpad);

  return ret;

error:
  ret = FALSE;
  goto done;
}

static void
type_found_cb (GstElement * typefind, guint probability,
    GstCaps * caps, InsanityTest * test)
{
  GList *demuxers = NULL, *capable_demuxers = NULL;
  GstPad *typefsrcpad = NULL;

  typefsrcpad = gst_element_get_static_pad (typefind, "src");

  /* First try to directly link to the decoder */
  if (pad_added_cb (typefind, typefsrcpad, test) == TRUE)
    return;

  /* if we can't find a demuxer that is concidered as good
   * (ie with rank primary, we just don't run the test */
  demuxers = gst_element_factory_list_get_elements
      (GST_ELEMENT_FACTORY_TYPE_DEMUXER, GST_RANK_PRIMARY);

  if (demuxers == NULL) {
    ERROR (test, "Could not find a demuxer concidered as good enough");
    insanity_test_done (test);
    goto done;
  }

  capable_demuxers = gst_element_factory_list_filter (demuxers, caps,
      GST_PAD_SINK, FALSE);

  glob_demuxer = gst_element_factory_create (capable_demuxers->data, "demuxer");
  if (glob_demuxer == NULL) {
    insanity_test_done (test);
    goto done;
  }

  gst_bin_add (GST_BIN (glob_pipeline), glob_demuxer);
  gst_element_link (glob_typefinder, glob_demuxer);
  gst_element_sync_state_with_parent (glob_demuxer);

  g_signal_connect (glob_demuxer, "pad-added", G_CALLBACK (pad_added_cb), test);

done:
  gst_plugin_feature_list_free (demuxers);
  gst_plugin_feature_list_free (capable_demuxers);
}

static gboolean
bus_message_cb (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
    {
      /* If we are waiting for a segment, keep waiting for it */
      if (glob_waiting_segment) {
        LOG (test, "EOS but waiting for a new segment... keep waiting");
        return FALSE;
      }

      LOG (test, "EOS... current %s next test",
          test_get_name (glob_in_progress));
      next_test (test);
      return FALSE;
    }
    default:
      break;
  }

  return TRUE;
}

/* Test Callbacks  and vmethods*/
static GstPipeline *
create_pipeline (InsanityGstPipelineTest * ptest, gpointer unused_data)
{
  gboolean uri_set;
  GstElementFactory *decofactory = NULL;

  GError *err = NULL;
  InsanityTest *test = INSANITY_TEST (ptest);
  gchar *decodername = NULL, *uri = NULL, *location = NULL;
  const gchar *klass;

  DECODER_TEST_LOCK ();
  glob_pipeline = GST_ELEMENT (gst_pipeline_new ("pipeline"));

  /* Create the source */
  insanity_test_get_boolean_argument (test, "push-mode",
      (gboolean *) & glob_push_mode);

  insanity_test_get_string_argument (test, "location", &location);
  if (location == NULL || g_strcmp0 (location, "") == 0) {
    ERROR (test, "Location name not set");
    goto failed;
  }

  uri = gst_filename_to_uri (location, &err);
  if (err != NULL) {
    ERROR (test, "Error creating uri %s", err->message);

    goto failed;
  } else if (glob_push_mode == FALSE) {
    glob_src = gst_element_factory_make ("filesrc", "src");
  } else {
    gchar *tmpuri;

    glob_src = gst_element_factory_make ("pushfilesrc", "src");
    tmpuri = g_strconcat ("push", uri, NULL);
    g_free (uri);

    uri = tmpuri;
  }

  uri_set = gst_uri_handler_set_uri (GST_URI_HANDLER (glob_src), uri);
  if (uri_set == FALSE) {
    goto failed;
  }

  if (!insanity_test_get_string_argument (test, "decoder-name", &decodername) ||
      g_strcmp0 (decodername, "") == 0) {
    ERROR (test, "Decoder name not set");
    goto failed;
  }

  /* ... create the decoder, will not be used until we typefind and
   * plug the demuxer */
  glob_decoder = gst_element_factory_make (decodername, "decoder");
  if (glob_decoder == NULL)
    goto failed;

  /* We check wether the element is a parser or not */
  decofactory = gst_element_get_factory (glob_decoder);
  klass = gst_element_factory_get_klass (decofactory);
  glob_testing_parser = g_strrstr (klass, "Parser") ? TRUE : FALSE;

  if (glob_testing_parser == FALSE && g_strrstr (klass, "Decoder") == NULL) {
    gchar *val_test = g_strdup_printf ("%s not a decoder nor a parser as"
        " neither of \"Decoder\" nor \"parser\" where present in the element"
        " factory klass: %s", decodername, klass);

    insanity_test_validate_checklist_item (test, "testing-decoder-or-parser",
        FALSE, val_test);

    g_free (val_test);
    goto failed;
  } else {
    insanity_test_validate_checklist_item (test, "testing-decoder-or-parser",
        TRUE, NULL);
  }

  /* ... create the typefinder */
  glob_typefinder = gst_element_factory_make ("typefind", "typefind");
  if (glob_typefinder == NULL)
    goto failed;

  g_signal_connect (glob_typefinder, "have-type", G_CALLBACK (type_found_cb),
      test);

  /* And the multiqueue */
  glob_multiqueue = gst_element_factory_make ("multiqueue", "multiqueue");
  g_object_set (glob_multiqueue, "sync-by-running-time", TRUE, NULL);

  gst_bin_add_many (GST_BIN (glob_pipeline), glob_src, glob_typefinder,
      glob_multiqueue, glob_decoder, NULL);

  if (gst_element_link (glob_src, glob_typefinder) == FALSE)
    goto failed;

done:
  DECODER_TEST_UNLOCK ();

  g_free (decodername);
  g_free (uri);
  g_free (location);
  if (err != NULL)
    g_error_free (err);

  return GST_PIPELINE (glob_pipeline);

failed:
  if (glob_pipeline != NULL)
    gst_object_unref (glob_pipeline);
  if (glob_demuxer != NULL)
    gst_object_unref (glob_decoder);
  if (glob_src != NULL)
    gst_object_unref (glob_src);
  if (glob_multiqueue != NULL)
    gst_object_unref (glob_multiqueue);

  glob_pipeline = glob_demuxer = glob_decoder = glob_multiqueue = glob_src =
      NULL;

  goto done;
}

static gboolean
start_cb (InsanityTest * test)
{
  gboolean ret = TRUE;

  GError *err = NULL;
  gchar *xmllocation = NULL, *location = NULL;

  DECODER_TEST_LOCK ();

  insanity_test_get_string_argument (test, "location", &location);
  if (location == NULL || g_strcmp0 (location, "") == 0) {
    ERROR (test, "Location name not set");
    ret = FALSE;
    goto done;
  }

  gst_segment_init (&glob_last_segment, GST_FORMAT_UNDEFINED);
  glob_parsing_buf = gst_buffer_new ();
  xmllocation = g_strconcat (location, ".xml", NULL);
  glob_parser = media_descriptor_parser_new (test, xmllocation, &err);
  if (glob_parser == NULL) {
    LOG (test, "Could not create media descriptor parser: %s not testing it",
        err->message);
    goto done;
  }

done:
  insanity_test_get_uint64_argument (test, "playback-duration",
      &glob_playback_duration);
  g_free (location);
  g_free (xmllocation);

  DECODER_TEST_UNLOCK ();

  return ret;
}

static void
reached_initial_state_cb (InsanityTest * test)
{
  /* and install wedged timeout */
  global_idle_timeout =
      g_timeout_add (1000, (GSourceFunc) & check_wedged, test);
}

static void
teardown_cb (InsanityTest * test)
{
  clean_test (test);

  gst_buffer_unref (glob_parsing_buf);
}

static gboolean
stop_cb (InsanityTest * test)
{
  if (!glob_wrong_seqnum) {
    insanity_test_validate_checklist_item (test, "seqnum-management", TRUE,
        NULL);
  }

  if (glob_bad_segment_clipping == FALSE) {
    if (glob_testing_parser == TRUE)
      LOG (test, "Testing a parser, Didn't check \"segment-clipping\"");
    else
      insanity_test_validate_checklist_item (test, "segment-clipping", TRUE,
          NULL);
  }

  /* We clean everything as the pipeline is rebuilt at each
   * iteration of start/stop*/
  clean_test (test);

  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;

  const gchar *location = NULL;
  const gchar *decoder_name = NULL;

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("stream-switch-test", "Tests stream "
      "switching inside playbin2", NULL);
  test = INSANITY_TEST (ptest);
  insanity_gst_pipeline_test_set_create_pipeline_in_start (ptest, TRUE);

  /* Arguments */
  insanity_test_add_string_argument (test, "location",
      "The location to test on", NULL, FALSE, location);
  insanity_test_add_string_argument (test, "decoder-name",
      "The decoder element name to test", NULL, FALSE, decoder_name);
  insanity_test_add_boolean_argument (test, "push-mode",
      "Whether the pipeline should run in push mode or not (pull mode)",
      NULL, FALSE, FALSE);
  insanity_test_add_uint64_argument (test, "playback-duration",
      "Stream time to playback for before seeking, in seconds", NULL, TRUE, 2);

  /* Checklist */
  insanity_test_add_checklist_item (test, "testing-decoder-or-parser",
      "Whether the element we are testing (referenced with \"decoder-name\""
      " is a decoder or a parser and thus can be tested here", NULL);
  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL);
  insanity_test_add_checklist_item (test, "seekable-detection",
      "The demuxer detects if a stream is seekable or not", NULL);
  insanity_test_add_checklist_item (test, "duration-detection",
      "The demuxer detects duration of the stream properly", NULL);

  insanity_test_add_checklist_item (test, "position-detection",
      "The demuxer detects the position in the stream properly", NULL);

  insanity_test_add_checklist_item (test, "segment-clipping",
      "Buffers were correctly clipped to the configured segment", NULL);
  insanity_test_add_checklist_item (test, "first-segment", "The demuxer sends a"
      " first segment with proper values before " "first buffers", NULL);
  insanity_test_add_checklist_item (test, "seqnum-management", "The events"
      "we receive have the seqnum it should have", NULL);
  insanity_test_add_checklist_item (test, "fast-forward", "The demuxer could "
      " properly play the stream fast-forward" "first buffers", NULL);
  insanity_test_add_checklist_item (test, "fast-backward", "The demuxer could "
      " properly play the stream fast-backward" "first buffers", NULL);
  insanity_test_add_checklist_item (test, "backward-playback",
      "The demuxer could " " properly play the stream backward" "first buffers",
      NULL);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &create_pipeline, NULL, NULL);

  g_signal_connect (test, "start", G_CALLBACK (&start_cb), NULL);
  g_signal_connect_after (test, "stop", G_CALLBACK (&stop_cb), NULL);
  g_signal_connect_after (test, "bus-message", G_CALLBACK (&bus_message_cb), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&teardown_cb), NULL);
  g_signal_connect_after (test, "reached-initial-state",
      G_CALLBACK (&reached_initial_state_cb), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
