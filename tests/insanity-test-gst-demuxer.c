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
#include <gst/pbutils/pbutils.h>

#include "media-descriptor-parser.h"
#include "media-descriptor-writer.h"

#define LOG(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "demuxer", INSANITY_LOG_LEVEL_DEBUG, "In progess %s: " format, \
  test_get_name (glob_in_progress), ##args)
#define ERROR(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "demuxer", INSANITY_LOG_LEVEL_SPAM, "In progess %s: " format, \
  test_get_name (glob_in_progress), ##args)

static GStaticMutex glob_mutex = G_STATIC_MUTEX_INIT;
#define DEMUX_TEST_LOCK() g_static_mutex_lock (&glob_mutex)
#define DEMUX_TEST_UNLOCK() g_static_mutex_unlock (&glob_mutex)

/* How far we allow a timestamp to be to match our target
 * 3 quarters of a second for now. Seeking precision isn't
 * very good it seems. Needs to be at the very least one
 * frame's worth for low framerate video. */
#define SEEK_THRESHOLD (GST_SECOND * 3 / 4)

/* timeout for gst_element_get_state() after a seek */
#define SEEK_TIMEOUT (10 * GST_SECOND)
#define FAST_FORWARD_PLAYING_THRESHOLD (G_USEC_PER_SEC / 3)

/* How much time we allow without receiving any buffer or event
 * before deciding the pipeline is wedged. Second precision. */
#define IDLE_TIMEOUT (GST_SECOND * 20)

typedef struct
{
  GstElement *demuxer;
  GstElement *fakesink;
  GstPad *pad;
  gulong probe_id;
  gboolean unlinked;

  InsanityTest *test;

  GstSegment last_segment;
  gboolean waiting_segment;
  /* First segment test */
  gboolean waiting_first_segment;

} ProbeContext;

typedef enum
{
  TEST_DESCRIPTOR_GENERATION,

  TEST_NONE,
  TEST_QUERIES,
  TEST_POSITION,

  /* Start seeking */
  TEST_FAST_FORWARD,            /* Always first seeking test */

  /* Seeks modes */
  TEST_SEGMENT_SEEK,
  /* Backward */
  TEST_BACKWARD_PLAYBACK,
  TEST_FAST_BACKWARD,           /* Always last seeking test */

  TEST_UNLINK_PAD,              /* Should always be last */

} TestInProgress;

/* Global GstElement-s */
static GstElement *glob_demuxer = NULL;
static GstElement *glob_pipeline = NULL;
static GstElement *glob_multiqueue = NULL;
static GstElement *glob_src = NULL;

/* Gloabl fields */
static guint16 glob_nb_pads = 0;
static ProbeContext *glob_prob_ctxs = NULL;
static MediaDescriptorParser *glob_parser = NULL;
static GstClockTime glob_playback_duration = GST_CLOCK_TIME_NONE;
static gboolean glob_push_mode = FALSE;
static GstBuffer *glob_parsing_buf = NULL;

static TestInProgress glob_in_progress = TEST_NONE;

/* Media descriptor writer context */
static MediaDescriptorWriter *glob_writer = NULL;
static gboolean glob_pipeline_restarted = TRUE;

/* checking Checking wedge */
static gint64 global_last_probe = 0;
static guint global_idle_timeout = 0;

/* Before starting to seek */
static gboolean glob_detecting_frame = FALSE;

/* Seeking, position and duration queries tests */
static GstClockTime glob_seekable = FALSE;
static GstClockTime glob_duration = GST_CLOCK_TIME_NONE;

/* Check position test */
static GstClockTime glob_first_pos_point = GST_CLOCK_TIME_NONE;
static GstPad *glob_position_check_pad = NULL;
static GstClockTime glob_expected_pos = GST_CLOCK_TIME_NONE;

/* Trick modes test */
static gdouble glob_seek_rate = 0;
static GstClockTime glob_seek_segment_seektime = GST_CLOCK_TIME_NONE;
static GstClockTime glob_seek_stop_ts = GST_CLOCK_TIME_NONE;
static GstClockTime glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;

/* Sequence number test */
static guint glob_seqnum = 0;
static gboolean glob_seqnum_found = FALSE;
static gboolean glob_wrong_seqnum = FALSE;

/* Unlink pad testing*/
static gboolean glob_unlinked_pad = FALSE;
static guint glob_unlinked_buf_timeout = 0;
static gboolean glob_buf_on_linked_pad = FALSE;

static void block_pad_cb (GstPad * pad, gboolean blocked, InsanityTest * test);
static gboolean next_test (InsanityTest * test);

static void
clean_test (InsanityTest * test)
{
  gint i;

  glob_demuxer = NULL;
  glob_pipeline = NULL;
  glob_multiqueue = NULL;
  glob_src = NULL;

  glob_playback_duration = GST_CLOCK_TIME_NONE;
  glob_push_mode = FALSE;

  glob_in_progress = TEST_NONE;

  global_last_probe = 0;
  global_idle_timeout = 0;
  glob_detecting_frame = FALSE;
  glob_duration = GST_CLOCK_TIME_NONE;
  glob_seekable = FALSE;

  for (i = 0; i < glob_nb_pads; i++) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        glob_prob_ctxs[i].pad, glob_prob_ctxs[i].probe_id);
  }

  g_free (glob_prob_ctxs);
  glob_prob_ctxs = NULL;

  glob_nb_pads = 0;

  g_clear_object (&glob_parser);
  g_clear_object (&glob_writer);
  glob_pipeline_restarted = TRUE;

  glob_expected_pos = GST_CLOCK_TIME_NONE;
  glob_first_pos_point = GST_CLOCK_TIME_NONE;
  glob_position_check_pad = NULL;

  glob_seek_rate = 0;
  glob_seek_segment_seektime = GST_CLOCK_TIME_NONE;
  glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;

  glob_seqnum = 0;
  glob_seqnum_found = FALSE;
  glob_wrong_seqnum = FALSE;

  glob_unlinked_pad = FALSE;
  glob_unlinked_buf_timeout = 0;
  glob_buf_on_linked_pad = FALSE;
}

