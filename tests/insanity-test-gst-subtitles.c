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
#include <gst/video/video.h>

#include "media-descriptor-parser.h"
#include "media-descriptor-writer.h"

#define LOG(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "subtitle", INSANITY_LOG_LEVEL_DEBUG, format "\n", ##args)
#define ERROR(test, format, args...) \
  INSANITY_LOG (INSANITY_TEST((test)), "subtitle", INSANITY_LOG_LEVEL_DEBUG, format "\n", ##args)

static GStaticMutex glob_mutex = G_STATIC_MUTEX_INIT;
#define SUBTITLES_TEST_LOCK() g_static_mutex_lock (&glob_mutex)
#define SUBTITLES_TEST_UNLOCK() g_static_mutex_unlock (&glob_mutex)

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
  GstElement *element;
  GstPad *pad;
  gulong probe_id;

  InsanityTest *test;

  GstSegment last_segment;
  gboolean waiting_segment;

  /* First segment test */
  gboolean waiting_first_segment;

} ProbeContext;

typedef enum
{
  TEST_SUBTTILE_DESCRIPTOR_GENERATION,

  TEST_NONE,
  TEST_SUBTITLE_DETECTION
} TestInProgress;

/*******************************************************************************
 *                 +--------+   +-------+  +-+   +----------+
 *               +>|typefind|+->|demuxer|+>|m|+->|          |
 * +------------+| +--------+   +-------+  |u|   | subtitle |
 * |videotestsrc|+             (if needed) |l|   |          |   +--------+
 * +------------+                          |t|   |          |+->|fakesink|
 *                                         |i|   | overlay  |   +--------+
 * +---------+    +---------+  +-------+   |q|   |          |
 * |filesrc  +----|capsfiler|--|convert|-> |u|+->|          |
 * |         |    |1080*1920|  +-------+   |e|   |          |
 * +---------+    +---------+              |u|   +----------+
 *                                         +-+
 ******************************************************************************/

/* Global GstElement-s */
static GstElement *glob_pipeline = NULL;
static GstElement *glob_uridecodebin = NULL;    /* videotestsrc */
static GstElement *glob_suboverlay = NULL;      /* A subtitleoverlay bin */
static GstElement *glob_videotestsrc = NULL;

/* Gloabl fields */
static MediaDescriptorParser *glob_parser = NULL;
static GList *glob_subtitled_frames = NULL;

/* Media descriptor writer context */
static MediaDescriptorWriter *glob_writer = NULL;
static GstClockTime glob_duration = 0;
static GstClockTime glob_seekable = FALSE;
static gboolean glob_pipeline_restarted = TRUE;
/* Used to avoid frame multiplication in case of continuous subtitles*/
static GstClockTime glob_last_subtitled_frame = GST_CLOCK_TIME_NONE;

static ProbeContext *glob_suboverlay_src_probe = NULL;
static ProbeContext *glob_renderer_sink_probe = NULL;
static gboolean glob_sub_found = FALSE;
static gboolean glob_wrong_rendered_buf = FALSE;
static gboolean glob_sub_render_found = FALSE;

static GstClockTime glob_first_subtitle_ts = GST_CLOCK_TIME_NONE;
static GstClockTime glob_playback_duration = GST_CLOCK_TIME_NONE;
static gboolean glob_push_mode = FALSE;

static TestInProgress glob_in_progress = TEST_NONE;

/* checking Checking wedge */
static gint64 global_last_probe = 0;
static guint global_idle_timeout = 0;

static gboolean next_test (InsanityTest * test);

static void
clean_test (InsanityTest * test)
{
  glob_pipeline = NULL;
  glob_videotestsrc = NULL;

  glob_uridecodebin = NULL;
  glob_suboverlay = NULL;

  glob_playback_duration = GST_CLOCK_TIME_NONE;
  glob_push_mode = FALSE;

  glob_sub_found = FALSE;
  glob_in_progress = TEST_NONE;

  global_last_probe = 0;
  global_idle_timeout = 0;
  glob_seekable = FALSE;

  if (glob_suboverlay_src_probe != NULL) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        glob_suboverlay_src_probe->pad, glob_suboverlay_src_probe->probe_id);
    g_slice_free (ProbeContext, glob_suboverlay_src_probe);
    glob_suboverlay_src_probe = NULL;
  }

  if (glob_renderer_sink_probe != NULL) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        glob_renderer_sink_probe->pad, glob_renderer_sink_probe->probe_id);
    g_slice_free (ProbeContext, glob_renderer_sink_probe);
    glob_renderer_sink_probe = NULL;
  }

  g_clear_object (&glob_parser);
  g_clear_object (&glob_writer);

  glob_first_subtitle_ts = GST_CLOCK_TIME_NONE;

  if (glob_subtitled_frames) {
    g_list_free_full (glob_subtitled_frames, (GDestroyNotify) gst_buffer_unref);
    glob_subtitled_frames = NULL;
  }

  glob_wrong_rendered_buf = FALSE;
  glob_pipeline_restarted = TRUE;
  glob_last_subtitled_frame = GST_CLOCK_TIME_NONE;
  glob_duration = 0;
}

/* Utils functions */
static inline const gchar *
test_get_name (TestInProgress in_progress)
{
  switch (in_progress) {
    case TEST_SUBTTILE_DESCRIPTOR_GENERATION:
      return "Generating XML descripton file";
    case TEST_NONE:
      return "None";
    case TEST_SUBTITLE_DETECTION:
      return "Detecting subtitles";
  }

  return NULL;
}

static inline const gchar *
pipeline_mode_get_name (gboolean push_mode)
{
  if (push_mode == TRUE)
    return "push";

  return "pull";
}

static gboolean
idle_restart_pipeline (void)
{
  gst_element_set_state (glob_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (glob_pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
  gst_element_seek_simple (glob_pipeline, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH, 0);
  gst_element_set_state (glob_pipeline, GST_STATE_PLAYING);
  glob_pipeline_restarted = TRUE;

  return FALSE;
}

static gint
sort_subtitle_bufs (GstBuffer * buf1, GstBuffer * buf2)
{
  if (GST_BUFFER_TIMESTAMP (buf1) <= GST_BUFFER_TIMESTAMP (buf2))
    return -1;

  return 1;
}

static gboolean
next_test (InsanityTest * test)
{
  gchar *sublocation = NULL, *xmllocation = NULL;
  GError *err = NULL;

  switch (glob_in_progress) {
    case TEST_SUBTTILE_DESCRIPTOR_GENERATION:
    {

      insanity_test_get_string_argument (test, "sublocation", &sublocation);
      xmllocation = g_strconcat (sublocation, ".subs.xml", NULL);

      LOG (test, "Done generation XML file, saving it to %s", xmllocation);

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

      glob_subtitled_frames = media_descriptor_parser_get_buffers (glob_parser,
          NULL, (GCompareFunc) sort_subtitle_bufs);

      if (glob_subtitled_frames == NULL) {
        ERROR (test, "No subtitles frames found");
        insanity_test_done (test);

        goto done;
      } else
        glob_first_subtitle_ts =
            GST_BUFFER_TIMESTAMP (glob_subtitled_frames->data);

      /* We reset the test so it starts again from the beginning */
      glob_in_progress = TEST_NONE;
      g_idle_add ((GSourceFunc) idle_restart_pipeline, NULL);

      break;
    }
    case TEST_NONE:
      glob_in_progress = TEST_SUBTITLE_DETECTION;
      break;
    case TEST_SUBTITLE_DETECTION:
      insanity_test_done (test);
      break;
  }

  LOG (test, "%s in progress", test_get_name (glob_in_progress));

done:
  g_free (sublocation);
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
    LOG (test, "Wedged, kicking");

    /* Unvalidate tests in progress */
    switch (glob_in_progress) {
      case TEST_SUBTTILE_DESCRIPTOR_GENERATION:
        insanity_test_done (test);
        return FALSE;
      case TEST_NONE:
        break;
      case TEST_SUBTITLE_DETECTION:
        insanity_test_validate_checklist_item (test, "subtitle-rendered",
            FALSE, "No buffers or events were seen for a while");
        break;
    }

    global_last_probe = g_get_monotonic_time ();
    g_idle_add ((GSourceFunc) & next_test, test);
  }

  return TRUE;
}

static void
generate_xml_media_descriptor (InsanityTest * test)
{
  GError *err = NULL;
  GstDiscovererInfo *info = NULL;
  gchar *sublocation = NULL, *suburi = NULL;

  GstDiscoverer *discoverer = gst_discoverer_new (5 * GST_SECOND, NULL);

  insanity_test_get_string_argument (test, "sublocation", &sublocation);

  if (G_UNLIKELY (discoverer == NULL)) {
    ERROR (test, "Error creating discoverer: %s\n", err->message);
    g_clear_error (&err);

    insanity_test_done (test);
    goto done;
  }

  suburi = gst_filename_to_uri (sublocation, &err);
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
      sublocation, glob_duration, glob_seekable);

  glob_in_progress = TEST_SUBTTILE_DESCRIPTOR_GENERATION;

  g_idle_add ((GSourceFunc) idle_restart_pipeline, NULL);

  media_descriptor_writer_add_stream (glob_writer,
      glob_suboverlay_src_probe->pad);

done:
  if (discoverer != NULL)
    g_object_unref (discoverer);

  if (info != NULL)
    gst_discoverer_info_unref (info);

  g_free (sublocation);
  g_free (suburi);
}