/* Utils functions */
static inline const gchar *
test_get_name (TestInProgress in_progress)
{
  switch (in_progress) {
    case TEST_DESCRIPTOR_GENERATION:
      return "Generation media descriptor xml file";
    case TEST_NONE:
      return "None";
    case TEST_QUERIES:
      return "Queries";
    case TEST_POSITION:
      return "Postion";
    case TEST_FAST_FORWARD:
      return "Fast forward";
    case TEST_SEGMENT_SEEK:
      return "Segment seek";
    case TEST_BACKWARD_PLAYBACK:
      return "Backward playback";
    case TEST_FAST_BACKWARD:
      return "Fast backward";
    case TEST_UNLINK_PAD:
      return "Unlink pad";
  }

  return NULL;
}

static inline ProbeContext *
find_probe_context (GstPad * pad)
{
  guint i;

  for (i = 0; i < glob_nb_pads; i++) {
    if (glob_prob_ctxs[i].pad == pad)
      return &glob_prob_ctxs[i];
  }

  return NULL;
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
    case TEST_SEGMENT_SEEK:
      insanity_test_validate_checklist_item (test, "segment-seek",
          validate, msg);
      break;
    case TEST_FAST_BACKWARD:
      glob_seqnum = 0;
      insanity_test_validate_checklist_item (test, "fast-backward",
          validate, msg);
      break;
    case TEST_UNLINK_PAD:
      insanity_test_validate_checklist_item (test, "unlink-pad-handling",
          validate, msg);
      break;
    default:
      ERROR (test, "Could not validate mode %i", glob_in_progress);
      return;
  }
}

static inline const gchar *
pipeline_mode_get_name (gboolean push_mode)
{
  if (push_mode == TRUE)
    return "push";

  return "pull";
}

static gboolean
idle_restart_pipeline (InsanityTest * test)
{
  LOG (test, "Restarting pipeline");
  gst_element_set_state (glob_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (glob_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_element_seek_simple (glob_pipeline, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH, 0);
  gst_element_set_state (glob_pipeline, GST_STATE_PLAYING);

  return FALSE;
}

static void
generate_xml_media_descriptor (InsanityTest * test)
{
  guint i;
  GError *err = NULL;
  GstDiscovererInfo *info = NULL;
  gchar *location = NULL, *suburi = NULL;

  GstDiscoverer *discoverer = gst_discoverer_new (5 * GST_SECOND, NULL);

  insanity_test_get_string_argument (test, "location", &location);

  if (G_UNLIKELY (discoverer == NULL)) {
    ERROR (test, "Error creating discoverer: %s\n", err->message);
    g_clear_error (&err);

    insanity_test_done (test);
    goto done;
  }

  suburi = gst_filename_to_uri (location, &err);
  if (err) {
    ERROR (test, "Could not construct filename");
    g_clear_error (&err);
    goto done;
  }

  info = gst_discoverer_discover_uri (discoverer, suburi, &err);
  if (info == NULL) {
    ERROR (test, "Error discovering: %s\n", err->message);
    g_clear_error (&err);

    insanity_test_done (test);
    goto done;
  }

  glob_duration = gst_discoverer_info_get_duration (info);
  glob_seekable = gst_discoverer_info_get_seekable (info);

  glob_writer = media_descriptor_writer_new (test,
      location, glob_duration, glob_seekable);

  glob_in_progress = TEST_DESCRIPTOR_GENERATION;
  glob_pipeline_restarted = FALSE;

  g_idle_add ((GSourceFunc) idle_restart_pipeline, test);

  for (i = 0; i < glob_nb_pads; i++) {
    media_descriptor_writer_add_stream (glob_writer, glob_prob_ctxs[i].pad);
  }

done:
  g_free (location);
  g_free (suburi);

  if (discoverer != NULL)
    g_object_unref (discoverer);

  if (info != NULL)
    gst_discoverer_info_unref (info);
}

/* Tests functions */
static void
test_unlink_pad (InsanityTest * test)
{
  DEMUX_TEST_LOCK ();
  if (glob_nb_pads < 1) {
    insanity_test_validate_checklist_item (test, "unlink-pad-handling", FALSE,
        "No pad can't unlink");
    next_test (test);
    return;
  }

  gst_pad_set_blocked_async (glob_prob_ctxs[0].pad, TRUE,
      (GstPadBlockCallback) block_pad_cb, test);
  DEMUX_TEST_UNLOCK ();
}

static inline gboolean
is_waiting_first_segment (void)
{
  guint i;

  for (i = 0; i < glob_nb_pads; i++) {
    if (glob_prob_ctxs[i].waiting_first_segment == TRUE) {
      return TRUE;
    }
  }

  return FALSE;
}

static inline gboolean
is_waiting_segment (void)
{
  guint i;

  for (i = 0; i < glob_nb_pads; i++) {
    if (glob_prob_ctxs[i].waiting_segment == TRUE) {
      return TRUE;
    }
  }

  return FALSE;
}

static inline void
set_waiting_segment (void)
{
  guint i;

  for (i = 0; i < glob_nb_pads; i++)
    glob_prob_ctxs[i].waiting_first_segment = TRUE;

}

static void
test_position (InsanityTest * test, GstBuffer * buf, GstPad * pad)
{
  GstQuery *query;
  GstClockTimeDiff diff;
  ProbeContext *probectx;

  /* Make sure to always use the same Segment to convert buffer timestamp to
   * stream time*/
  if (glob_position_check_pad == NULL)
    glob_position_check_pad = pad;
  else if (glob_position_check_pad != pad)
    return;

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf) == FALSE)
    return;

  probectx = find_probe_context (pad);
  if (GST_CLOCK_TIME_IS_VALID (glob_first_pos_point) == FALSE) {
    glob_first_pos_point = gst_segment_to_stream_time (&probectx->last_segment,
        probectx->last_segment.format, GST_BUFFER_TIMESTAMP (buf));
  }

  glob_expected_pos = gst_segment_to_stream_time (&probectx->last_segment,
      probectx->last_segment.format, GST_BUFFER_TIMESTAMP (buf));

  diff = ABS (GST_CLOCK_DIFF (glob_expected_pos, glob_first_pos_point));

  /* We wait the pipeline to run at least glob_playback_duration secs
   * before actually testing the position */
  if (diff < glob_playback_duration * GST_SECOND)
    return;

  query = gst_query_new_position (GST_FORMAT_TIME);
  if (gst_element_query (glob_demuxer, query)) {
    gint64 pos;
    GstFormat fmt;
    gchar *validate_msg = NULL;
    gboolean validate;

    gst_query_parse_position (query, &fmt, &pos);

    validate = (pos == glob_expected_pos);
    if (validate == FALSE) {
      validate_msg = g_strdup_printf ("Found position: %" GST_TIME_FORMAT
          " expected: %" GST_TIME_FORMAT, GST_TIME_ARGS (pos),
          GST_TIME_ARGS (glob_expected_pos));
    }

    insanity_test_validate_checklist_item (test, "position-detection",
        validate, validate_msg);

    g_free (validate_msg);
  } else {
    LOG (test,
        "%s Does not handle position queries (position-detection \"SKIP\")\n",
        gst_element_factory_get_longname (gst_element_get_factory
            (glob_demuxer)));
  }
  gst_query_unref (query);

  next_test (test);
}

static gboolean
test_seek_modes (InsanityTest * test)
{
  gboolean res;
  GstEvent *event;
  GstSeekFlags flags = GST_SEEK_FLAG_FLUSH;
  GstSeekType stop_type = GST_SEEK_TYPE_SET;

  /* Reset global seek props */
  glob_seek_first_buf_ts = GST_CLOCK_TIME_NONE;
  glob_seek_stop_ts = glob_duration;
  glob_seek_segment_seektime = 0;

  /* Set seeking arguments */
  switch (glob_in_progress) {
    case TEST_BACKWARD_PLAYBACK:
      glob_seek_rate = -1;
      break;
    case TEST_SEGMENT_SEEK:
      glob_seek_rate = 1;
      if (glob_duration < 10 * GST_SECOND)
        glob_seek_stop_ts = glob_duration / 2;
      else
        glob_seek_stop_ts = 10 * GST_SECOND;

      flags = GST_SEEK_FLAG_SEGMENT;
      break;
    case TEST_FAST_FORWARD:
      glob_seek_rate = 2;
      if (glob_duration < 10 * GST_SECOND)
        glob_seek_stop_ts = glob_duration / 2;
      else
        glob_seek_stop_ts = 10 * GST_SECOND;
      stop_type = GST_SEEK_TYPE_NONE;
      break;
    case TEST_FAST_BACKWARD:
      glob_seek_rate = -2;
      break;
    default:
      return FALSE;
  }

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
      gchar *validate_msg = NULL;

      validate_msg = g_strdup_printf ("No media-descriptor file, result (%s)"
          "not verified against it", seekable ? "Seekable" : "Not seekable");

      insanity_test_validate_checklist_item (test, "seekable-detection",
          TRUE, validate_msg);

      glob_seekable = seekable;

      g_free (validate_msg);
    } else {
      known_seekable = media_descriptor_parser_get_seekable (glob_parser);

      insanity_test_validate_checklist_item (test, "seekable-detection",
          known_seekable == seekable, NULL);
      glob_seekable = known_seekable;
    }

  } else {
    if (glob_parser != NULL)
      glob_seekable = media_descriptor_parser_get_seekable (glob_parser);

    LOG (test, "%s Does not handle seeking queries (seekable-detection "
        "\"SKIP\")\n", gst_element_factory_get_longname (gst_element_get_factory
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
      validate_msg = g_strdup_printf ("Found duration %" GST_TIME_FORMAT
          " No media-descriptor file, result not verified against it",
          GST_TIME_ARGS (duration));

      insanity_test_validate_checklist_item (test, "duration-detection",
          TRUE, validate_msg);
      glob_duration = duration;

      g_free (validate_msg);
      validate_msg = NULL;
    } else {
      gboolean validate;

      glob_duration = media_descriptor_parser_get_duration (glob_parser);
      gst_query_parse_duration (query, &fmt, &duration);

      if ((validate = (glob_duration == duration)) == FALSE) {
        validate_msg = g_strdup_printf ("Found time %" GST_TIME_FORMAT "-> %"
            GST_TIME_FORMAT, GST_TIME_ARGS (duration),
            GST_TIME_ARGS (glob_duration));
      }

      insanity_test_validate_checklist_item (test, "duration-detection",
          validate, validate_msg);

      g_free (validate_msg);
      validate_msg = NULL;
    }

  } else {
    if (glob_parser != NULL)
      glob_duration = media_descriptor_parser_get_seekable (glob_parser);

    LOG (test, "%s Does not handle duration queries (duration-detection "
        "\"SKIP\")\n", gst_element_factory_get_longname (gst_element_get_factory
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

static gboolean
next_test (InsanityTest * test)
{
  gchar *location = NULL, *xmllocation = NULL;
  GError *err = NULL;

  switch (glob_in_progress) {
    case TEST_DESCRIPTOR_GENERATION:
    {
      insanity_test_get_string_argument (test, "location", &location);
      xmllocation = g_strconcat (location, ".xml", NULL);

      if (media_descriptor_writer_write (glob_writer, xmllocation) == FALSE) {
        ERROR (test, "Could not write media descriptor");
        insanity_test_done (test);
        goto done;
      }

      glob_parser = media_descriptor_parser_new (test, xmllocation, &err);
      if (glob_parser == NULL) {
        ERROR (test, "Could not create media descriptor after writing it");
        insanity_test_done (test);
        goto done;
      }

      /* We reset the test so it starts again from the beginning */
      glob_detecting_frame = TRUE;
      glob_in_progress = TEST_NONE;
      g_idle_add ((GSourceFunc) idle_restart_pipeline, test);

      break;
    }
    case TEST_NONE:
      glob_in_progress = TEST_QUERIES;
      test_queries (test);
      break;
    case TEST_QUERIES:
      glob_in_progress = TEST_POSITION;
      break;
    case TEST_POSITION:
      if (glob_seekable == FALSE) {
        /* Do not enter seek mode tests and jump to the unlink pad test */
        LOG (test, "%s not seekable in %s mode \n",
            gst_element_factory_get_longname (gst_element_get_factory
                (glob_demuxer)), pipeline_mode_get_name (glob_push_mode));

        glob_in_progress = TEST_UNLINK_PAD;
        test_unlink_pad (test);

        return FALSE;
      }
      /* Stop detecting frame as we start seeking */
      glob_detecting_frame = FALSE;
      glob_in_progress = TEST_BACKWARD_PLAYBACK;
      set_waiting_segment ();
      g_timeout_add (100, (GSourceFunc) & test_seek_modes, test);
      break;
    case TEST_BACKWARD_PLAYBACK:
      glob_in_progress = TEST_SEGMENT_SEEK;
      set_waiting_segment ();
      g_timeout_add (100, (GSourceFunc) & test_seek_modes, test);
      break;
    case TEST_SEGMENT_SEEK:
      glob_in_progress = TEST_FAST_FORWARD;
      set_waiting_segment ();
      g_timeout_add (100, (GSourceFunc) & test_seek_modes, test);
      break;
    case TEST_FAST_FORWARD:
      glob_in_progress = TEST_FAST_BACKWARD;
      set_waiting_segment ();
      g_timeout_add (100, (GSourceFunc) & test_seek_modes, test);
      break;
    case TEST_FAST_BACKWARD:
      glob_in_progress = TEST_UNLINK_PAD;
      test_unlink_pad (test);
      break;
    default:
      insanity_test_done (test);
      return FALSE;
  }

  LOG (test, "%s in progress\n", test_get_name (glob_in_progress));

done:
  g_free (location);
  g_free (xmllocation);

  return FALSE;
}

static gboolean
check_wedged (gpointer data)
{
  InsanityTest *test = data;
  gint64 idle;

  idle = (global_last_probe <= 0) ?
      0 : 1000 * (g_get_monotonic_time () - global_last_probe);
  if (idle >= IDLE_TIMEOUT) {
    LOG (test, "Wedged, kicking\n");

    /* Unvalidate tests in progress */
    switch (glob_in_progress) {
      case TEST_DESCRIPTOR_GENERATION:
        /* pass */
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
        break;
      case TEST_FAST_FORWARD:
        insanity_test_validate_checklist_item (test, "fast-forward", FALSE,
            "No buffers or events were seen for a while");
        break;
      case TEST_SEGMENT_SEEK:
        insanity_test_validate_checklist_item (test, "segment-seek", FALSE,
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
      case TEST_UNLINK_PAD:
        insanity_test_validate_checklist_item (test, "unlink-pad-handling",
            FALSE, "No buffers or events were seen for a while");
        break;
    }

    global_last_probe = g_get_monotonic_time ();
    g_idle_add ((GSourceFunc) & next_test, test);
  }

  return TRUE;
}

/* Pipeline Callbacks */
static gboolean
buf_on_unlinked_pad_seen_cb (InsanityTest * test)
{
  if (glob_nb_pads > 1) {
    if (glob_buf_on_linked_pad == TRUE) {
      insanity_test_validate_checklist_item (test, "unlink-pad-handling",
          TRUE, NULL);
      next_test (test);
    } else {
      return TRUE;
    }
  }

  return FALSE;
}

static void
fakesink_handoff_cb (GstElement * fsink, GstBuffer * buf, GstPad * pad,
    InsanityTest * test)
{
  glob_buf_on_linked_pad = TRUE;

  g_object_set (fsink, "signal-handoffs", FALSE, NULL);
  g_signal_handlers_disconnect_by_func (fsink, fakesink_handoff_cb, test);
}

static void
block_pad_cb (GstPad * pad, gboolean blocked, InsanityTest * test)
{
  ProbeContext *probectx;

  probectx = find_probe_context (pad);

  if (blocked == TRUE) {
    GstPad *mqsinkpad, *mqueuesrcpad, *fakesinkpad;
    GstState state;

    mqsinkpad = gst_pad_get_peer (pad);
    gst_pad_unlink (pad, mqsinkpad);
    gst_object_unref (mqsinkpad);

    fakesinkpad = gst_element_get_static_pad (probectx->fakesink, "sink");
    mqueuesrcpad = gst_pad_get_peer (fakesinkpad);
    gst_pad_unlink (mqueuesrcpad, fakesinkpad);
    gst_object_unref (mqueuesrcpad);
    gst_object_unref (fakesinkpad);

    /* Avoid a race where fakesink state changes to NULL and
     * then resynced with the pipeline state before removing it from the
     * pipeline*/
    gst_object_ref (probectx->fakesink);
    gst_bin_remove (GST_BIN (glob_pipeline), probectx->fakesink);

    gst_element_set_state (probectx->fakesink, GST_STATE_NULL);
    if (gst_element_get_state (probectx->fakesink, &state, NULL,
            GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_SUCCESS) {
      gst_object_unref (probectx->fakesink);
      gst_pad_set_blocked_async (probectx->pad, FALSE,
          (GstPadBlockCallback) block_pad_cb, test);

    } else {
      validate_current_test (test, FALSE, "Could not set sink to STATE_NULL");
      next_test (test);
    }
  } else {
    probectx->unlinked = TRUE;
    glob_unlinked_pad = TRUE;

    if (glob_nb_pads > 1) {
      guint i;

      for (i = 0; i < glob_nb_pads; i++) {
        if (glob_prob_ctxs[i].unlinked == FALSE) {
          g_object_set (glob_prob_ctxs[i].fakesink, "signal-handoffs", TRUE,
              NULL);
          g_signal_connect (glob_prob_ctxs[i].fakesink, "handoff",
              G_CALLBACK (fakesink_handoff_cb), test);
          break;
        }
      }

      /*Seek if possible to avoid hitting EOS */
      if (glob_seekable) {
        GstEvent *event;

        event = gst_event_new_seek (1, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE,
            GST_CLOCK_TIME_NONE);
        set_waiting_segment ();
        glob_seqnum_found = FALSE;
        glob_seqnum = gst_util_seqnum_next ();
        gst_event_set_seqnum (event, glob_seqnum);
        gst_element_send_event (glob_pipeline, event);

        /* ret is not really important here */
      }
    }
    /* Else waiting error on the bus */
  }
}

static inline void
media_descriptor_probe (InsanityTest * test, GstPad * pad,
    GstMiniObject * object, gpointer userdata)
{
  if (glob_pipeline_restarted == TRUE) {
    if (GST_IS_BUFFER (object))
      media_descriptor_writer_add_frame (glob_writer, pad, GST_BUFFER (object));
  }
}

static gboolean
probe_cb (InsanityGstTest * ptest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  InsanityTest *test = INSANITY_TEST (ptest);
  ProbeContext *probectx = find_probe_context (pad);

  global_last_probe = g_get_monotonic_time ();

  if (glob_in_progress == TEST_DESCRIPTOR_GENERATION) {
    media_descriptor_probe (test, pad, object, userdata);
    goto done;
  }

  if (GST_IS_BUFFER (object)) {
    GstBuffer *buf = GST_BUFFER (object);
    GstClockTime ts = GST_BUFFER_TIMESTAMP (buf);

    if (glob_detecting_frame == TRUE) {
      if (media_descriptor_parser_add_frame (glob_parser, pad, buf,
              glob_parsing_buf) == FALSE) {

        gchar *message = g_strdup_printf ("expect -> real "
            " offset %" G_GUINT64_FORMAT "-> %" G_GUINT64_FORMAT
            " off_end %" G_GUINT64_FORMAT " -> %" G_GUINT64_FORMAT
            " duration %" GST_TIME_FORMAT " -> %" GST_TIME_FORMAT
            " timestamp %" GST_TIME_FORMAT " -> %" GST_TIME_FORMAT
            " Is Keyframe %i -> %i", GST_BUFFER_OFFSET (glob_parsing_buf),
            GST_BUFFER_OFFSET (buf), GST_BUFFER_OFFSET_END (glob_parsing_buf),
            GST_BUFFER_OFFSET_END (buf),
            GST_TIME_ARGS (GST_BUFFER_DURATION (glob_parsing_buf)),
            GST_TIME_ARGS (GST_BUFFER_DURATION (buf)),
            GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (glob_parsing_buf)),
            GST_TIME_ARGS (ts), GST_BUFFER_FLAG_IS_SET (glob_parsing_buf,
                GST_BUFFER_FLAG_DELTA_UNIT) == FALSE,
            GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT) == FALSE);

        insanity_test_validate_checklist_item (test, "frames-detection", FALSE,
            message);

        g_free (message);
      } else {
        insanity_test_validate_checklist_item (test, "frames-detection", TRUE,
            NULL);
      }
    }

    switch (glob_in_progress) {
      case TEST_NONE:
      {
        if (probectx->waiting_first_segment == TRUE)
          insanity_test_validate_checklist_item (test, "first-segment", FALSE,
              "Got a buffer before the first segment");

        if (is_waiting_first_segment () == FALSE) {
          insanity_test_validate_checklist_item (test, "first-segment", TRUE,
              NULL);
          next_test (test);
        }

        break;
      }
      case TEST_POSITION:
        test_position (test, buf, pad);
        break;
      case TEST_FAST_FORWARD:
      case TEST_BACKWARD_PLAYBACK:
      case TEST_FAST_BACKWARD:
      {
        gint64 stime_ts;

        if (GST_CLOCK_TIME_IS_VALID (ts) == FALSE ||
            is_waiting_segment () == TRUE) {
          break;
        }

        stime_ts = gst_segment_to_stream_time (&probectx->last_segment,
            probectx->last_segment.format, ts);

        if (GST_CLOCK_TIME_IS_VALID (glob_seek_first_buf_ts) == FALSE) {
          GstClockTime expected_ts =
              gst_segment_to_stream_time (&probectx->last_segment,
              probectx->last_segment.format,
              glob_seek_rate <
              0 ? glob_seek_stop_ts : glob_seek_segment_seektime);

          if (stime_ts > expected_ts) {
            gchar *valmsg =
                g_strdup_printf ("Received buffer timestamp %" GST_TIME_FORMAT
                " Seeek wanted %" GST_TIME_FORMAT " Rate: %lf \n",
                GST_TIME_ARGS (stime_ts),
                GST_TIME_ARGS (expected_ts), glob_seek_rate);

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
      case TEST_UNLINK_PAD:
      {
        if (probectx->unlinked == TRUE && glob_unlinked_buf_timeout == 0)
          glob_unlinked_buf_timeout = g_timeout_add (100,
              (GSourceFunc) & buf_on_unlinked_pad_seen_cb, test);
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
      case GST_EVENT_TAG:
      {
        GstTagList *taglist;

        if (glob_parser != NULL) {

          gst_event_parse_tag (event, &taglist);

          insanity_test_validate_checklist_item (test,
              "tag-detection", media_descriptor_parser_add_taglist (glob_parser,
                  taglist), NULL);
        }
        break;
      }
      case GST_EVENT_NEWSEGMENT:
      {
        GstFormat fmt;
        gint64 start, stop, position;
        gdouble rate, applied_rate;
        gboolean update;

        if (glob_seqnum == 0 && glob_seqnum_found == FALSE) {
          /* This should only happen for the first segment */
          glob_seqnum = gst_event_get_seqnum (event);
          glob_seqnum_found = TRUE;
        }

        gst_event_parse_new_segment_full (event, &update, &rate,
            &applied_rate, &fmt, &start, &stop, &position);
        gst_segment_set_newsegment_full (&probectx->last_segment, update, rate,
            applied_rate, fmt, start, stop, position);

        if (probectx->waiting_segment == FALSE)
          /* Cache the segment as it will be our reference but don't look
           * further */
          goto done;

        if (probectx->waiting_first_segment == TRUE) {
          /* Make sure that a new segment has been received for each stream */
          probectx->waiting_first_segment = FALSE;
          probectx->waiting_segment = FALSE;
        } else if (glob_in_progress >= TEST_FAST_FORWARD &&
            glob_in_progress <= TEST_FAST_BACKWARD) {
          GstClockTimeDiff diff;
          GstClockTimeDiff wdiff, rdiff;

          rdiff =
              ABS (GST_CLOCK_DIFF (stop, start)) * ABS (rate * applied_rate);
          wdiff =
              ABS (GST_CLOCK_DIFF (glob_seek_stop_ts,
                  glob_seek_segment_seektime));

          diff = GST_CLOCK_DIFF (position, glob_seek_segment_seektime);

          /* Now compare with the expected segment */
          if (((rate * applied_rate) == glob_seek_rate
                  && position == glob_seek_segment_seektime) == FALSE) {
            GstClockTime stopdiff = ABS (GST_CLOCK_DIFF (rdiff, wdiff));

            gchar *validate_msg =
                g_strdup_printf ("Wrong segment received, Rate %lf expected "
                "%f, start time diff %" GST_TIME_FORMAT " stop diff %"
                GST_TIME_FORMAT, (rate * applied_rate), glob_seek_rate,
                GST_TIME_ARGS (diff), GST_TIME_ARGS (stopdiff));

            validate_current_test (test, FALSE, validate_msg);
            next_test (test);
            g_free (validate_msg);
          }
        }

        probectx->waiting_segment = FALSE;
        break;
      }
      default:
        break;
    }
  }

done:
  return TRUE;
}

static void
pad_added_cb (GstElement * demuxer, GstPad * new_pad,
    InsanityGstPipelineTest * ptest)
{
  GstElement *fakesink;
  GstIterator *it = NULL;
  GstPadTemplate *mqsinktmpl;
  GstPadLinkReturn linkret;

  gulong probe_id;
  GstPad *mqsinkpad = NULL, *mqsrcpad = NULL, *ssinkpad = NULL, *tmppad;

  DEMUX_TEST_LOCK ();
  mqsinktmpl =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
      (glob_multiqueue), "sink%d");

  if (mqsinktmpl == NULL)
    goto done;

  mqsinkpad = gst_element_request_pad (glob_multiqueue, mqsinktmpl, NULL, NULL);

  it = gst_pad_iterate_internal_links (mqsinkpad);
  if (!it || (gst_iterator_next (it, (gpointer) & mqsrcpad)) != GST_ITERATOR_OK
      || mqsrcpad == NULL) {
    ERROR (INSANITY_TEST (ptest), "Couldn't get srcpad from multiqueue for "
        "sinkpad %" GST_PTR_FORMAT, mqsinkpad);

    goto done;
  }

  fakesink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add (GST_BIN (glob_pipeline), fakesink);
  gst_element_sync_state_with_parent (fakesink);

  ssinkpad = gst_element_get_static_pad (fakesink, "sink");
  if (ssinkpad == NULL)
    goto done;

  linkret = gst_pad_link (mqsrcpad, ssinkpad);
  if (linkret != GST_PAD_LINK_OK)
    goto done;

  linkret = gst_pad_link (new_pad, mqsinkpad);
  if (linkret != GST_PAD_LINK_OK)
    goto done;

  if (insanity_gst_test_add_data_probe (INSANITY_GST_TEST (ptest),
          GST_BIN (glob_pipeline), GST_OBJECT_NAME (demuxer),
          GST_ELEMENT_NAME (new_pad), &tmppad, &probe_id,
          &probe_cb, NULL, NULL) == TRUE) {

    glob_prob_ctxs = g_renew (ProbeContext, glob_prob_ctxs, glob_nb_pads + 1);
    glob_prob_ctxs[glob_nb_pads].probe_id = probe_id;
    glob_prob_ctxs[glob_nb_pads].pad = tmppad;
    glob_prob_ctxs[glob_nb_pads].demuxer = demuxer;
    glob_prob_ctxs[glob_nb_pads].fakesink = fakesink;
    glob_prob_ctxs[glob_nb_pads].test = INSANITY_TEST (ptest);
    glob_prob_ctxs[glob_nb_pads].unlinked = FALSE;
    glob_prob_ctxs[glob_nb_pads].waiting_first_segment = TRUE;
    glob_prob_ctxs[glob_nb_pads].waiting_segment = TRUE;
    gst_segment_init (&glob_prob_ctxs[glob_nb_pads].last_segment,
        GST_FORMAT_UNDEFINED);

    glob_nb_pads++;

    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "install-probes", TRUE, NULL);
  } else {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "install-probes", FALSE, "Failed to attach probe to fakesink");

    /* No reason to keep the test alive if there is a probe we can't add */
    insanity_test_done (INSANITY_TEST (ptest));
    goto done;
  }

  if (glob_parser)
    media_descriptor_parser_add_stream (glob_parser, new_pad);
  if (glob_writer)
    media_descriptor_writer_add_stream (glob_writer, new_pad);

done:
  DEMUX_TEST_UNLOCK ();
  if (it)
    gst_iterator_free (it);

  if (mqsinkpad)
    gst_object_unref (mqsinkpad);

  if (ssinkpad)
    gst_object_unref (ssinkpad);
}

static gboolean
bus_message_cb (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:
    {
      if (glob_in_progress == TEST_UNLINK_PAD) {
        if (glob_nb_pads == 1) {
          insanity_test_validate_checklist_item (test, "unlink-pad-handling",
              TRUE, "Only one pad emited error as expected");

          insanity_test_done (test);
          return FALSE;
        } else {
          insanity_test_validate_checklist_item (test, "unlink-pad-handling",
              FALSE, "Only one pad unlinked and crashed, shouldn't happen");
        }
      }
      break;
    }
    case GST_MESSAGE_SEGMENT_DONE:
      if (glob_in_progress == TEST_SEGMENT_SEEK) {
        validate_current_test (test, TRUE, NULL);
        next_test (test);
      }

      break;
    case GST_MESSAGE_STATE_CHANGED:
    {
      if (glob_in_progress == TEST_DESCRIPTOR_GENERATION) {
        if (GST_MESSAGE_SRC (msg) == GST_OBJECT (glob_pipeline)) {
          GstState oldstate, newstate, pending;
          gst_message_parse_state_changed (msg, &oldstate, &newstate, &pending);

          if (newstate == GST_STATE_PLAYING
              && pending == GST_STATE_VOID_PENDING)
            glob_pipeline_restarted = TRUE;
        }
      }
    }
      break;
    case GST_MESSAGE_EOS:
    {
      LOG (test, "Got EOS\n");

      if (glob_in_progress == TEST_SEGMENT_SEEK
          && is_waiting_segment () == FALSE) {
        insanity_test_validate_checklist_item (test, "segment-seek", FALSE,
            "Received an EOS after a segment-seek, shouldn't happen");
        next_test (test);
      } else if (glob_in_progress == TEST_DESCRIPTOR_GENERATION &&
          glob_pipeline_restarted == TRUE)
        next_test (test);

      /* Keep testing anyway */
      return FALSE;
    }
    case GST_MESSAGE_TAG:
      if (glob_in_progress == TEST_DESCRIPTOR_GENERATION) {
        GstTagList *taglist = NULL;

        gst_message_parse_tag (msg, &taglist);
        media_descriptor_writer_add_taglist (glob_writer, taglist);
      }
      break;
    default:
      break;
  }

  return TRUE;
}

/* Test Callbacks  and vmethods*/
static GstPipeline *
demux_test_create_pipeline (InsanityGstPipelineTest * ptest,
    gpointer unused_data)
{

  GError *err = NULL;
  const gchar *klass;
  GstElementFactory *decofactory = NULL;
  gchar *demuxname = NULL, *uri = NULL, *location = NULL;

  InsanityTest *test = INSANITY_TEST (ptest);

  DEMUX_TEST_LOCK ();
  glob_pipeline = GST_ELEMENT (gst_pipeline_new ("pipeline"));

  /* Create the source */
  insanity_test_get_boolean_argument (test, "push-mode", &glob_push_mode);

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

  if (gst_uri_handler_set_uri (GST_URI_HANDLER (glob_src), uri) == FALSE)
    goto failed;

  /* ... create the demuxer */
  if (!insanity_test_get_string_argument (test, "demuxer", &demuxname) ||
      g_strcmp0 (demuxname, "") == 0) {
    ERROR (test, "Demuxer name not set");
    goto failed;
  }
  glob_demuxer = gst_element_factory_make (demuxname, "demuxer");
  if (glob_demuxer == NULL)
    goto failed;

  g_signal_connect (glob_demuxer, "pad-added", G_CALLBACK (pad_added_cb),
      ptest);

  /* And the multiqueue */
  glob_multiqueue = gst_element_factory_make ("multiqueue", "multiqueue");
  g_object_set (glob_multiqueue, "sync-by-running-time", TRUE, NULL);

  /* Add everything to the bin */
  gst_bin_add_many (GST_BIN (glob_pipeline), glob_src, glob_demuxer,
      glob_multiqueue, NULL);

  /* ... and link */
  if (gst_element_link (glob_src, glob_demuxer) == FALSE)
    goto failed;

  /* We check wether the element is a parser or not */
  decofactory = gst_element_get_factory (glob_demuxer);
  klass = gst_element_factory_get_klass (decofactory);

  if (g_strrstr (klass, "Demuxer") == NULL) {
    gchar *val_test = g_strdup_printf ("%s not a demuxer as "
        "Demuxer not present in the element factory klass: %s", demuxname,
        klass);

    insanity_test_validate_checklist_item (test, "testing-demuxer",
        FALSE, val_test);

    g_free (val_test);
    goto failed;
  }

  insanity_test_validate_checklist_item (test, "testing-demuxer", TRUE, NULL);

done:
  DEMUX_TEST_UNLOCK ();

  g_free (demuxname);
  g_free (uri);
  g_free (location);
  if (err != NULL)
    g_error_free (err);

  return GST_PIPELINE (glob_pipeline);

failed:
  if (glob_pipeline != NULL)
    gst_object_unref (glob_pipeline);
  if (glob_demuxer != NULL)
    gst_object_unref (glob_demuxer);
  if (glob_src != NULL)
    gst_object_unref (glob_src);
  if (glob_multiqueue != NULL)
    gst_object_unref (glob_multiqueue);

  glob_pipeline = glob_demuxer = glob_multiqueue = glob_src = NULL;

  goto done;
}

static gboolean
start_cb (InsanityTest * test)
{
  gboolean ret = TRUE;

  GError *err = NULL;
  gchar *xmllocation = NULL, *location = NULL;

  DEMUX_TEST_LOCK ();

  insanity_test_get_string_argument (test, "location", &location);
  if (location == NULL || g_strcmp0 (location, "") == 0) {
    ERROR (test, "Location name not set\n");
    ret = FALSE;
    goto done;
  }

  glob_parsing_buf = gst_buffer_new ();
  xmllocation = g_strconcat (location, ".xml", NULL);
  glob_parser = media_descriptor_parser_new (test, xmllocation, &err);
  if (glob_parser == NULL) {
    gboolean generate_media_desc;

    LOG (test, "Could not create media descriptor parser: %s", err->message);

    insanity_test_get_boolean_argument (test, "generate-media-descriptor",
        (gboolean *) & generate_media_desc);

    if (generate_media_desc == FALSE) {
      ret = FALSE;
      goto done;
    }

    LOG (test, "%s not found generating it", xmllocation);

    generate_xml_media_descriptor (test);
    goto done;
  }

  glob_detecting_frame = media_descriptor_parser_detects_frames (glob_parser);

done:
  g_free (location);
  g_free (xmllocation);

  insanity_test_get_uint64_argument (test, "playback-duration",
      &glob_playback_duration);

  DEMUX_TEST_UNLOCK ();

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

  /* Clear variables that are shared between all start/stop iterations */
  gst_buffer_unref (glob_parsing_buf);
  glob_parsing_buf = NULL;
}

static gboolean
stop_cb (InsanityTest * test)
{
  if (glob_parser == NULL) {

    LOG (test, "No xml file found \"stream-detection\", \"frame-detection\" "
        "and \"tag-detection\" skiped\n");
  } else if (glob_writer != NULL) {
    LOG (test, "Xml file generated \"stream-detection\", \"frame-detection\" "
        "and \"tag-detection\" do not mean much\n");
  } else {
    insanity_test_validate_checklist_item (test, "stream-detection",
        media_descriptor_parser_all_stream_found (glob_parser), NULL);

    insanity_test_validate_checklist_item (test, "tag-detection",
        media_descriptor_parser_all_tags_found (glob_parser), NULL);
  }

  if (!glob_wrong_seqnum) {
    insanity_test_validate_checklist_item (test, "seqnum-management", TRUE,
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
  const gchar *demuxer_name = NULL;

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("stream-switch-test", "Tests stream "
      "switching inside playbin2", NULL);
  test = INSANITY_TEST (ptest);
  insanity_gst_pipeline_test_set_create_pipeline_in_start (ptest, TRUE);

  /* Arguments */
  insanity_test_add_string_argument (test, "location",
      "The location to test on", NULL, FALSE, location);
  insanity_test_add_string_argument (test, "demuxer",
      "The demuxer element name " "to test", NULL, FALSE, demuxer_name);
  insanity_test_add_boolean_argument (test, "push-mode",
      "Whether the pipeline should run in push mode or not (pull mode)",
      NULL, FALSE, FALSE);
  insanity_test_add_boolean_argument (test, "generate-media-descriptor",
      "Whether you want to generate the media descriptor XML file if needed",
      NULL, TRUE, TRUE);
  insanity_test_add_uint64_argument (test, "playback-duration",
      "Stream time to playback for before seeking, in seconds", NULL, TRUE, 2);

  /* Checklist */
  insanity_test_add_checklist_item (test, "testing-demuxer",
      "Whether the element we are testing (referenced with \"decoder-name\""
      "is indeed a demuxer", NULL);
  insanity_test_add_checklist_item (test, "stream-detection", "The demuxer "
      "detects the various stream and sets the caps properly", NULL);
  insanity_test_add_checklist_item (test, "frames-detection", "The demuxer "
      "detects the frames and its metadatas properly", NULL);
  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL);
  insanity_test_add_checklist_item (test, "seekable-detection",
      "The demuxer detects if a stream is seekable or not", NULL);
  insanity_test_add_checklist_item (test, "duration-detection",
      "The demuxer detects duration of the stream properly", NULL);
  insanity_test_add_checklist_item (test, "position-detection",
      "The demuxer detects the position in the stream properly", NULL);
  insanity_test_add_checklist_item (test, "tag-detection",
      "The demuxer detects the tags in the stream properly", NULL);
  insanity_test_add_checklist_item (test, "first-segment", "The demuxer sends a"
      " first segment with proper values before " "first buffers", NULL);
  insanity_test_add_checklist_item (test, "seqnum-management", "The events"
      "we receive have the seqnum it should have", NULL);
  insanity_test_add_checklist_item (test, "fast-forward", "The demuxer could "
      " properly play the stream fast-forward" "first buffers", NULL);
  insanity_test_add_checklist_item (test, "fast-backward", "The demuxer could "
      " properly play the stream fast-backward" "first buffers", NULL);
  insanity_test_add_checklist_item (test, "segment-seek", "The demuxer could "
      " properly segment seeking", NULL);
  insanity_test_add_checklist_item (test, "backward-playback",
      "The demuxer could " " properly play the stream backward" "first buffers",
      NULL);
  insanity_test_add_checklist_item (test, "unlink-pad-handling", "The demuxer"
      "properly handles pad is unlinking (errors out if only 1 source pad, keep"
      "pushing buffer on other pad otherwize)" "first buffers", NULL);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &demux_test_create_pipeline, NULL, NULL);

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