/* Pipeline Callbacks */
static gboolean
renderer_probe_cb (InsanityGstTest * ptest, GstPad * pad,
    GstMiniObject * object, gpointer userdata)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  if (GST_IS_BUFFER (object)) {
    gint64 stime_ts;
    GstBuffer *buf = GST_BUFFER (object), *nbuf;

    if (glob_in_progress == TEST_SUBTTILE_DESCRIPTOR_GENERATION)
      goto done;

    if (GST_CLOCK_TIME_IS_VALID (GST_BUFFER_DURATION (buf)) == FALSE
        && glob_parser == NULL) {
      gboolean generate_media_desc;

      insanity_test_get_boolean_argument (test, "create-media-descriptor",
          (gboolean *) & generate_media_desc);

      /* We generate the XML file if needed and allowed by user */
      if (generate_media_desc)
        generate_xml_media_descriptor (test);
      else
        insanity_test_done (test);
    } else if (glob_parser == NULL) {

      /* Avoid using xml descriptor when not needed */
      stime_ts =
          gst_segment_to_stream_time (&glob_renderer_sink_probe->last_segment,
          glob_renderer_sink_probe->last_segment.format,
          GST_BUFFER_TIMESTAMP (buf));

      if (GST_CLOCK_TIME_IS_VALID (glob_first_subtitle_ts) == FALSE)
        glob_first_subtitle_ts = stime_ts;

      nbuf = gst_buffer_new ();
      GST_BUFFER_TIMESTAMP (nbuf) = stime_ts;
      GST_BUFFER_DURATION (nbuf) = GST_BUFFER_DURATION (buf);

      glob_subtitled_frames = g_list_insert_sorted (glob_subtitled_frames, nbuf,
          (GCompareFunc) sort_subtitle_bufs);
    }
  } else if (GST_IS_EVENT (object)) {
    GstEvent *event = GST_EVENT (object);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_NEWSEGMENT:
      {
        GstFormat fmt;
        gint64 start, stop, position;
        gdouble rate, applied_rate;
        gboolean update;

        /* We do not care about event during subtitle generation */
        if (glob_in_progress == TEST_SUBTTILE_DESCRIPTOR_GENERATION)
          goto done;

        gst_event_parse_new_segment_full (event, &update, &rate,
            &applied_rate, &fmt, &start, &stop, &position);
        gst_segment_set_newsegment_full
            (&glob_renderer_sink_probe->last_segment, update, rate,
            applied_rate, fmt, start, stop, position);

        if (glob_renderer_sink_probe->waiting_segment == FALSE)
          /* Cache the segment as it will be our reference but don't look
           * further */
          goto done;

        if (glob_renderer_sink_probe->waiting_first_segment == TRUE) {
          /* Make sure that a new segment has been received for each stream */
          glob_renderer_sink_probe->waiting_first_segment = FALSE;
          glob_renderer_sink_probe->waiting_segment = FALSE;
        }

        glob_renderer_sink_probe->waiting_segment = FALSE;
        break;
      }
      default:
        break;
    }
  }

done:
  return TRUE;
}

static gboolean
frame_contains_subtitles (GstBuffer * buff)
{
  guint x, y, first_sub_pix_x = 0, first_sub_pix_y = 0, last_sub_y = 0;

  guint8 *data = GST_BUFFER_DATA (buff);

  for (y = 0; y < 1080; y++) {
    for (x = 0; x < 1920 * 3; x += 3) {
      if ((data[x + y * 1920 * 3] != 0x00) ||
          (data[x + y * 1920 * 3 + 1] != 0x00) ||
          (data[x + y * 1920 * 3 + 2] != 0x00)) {

        if (first_sub_pix_x == 0) {
          first_sub_pix_x = x / 3;
          first_sub_pix_y = y;
        }

        last_sub_y = y;
      }
    }
  }

  if (last_sub_y != 0 && last_sub_y != first_sub_pix_y)
    return TRUE;

  return FALSE;
}

static gboolean
probe_cb (InsanityGstTest * ptest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  global_last_probe = g_get_monotonic_time ();

  if (GST_IS_BUFFER (object)) {
    GstClockTime buf_start, buf_end;
    GstBuffer *next_sub, *buf = GST_BUFFER (object);

    buf_start =
        gst_segment_to_stream_time (&glob_suboverlay_src_probe->last_segment,
        glob_suboverlay_src_probe->last_segment.format,
        GST_BUFFER_TIMESTAMP (buf));
    buf_end = buf_start + GST_BUFFER_DURATION (buf);

    if (glob_in_progress == TEST_SUBTTILE_DESCRIPTOR_GENERATION) {
      if (glob_pipeline_restarted == TRUE) {
        gboolean has_subs;

        if (glob_duration > 0 && buf_end > glob_duration) {
          /* Done according to the duration previously found by the
           * discoverer */
          next_test (test);
        }

        has_subs = frame_contains_subtitles (buf);
        if (GST_CLOCK_TIME_IS_VALID (glob_last_subtitled_frame)) {
          if (has_subs == FALSE) {
            GstBuffer *nbuf = gst_buffer_new ();

            GST_BUFFER_TIMESTAMP (nbuf) = glob_last_subtitled_frame;
            GST_BUFFER_DURATION (nbuf) = buf_end - glob_last_subtitled_frame;
            media_descriptor_writer_add_frame (glob_writer, pad, nbuf);

            glob_last_subtitled_frame = GST_CLOCK_TIME_NONE;
            gst_buffer_unref (nbuf);
          }
        } else if (has_subs) {
          glob_last_subtitled_frame = buf_start;
        }
      }

      goto done;
    }

    /* We played enough... next test */
    if (GST_CLOCK_TIME_IS_VALID (glob_first_subtitle_ts) &&
        buf_start >=
        glob_first_subtitle_ts + glob_playback_duration * GST_SECOND) {
      next_test (test);
    }

    switch (glob_in_progress) {
      case TEST_NONE:
      {

        if (glob_suboverlay_src_probe->waiting_first_segment == TRUE) {
          insanity_test_validate_checklist_item (test, "first-segment", FALSE,
              "Got a buffer before the first segment");
        }
        next_test (test);
      }
      default:
        break;
    }

    if (glob_subtitled_frames != NULL) {
      GstClockTime sub_start, sub_end;

      next_sub = GST_BUFFER (glob_subtitled_frames->data);

      sub_start = GST_BUFFER_TIMESTAMP (next_sub);
      sub_end = GST_BUFFER_DURATION_IS_VALID (next_sub) ?
          GST_BUFFER_DURATION (next_sub) + sub_start : -1;

      if (buf_start >= sub_start && buf_end < sub_end) {
        if (frame_contains_subtitles (buf) == TRUE) {
          glob_sub_render_found = TRUE;
          insanity_test_validate_checklist_item (test, "subtitle-rendered",
              TRUE, NULL);
        } else {
          gchar *msg = g_strdup_printf ("Subtitle start %" GST_TIME_FORMAT
              " end %" GST_TIME_FORMAT " received buffer with no sub start %"
              GST_TIME_FORMAT " end %" GST_TIME_FORMAT,
              GST_TIME_ARGS (sub_start),
              GST_TIME_ARGS (sub_end), GST_TIME_ARGS (buf_start),
              GST_TIME_ARGS (buf_end));

          insanity_test_validate_checklist_item (test, "subtitle-rendered",
              FALSE, msg);
          glob_wrong_rendered_buf = TRUE;

          g_free (msg);
        }
      } else if (buf_end > sub_end) {
        /* We got a buffer that is after the subtitle we were waiting for
         * remove that buffer as not waiting for it anymore */
        gst_buffer_unref (next_sub);

        glob_subtitled_frames = g_list_remove (glob_subtitled_frames, next_sub);
      }
    }

  } else if (GST_IS_EVENT (object)) {
    GstEvent *event = GST_EVENT (object);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_NEWSEGMENT:
      {
        GstFormat fmt;
        gint64 start, stop, position;
        gdouble rate, applied_rate;
        gboolean update;

        gst_event_parse_new_segment_full (event, &update, &rate,
            &applied_rate, &fmt, &start, &stop, &position);
        gst_segment_set_newsegment_full
            (&glob_suboverlay_src_probe->last_segment, update, rate,
            applied_rate, fmt, start, stop, position);

        if (glob_suboverlay_src_probe->waiting_first_segment == TRUE) {
          insanity_test_validate_checklist_item (test, "first-segment", TRUE,
              NULL);
          glob_suboverlay_src_probe->waiting_first_segment = FALSE;
        }

        if (glob_suboverlay_src_probe->waiting_segment == FALSE)
          /* Cache the segment as it will be our reference but don't look
           * further */
          goto done;

        if (glob_suboverlay_src_probe->waiting_first_segment == TRUE) {
          /* Make sure that a new segment has been received for each stream */
          glob_suboverlay_src_probe->waiting_first_segment = FALSE;
          glob_suboverlay_src_probe->waiting_segment = FALSE;
        }

        glob_suboverlay_src_probe->waiting_segment = FALSE;
        break;
      }
      default:
        break;
    }
  }

done:
  return TRUE;
}

static gint
find_renderer_subtitle_sinkpad (GstPad * pad)
{
  GstCaps *caps = gst_pad_get_caps (pad);
  GstStructure *stru = gst_caps_get_structure (caps, 0);

  gst_object_unref (pad);

  if (g_strrstr (gst_structure_get_name (stru), "subpicture") != NULL)
    return 0;
  else if (g_strrstr (gst_structure_get_name (stru), "video") == NULL)
    return 0;

  return 1;

}

static void
suboverlay_child_added_cb (GstElement * suboverlay, GstElement * child,
    InsanityTest * test)
{
  GstIterator *it;
  GstPad *render_sub_sink, *tmppad;
  GstElementFactory *fact;
  const gchar *klass, *name;
  gulong probe_id;

  gboolean is_renderer = FALSE;


  /* cc-ed from -base/gstsubtitleoveraly.c */
  fact = gst_element_get_factory (child);
  klass = gst_element_factory_get_klass (fact);

  if (GST_IS_BIN (child))
    return;

  name = gst_plugin_feature_get_name (GST_PLUGIN_FEATURE_CAST (fact));

  if (g_strrstr (klass, "Overlay/Subtitle") != NULL ||
      g_strrstr (klass, "Overlay/SubPicture") != NULL)
    is_renderer = TRUE;

  else if (g_strcmp0 (name, "textoverlay") == 0)
    is_renderer = TRUE;

  if (is_renderer == FALSE)
    return;

  LOG (test, "Renderer found: %s", name);

  /* Now adding the probe to the renderer "subtitle" sink pad */
  it = gst_element_iterate_sink_pads (child);
  render_sub_sink = gst_iterator_find_custom (it,
      (GCompareFunc) find_renderer_subtitle_sinkpad, NULL);
  gst_iterator_free (it);

  if (insanity_gst_test_add_data_probe (INSANITY_GST_TEST (test),
          GST_BIN (glob_pipeline), GST_OBJECT_NAME (child),
          GST_ELEMENT_NAME (render_sub_sink), &tmppad, &probe_id,
          &renderer_probe_cb, NULL, NULL) == TRUE) {

    glob_renderer_sink_probe = g_slice_new0 (ProbeContext);
    glob_renderer_sink_probe->probe_id = probe_id;
    glob_renderer_sink_probe->pad = render_sub_sink;
    glob_renderer_sink_probe->element = child;
    glob_renderer_sink_probe->test = test;
    glob_renderer_sink_probe->waiting_first_segment = TRUE;

    insanity_test_validate_checklist_item (test, "install-probes", TRUE, NULL);
  } else {
    insanity_test_validate_checklist_item (test, "install-probes", FALSE,
        "Failed to attach probe to fakesink");
    insanity_test_done (test);

    return;
  }

}

static gboolean
pad_added_cb (GstElement * element, GstPad * new_pad, InsanityTest * test)
{
  GstPadLinkReturn linkret;

  GstIterator *it = NULL;
  GstCaps *caps = NULL;
  gboolean ret = TRUE;

  GstPad *mqsinkpad = NULL, *mqsrcpad = NULL, *ssinkpad =
      NULL, *suboverlaysinkpad = NULL;

  SUBTITLES_TEST_LOCK ();

  /* First check if the pad caps are compatible with the suboverlay */
  caps = gst_pad_get_caps (new_pad);
  suboverlaysinkpad = gst_element_get_compatible_pad (glob_suboverlay, new_pad,
      caps);

  if (suboverlaysinkpad == NULL) {
    LOG (test,
        "Pad %" GST_PTR_FORMAT " with caps %" GST_PTR_FORMAT " Not usefull",
        new_pad, caps);
    goto error;
  }

  glob_sub_found = TRUE;
  insanity_test_validate_checklist_item (test, "testing-subtitles", TRUE, NULL);

  /* Link to the decoder */
  linkret = gst_pad_link (new_pad, suboverlaysinkpad);
  if (linkret != GST_PAD_LINK_OK) {
    ERROR (test, "Getting linking %" GST_PTR_FORMAT " with %" GST_PTR_FORMAT,
        mqsrcpad, suboverlaysinkpad);
    goto error;
  }

  LOG (test, "Pad %" GST_PTR_FORMAT " with caps %" GST_PTR_FORMAT " linked,"
      " and ready to be tested", new_pad, caps);

done:
  SUBTITLES_TEST_UNLOCK ();

  if (it)
    gst_iterator_free (it);

  if (suboverlaysinkpad)
    gst_object_unref (suboverlaysinkpad);

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

static gboolean
bus_message_cb (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  InsanityTest *test = INSANITY_TEST (ptest);

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
    {
      LOG (test, "Got EOS");
      if (glob_in_progress == TEST_SUBTTILE_DESCRIPTOR_GENERATION)
        next_test (test);

      /* Keep testing anyway */
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
  GstCaps *caps;
  gulong probe_id;
  GError *err = NULL;
  GstIterator *it = NULL;
  gchar *uri = NULL, *sublocation = NULL;
  GstElement *capsfilter = NULL, *capsfilter1 = NULL, *colorspace =
      NULL, *colorspace1 = NULL, *fakesink = NULL;
  GstPad *fakesinksink = NULL, *tmppad = NULL;

  InsanityTest *test = INSANITY_TEST (ptest);

  SUBTITLES_TEST_LOCK ();
  glob_pipeline = GST_ELEMENT (gst_pipeline_new ("pipeline"));

  /* Create the source */
  insanity_test_get_boolean_argument (test, "push-mode",
      (gboolean *) & glob_push_mode);

  insanity_test_get_string_argument (test, "sublocation", &sublocation);
  if (sublocation == NULL || g_strcmp0 (sublocation, "") == 0) {
    ERROR (test, "Location name not set\n");
    goto creation_failed;
  }

  uri = gst_filename_to_uri (sublocation, &err);
  if (err != NULL) {
    ERROR (test, "Error creating uri %s", err->message);

    goto creation_failed;
  }
  if (glob_push_mode == TRUE) {
    gchar *tmpuri;

    glob_uridecodebin = gst_element_factory_make ("pushfilesrc", "src");
    tmpuri = g_strconcat ("push", uri, NULL);
    g_free (uri);

    uri = tmpuri;
  }

  glob_uridecodebin = gst_element_factory_make ("uridecodebin", "src");
  g_signal_connect (glob_uridecodebin, "pad-added", G_CALLBACK (pad_added_cb),
      test);
  g_object_set (glob_uridecodebin, "uri", uri, NULL);

  /* the subtitleoverlay */
  glob_suboverlay =
      gst_element_factory_make ("subtitleoverlay", "subtitleoverlay");
  if (glob_suboverlay == NULL)
    goto creation_failed;

  /* the fakesink */
  fakesink = gst_element_factory_make ("fakesink", "fakesink");
  if (fakesink == NULL)
    goto creation_failed;

  /* and the videotestsrc */
  glob_videotestsrc = gst_element_factory_make ("videotestsrc", "videotestsrc");
  if (glob_videotestsrc == NULL)
    goto creation_failed;
  g_object_set (glob_videotestsrc, "pattern", 2, "do-timestamp", TRUE, NULL);

  /* Make sure the video is big enough */
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  if (capsfilter == NULL)
    goto creation_failed;

  caps = gst_video_format_new_caps (GST_VIDEO_FORMAT_RGB, 1920, 1080, 25, 1,
      1, 1);

  g_object_set (capsfilter, "caps", caps, NULL);

  capsfilter1 = gst_element_factory_make ("capsfilter", NULL);
  if (capsfilter1 == NULL)
    goto creation_failed;

  /* We want the last frame that we will "parse" to check if it contains
   * subtitles to be in RGB to make simpler for us */
  caps = gst_caps_from_string ("video/x-raw-rgb, bpp=24, height=(gint)1080,"
      "width=(gint)1920;");
  g_object_set (capsfilter1, "caps", caps, NULL);

  colorspace = gst_element_factory_make ("ffmpegcolorspace", NULL);
  if (colorspace == NULL)
    goto creation_failed;

  colorspace1 = gst_element_factory_make ("ffmpegcolorspace", NULL);
  if (colorspace1 == NULL)
    goto creation_failed;

  /* Now add to the pipeline */
  gst_bin_add_many (GST_BIN (glob_pipeline), glob_uridecodebin,
      glob_videotestsrc, capsfilter, glob_suboverlay,
      capsfilter1, colorspace, colorspace1, fakesink, NULL);

  /* link video branch elements */
  gst_element_link_many (glob_videotestsrc, capsfilter,
      glob_suboverlay, colorspace, capsfilter1, fakesink, NULL);

  /* And install a probe to the subtitleoverlay src pad */
  fakesinksink = gst_element_get_static_pad (fakesink, "sink");
  if (fakesinksink == NULL)
    goto failed;

  if (insanity_gst_test_add_data_probe (INSANITY_GST_TEST (test),
          GST_BIN (glob_pipeline), GST_OBJECT_NAME (fakesink),
          GST_OBJECT_NAME (fakesinksink), &tmppad, &probe_id,
          &probe_cb, NULL, NULL) == TRUE) {

    glob_suboverlay_src_probe = g_slice_new0 (ProbeContext);
    glob_suboverlay_src_probe->probe_id = probe_id;
    glob_suboverlay_src_probe->pad = fakesinksink;
    glob_suboverlay_src_probe->element = fakesink;
    glob_suboverlay_src_probe->test = test;
    glob_suboverlay_src_probe->waiting_first_segment = TRUE;

    insanity_test_validate_checklist_item (test, "install-probes", TRUE, NULL);
  } else {
    insanity_test_validate_checklist_item (test,
        "install-probes", FALSE, "Failed to attach probe to fakesink");
    insanity_test_done (test);
    goto failed;
  }

  g_signal_connect (GST_CHILD_PROXY (glob_suboverlay), "child-added",
      G_CALLBACK (suboverlay_child_added_cb), test);

done:
  SUBTITLES_TEST_UNLOCK ();

  g_free (uri);
  g_free (sublocation);
  if (err != NULL)
    g_error_free (err);
  if (it != NULL)
    gst_iterator_free (it);

  return GST_PIPELINE (glob_pipeline);

failed:
  if (glob_pipeline != NULL)
    gst_object_unref (glob_pipeline);

  glob_suboverlay = glob_pipeline = glob_videotestsrc =
      glob_uridecodebin = NULL;
  goto done;

creation_failed:
  if (glob_uridecodebin != NULL)
    gst_object_unref (glob_uridecodebin);
  if (glob_suboverlay != NULL)
    gst_object_unref (glob_suboverlay);
  if (glob_videotestsrc != NULL)
    gst_object_unref (glob_videotestsrc);
  if (fakesink != NULL)
    gst_object_unref (fakesink);

  goto failed;
}

static gboolean
start_cb (InsanityTest * test)
{
  gboolean ret = TRUE;

  gchar *sublocation = NULL, *xmllocation = NULL;
  GError *err = NULL;

  SUBTITLES_TEST_LOCK ();

  insanity_test_get_string_argument (test, "sublocation", &sublocation);
  if (sublocation == NULL || g_strcmp0 (sublocation, "") == 0) {
    ERROR (test, "Location name not set\n");
    ret = FALSE;
    goto done;
  }

  xmllocation = g_strconcat (sublocation, ".subs.xml", NULL);
  glob_parser = media_descriptor_parser_new (test, xmllocation, &err);
  if (glob_parser == NULL) {
    LOG (test, "Could not create media descriptor parser: %s not testing it",
        err->message);
    goto done;
  } else {
    glob_subtitled_frames = media_descriptor_parser_get_buffers (glob_parser,
        NULL, (GCompareFunc) sort_subtitle_bufs);
    if (glob_subtitled_frames == NULL) {
      ERROR (test, "No subtitles frames found");
      ret = FALSE;

      goto done;
    } else
      glob_first_subtitle_ts =
          GST_BUFFER_TIMESTAMP (glob_subtitled_frames->data);
  }

done:
  g_free (sublocation);
  g_free (xmllocation);

  insanity_test_get_uint64_argument (test, "playback-duration",
      &glob_playback_duration);

  SUBTITLES_TEST_UNLOCK ();

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
}

static gboolean
stop_cb (InsanityTest * test)
{
  if (glob_sub_found == FALSE) {
    /* A "not linked" error will be posted on the bus and the test will
     * properly be stoped in this case */
    insanity_test_validate_checklist_item (test, "testing-subtitles", FALSE,
        NULL);
  }
  if (glob_wrong_rendered_buf == TRUE || glob_sub_render_found == FALSE) {
    insanity_test_validate_checklist_item (test, "subtitle-rendered", FALSE,
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

  const gchar *sublocation = NULL;

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("subtitles-test",
      "Tests subtitles behaviour in various conditions", NULL);
  test = INSANITY_TEST (ptest);
  insanity_gst_pipeline_test_set_create_pipeline_in_start (ptest, TRUE);

  /* Arguments */
  insanity_test_add_string_argument (test, "sublocation",
      "The sublocation to test on", NULL, FALSE, sublocation);
  insanity_test_add_boolean_argument (test, "push-mode",
      "Whether the pipeline should run in push mode or not (pull mode)",
      NULL, FALSE, FALSE);
  insanity_test_add_uint64_argument (test, "playback-duration",
      "Stream time to playback for before seeking, in seconds", NULL, TRUE, 2);
  insanity_test_add_boolean_argument (test, "create-media-descriptor",
      "Whether to create the media descriptor XML file if not present",
      NULL, TRUE, TRUE);

  /* Checklist */
  insanity_test_add_checklist_item (test, "testing-subtitles",
      "Whether we found subtitle in @sublocation", NULL);
  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL);
  insanity_test_add_checklist_item (test, "subtitle-rendered",
      "The subtitles are properly rendered on top of the video", NULL);
  insanity_test_add_checklist_item (test, "first-segment", "The demuxer sends a"
      " first segment with proper values before " "first buffers", NULL);

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
