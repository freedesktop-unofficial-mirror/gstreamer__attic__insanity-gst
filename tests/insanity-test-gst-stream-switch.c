/* Insanity QA system

 Copyright (c) 2012, Collabora Ltd
   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>

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

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <insanity-gst/insanity-gst.h>
#include <string.h>

static GType gst_caps_src_get_type (void);
static GType gst_multiple_stream_demux_get_type (void);
static GType gst_codec_sink_get_type (void);
static GType gst_audio_codec_sink_get_type (void);
static GType gst_video_codec_sink_get_type (void);

/***** Source element that creates buffers with specific caps *****/

#undef parent_class
#define parent_class caps_src_parent_class
typedef struct _GstCapsSrc GstCapsSrc;
typedef GstPushSrcClass GstCapsSrcClass;

struct _GstCapsSrc
{
  GstPushSrc parent;

  GstCaps *caps;
  gchar *uri;
  gint nbuffers;
};

static GstURIType
gst_caps_src_uri_get_type (void)
{
  return GST_URI_SRC;
}

static gchar **
gst_caps_src_uri_get_protocols (void)
{
  static gchar *protocols[] = { (char *) "caps", NULL };

  return protocols;
}

static const gchar *
gst_caps_src_uri_get_uri (GstURIHandler * handler)
{
  GstCapsSrc *src = (GstCapsSrc *) handler;

  return src->uri;
}

static gboolean
gst_caps_src_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  GstCapsSrc *src = (GstCapsSrc *) handler;

  if (uri == NULL || !g_str_has_prefix (uri, "caps:"))
    return FALSE;

  g_free (src->uri);
  src->uri = g_strdup (uri);

  if (src->caps)
    gst_caps_unref (src->caps);
  src->caps = NULL;

  return TRUE;
}

static void
gst_caps_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_caps_src_uri_get_type;
  iface->get_protocols = gst_caps_src_uri_get_protocols;
  iface->get_uri = gst_caps_src_uri_get_uri;
  iface->set_uri = gst_caps_src_uri_set_uri;
}

static void
gst_caps_src_init_type (GType type)
{
  static const GInterfaceInfo uri_hdlr_info = {
    gst_caps_src_uri_handler_init, NULL, NULL
  };

  g_type_add_interface_static (type, GST_TYPE_URI_HANDLER, &uri_hdlr_info);
}

GST_BOILERPLATE_FULL (GstCapsSrc, gst_caps_src, GstPushSrc,
    GST_TYPE_PUSH_SRC, gst_caps_src_init_type);

static void
gst_caps_src_base_init (gpointer klass)
{
  static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src",
      GST_PAD_SRC, GST_PAD_ALWAYS,
      GST_STATIC_CAPS_ANY);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &src_templ);
  gst_element_class_set_details_simple (element_class,
      "CapsSource", "Source/Generic", "yep", "me");
}

static void
gst_caps_src_finalize (GObject * object)
{
  GstCapsSrc *src = (GstCapsSrc *) object;

  if (src->caps)
    gst_caps_unref (src->caps);
  src->caps = NULL;
  g_free (src->uri);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_caps_src_create (GstPushSrc * psrc, GstBuffer ** p_buf)
{
  GstCapsSrc *src = (GstCapsSrc *) psrc;
  GstBuffer *buf;

  if (!src->caps) {
    if (!src->uri) {
      return GST_FLOW_ERROR;
    }

    src->caps = gst_caps_from_string (src->uri + sizeof ("caps"));
    if (!src->caps) {
      return GST_FLOW_ERROR;
    }
  }

  buf = gst_buffer_new ();
  gst_buffer_set_caps (buf, src->caps);
  GST_BUFFER_TIMESTAMP (buf) =
      gst_util_uint64_scale (src->nbuffers, GST_SECOND, 25);
  GST_BUFFER_DURATION (buf) =
      gst_util_uint64_scale (src->nbuffers + 1, GST_SECOND, 25)
      - GST_BUFFER_TIMESTAMP (buf);
  src->nbuffers++;

  *p_buf = buf;
  return GST_FLOW_OK;
}

static void
gst_caps_src_class_init (GstCapsSrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstPushSrcClass *pushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_caps_src_finalize;
  pushsrc_class->create = gst_caps_src_create;
}

static void
gst_caps_src_init (GstCapsSrc * src, GstCapsSrcClass * klass)
{
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
}


/***** Demux element that creates a specific number of streams based on caps *****/
#undef parent_class
#define parent_class multiple_stream_demux_parent_class
typedef struct _GstMultipleStreamDemux GstMultipleStreamDemux;
typedef GstElementClass GstMultipleStreamDemuxClass;
typedef struct _GstMultipleStreamDemuxStream GstMultipleStreamDemuxStream;

typedef enum
{
  STREAM_TYPE_AUDIO, STREAM_TYPE_VIDEO, STREAM_TYPE_TEXT, STREAM_TYPE_OTHER
} StreamType;

struct _GstMultipleStreamDemuxStream
{
  GstPad *srcpad;
  StreamType type;
  GstFlowReturn last_flow;
};

struct _GstMultipleStreamDemux
{
  GstElement parent;

  GstPad *sinkpad;
  GstCaps *sinkcaps;

  gint n_streams;
  GstMultipleStreamDemuxStream *streams;

  GList *pending_events;
};

GST_BOILERPLATE (GstMultipleStreamDemux, gst_multiple_stream_demux, GstElement,
    GST_TYPE_ELEMENT);

static void
gst_multiple_stream_demux_base_init (gpointer klass)
{
  static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS,
      GST_STATIC_CAPS ("application/x-multiple-streams, "
          "n-audio  = (int) [0, 32], "
          "n-non-raw-audio  = (int) [0, 32], "
          "n-video  = (int) [0, 32], "
          "n-non-raw-video  = (int) [0, 32], "
          "n-text   = (int) [0, 64], " "n-other  = (int) [0, 64]")
      );
  static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src_%d",
      GST_PAD_SRC, GST_PAD_SOMETIMES,
      GST_STATIC_CAPS
      ("audio/x-raw-int; audio/x-compressed; "
          "video/x-raw-rgb; video/x-compressed; "
          "text/plain; application/x-something")
      );
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_templ);
  gst_element_class_add_static_pad_template (element_class, &src_templ);
  gst_element_class_set_details_simple (element_class,
      "MultipleStreamDemux", "Codec/Demux", "yep", "me");
}

static void
gst_multiple_stream_demux_finalize (GObject * object)
{
  GstMultipleStreamDemux *demux = (GstMultipleStreamDemux *) object;

  gst_caps_replace (&demux->sinkcaps, NULL);

  g_list_foreach (demux->pending_events, (GFunc) gst_event_unref, NULL);
  g_list_free (demux->pending_events);
  demux->pending_events = NULL;

  g_free (demux->streams);
  demux->streams = NULL;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_multiple_stream_demux_class_init (GstMultipleStreamDemuxClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = gst_multiple_stream_demux_finalize;
}

static GstFlowReturn
gst_multiple_stream_demux_combine_flow_ret (GstMultipleStreamDemux * demux,
    GstMultipleStreamDemuxStream * stream, GstFlowReturn ret)
{
  gint i;

  stream->last_flow = ret;
  if (ret != GST_FLOW_NOT_LINKED)
    goto done;

  for (i = 0; i < demux->n_streams; i++) {
    ret = demux->streams[i].last_flow;
    if (ret != GST_FLOW_NOT_LINKED)
      goto done;
  }

done:
  return ret;
}

static GstFlowReturn
gst_multiple_stream_demux_chain (GstPad * pad, GstBuffer * buf)
{
  GstMultipleStreamDemux *demux =
      (GstMultipleStreamDemux *) GST_PAD_PARENT (pad);
  GstFlowReturn ret = GST_FLOW_OK;
  gint i;

  for (i = 0; i < demux->n_streams; i++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[i];
    GstBuffer *outbuf;
    guint size;
    GList *l;

    for (l = demux->pending_events; l; l = l->next) {
      gst_pad_push_event (stream->srcpad, gst_event_ref (l->data));
    }

    switch (stream->type) {
      case STREAM_TYPE_VIDEO:{
        size = gst_video_format_get_size (GST_VIDEO_FORMAT_xRGB, 800, 600);
        outbuf = gst_buffer_new_and_alloc (size);
        gst_buffer_copy_metadata (outbuf, buf,
            GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS);
        gst_buffer_set_caps (outbuf, GST_PAD_CAPS (stream->srcpad));
        break;
      }
      case STREAM_TYPE_AUDIO:{
        size =
            gst_util_uint64_scale (GST_BUFFER_DURATION (buf), 48000 * 2,
            GST_SECOND);
        outbuf = gst_buffer_new_and_alloc (size);
        gst_buffer_copy_metadata (outbuf, buf,
            GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS);
        gst_buffer_set_caps (outbuf, GST_PAD_CAPS (stream->srcpad));
        break;
      }
      case STREAM_TYPE_OTHER:
      case STREAM_TYPE_TEXT:{
        size = 256;
        outbuf = gst_buffer_new_and_alloc (size);
        gst_buffer_copy_metadata (outbuf, buf,
            GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS);
        gst_buffer_set_caps (outbuf, GST_PAD_CAPS (stream->srcpad));
        break;
      }
      default:
        g_assert_not_reached ();
    }

    /* Mark the stream ID of this buffer */
    memset (GST_BUFFER_DATA (outbuf), i, size);

    ret = gst_pad_push (stream->srcpad, outbuf);
    ret = gst_multiple_stream_demux_combine_flow_ret (demux, stream, ret);
    if (ret != GST_FLOW_OK)
      goto done;
  }

  g_list_foreach (demux->pending_events, (GFunc) gst_event_unref, NULL);
  g_list_free (demux->pending_events);
  demux->pending_events = NULL;

done:
  gst_buffer_unref (buf);

  return ret;
}

static gboolean
gst_multiple_stream_demux_event (GstPad * pad, GstEvent * event)
{
  GstMultipleStreamDemux *demux =
      (GstMultipleStreamDemux *) gst_pad_get_parent (pad);
  gboolean ret = TRUE;

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    gint i;

    for (i = 0; i < demux->n_streams; i++)
      demux->streams[i].last_flow = GST_FLOW_OK;

    g_list_foreach (demux->pending_events, (GFunc) gst_event_unref, NULL);
    g_list_free (demux->pending_events);
    demux->pending_events = NULL;
  }

  if (demux->streams) {
    gint i;

    for (i = 0; i < demux->n_streams; i++)
      ret = ret
          && gst_pad_push_event (demux->streams[i].srcpad,
          gst_event_ref (event));
  } else if (GST_EVENT_IS_SERIALIZED (event)) {
    demux->pending_events =
        g_list_append (demux->pending_events, gst_event_ref (event));
  }

  gst_event_unref (event);

  gst_object_unref (demux);
  return ret;
}

static void
create_pad (GstMultipleStreamDemux * demux,
    GstMultipleStreamDemuxStream * stream, gint idx, GstCaps * caps)
{
  GstPadTemplate *templ;
  gchar *name;

  templ =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (demux),
      "src_%d");
  name = g_strdup_printf ("src_%d", idx);
  stream->srcpad = gst_pad_new_from_template (templ, name);
  g_free (name);
  gst_pad_set_active (stream->srcpad, TRUE);
  gst_pad_use_fixed_caps (stream->srcpad);

  gst_pad_set_caps (stream->srcpad, caps);

  gst_element_add_pad (GST_ELEMENT (demux), stream->srcpad);
}

static gboolean
gst_multiple_stream_demux_setcaps (GstPad * pad, GstCaps * caps)
{
  GstMultipleStreamDemux *demux =
      (GstMultipleStreamDemux *) gst_pad_get_parent (pad);
  GstStructure *s;
  gint n_audio, n_nonraw_audio, n_video, n_nonraw_video;
  gint n_text, n_other, n;
  gint i, j;

  if (demux->streams) {
    if (!gst_caps_is_equal (caps, demux->sinkcaps))
      return FALSE;
    return TRUE;
  }

  s = gst_caps_get_structure (caps, 0);

  if (!gst_structure_get_int (s, "n-audio", &n_audio) ||
      !gst_structure_get_int (s, "n-non-raw-audio", &n_nonraw_audio) ||
      !gst_structure_get_int (s, "n-video", &n_video) ||
      !gst_structure_get_int (s, "n-non-raw-video", &n_nonraw_video) ||
      !gst_structure_get_int (s, "n-text", &n_text) ||
      !gst_structure_get_int (s, "n-other", &n_other))
    return FALSE;

  demux->n_streams = n =
      n_audio + n_nonraw_audio + n_video + n_nonraw_video + n_text + n_other;
  gst_caps_replace (&demux->sinkcaps, caps);
  demux->streams = g_new0 (GstMultipleStreamDemuxStream, n);

  j = 0;
  for (i = 0; i < n_audio; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps = gst_caps_new_simple ("audio/x-raw-int",
        "rate", G_TYPE_INT, 48000,
        "channels", G_TYPE_INT, 1,
        "width", G_TYPE_INT, 16,
        "depth", G_TYPE_INT, 16,
        "endianness", G_TYPE_INT, G_BYTE_ORDER,
        "signed", G_TYPE_BOOLEAN, TRUE, NULL);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_AUDIO;
  }

  for (i = 0; i < n_nonraw_audio; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps = gst_caps_new_simple ("audio/x-compressed", NULL);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_AUDIO;
  }

  for (i = 0; i < n_video; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps =
        gst_video_format_new_caps (GST_VIDEO_FORMAT_xRGB, 800, 600, 25, 1, 1,
        1);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_VIDEO;
  }

  for (i = 0; i < n_nonraw_video; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps = gst_caps_new_simple ("video/x-compressed", NULL);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_VIDEO;
  }

  for (i = 0; i < n_text; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps = gst_caps_new_simple ("text/plain", NULL);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_TEXT;
  }

  for (i = 0; i < n_other; i++, j++) {
    GstMultipleStreamDemuxStream *stream = &demux->streams[j];
    GstCaps *caps;

    caps = gst_caps_new_simple ("application/x-something", NULL);
    create_pad (demux, stream, j, caps);
    gst_caps_unref (caps);

    stream->type = STREAM_TYPE_OTHER;
  }

  gst_element_no_more_pads (GST_ELEMENT (demux));

  gst_object_unref (demux);
  return TRUE;
}

static void
gst_multiple_stream_demux_init (GstMultipleStreamDemux * demux,
    GstMultipleStreamDemuxClass * klass)
{
  GstPadTemplate *templ;

  templ = gst_element_class_get_pad_template (klass, "sink");
  demux->sinkpad = gst_pad_new_from_template (templ, "sink");
  gst_pad_set_setcaps_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_multiple_stream_demux_setcaps));
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_multiple_stream_demux_chain));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_multiple_stream_demux_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);
}

#undef parent_class
#define parent_class codec_sink_parent_class

typedef struct _GstCodecSink GstCodecSink;
typedef GstBaseSinkClass GstCodecSinkClass;

struct _GstCodecSink
{
  GstBaseSink parent;

  gboolean audio;
  gboolean raw;
  gint n_raw, n_compressed;
};

GST_BOILERPLATE (GstCodecSink, gst_codec_sink, GstBaseSink, GST_TYPE_BASE_SINK);

static void
gst_codec_sink_base_init (gpointer klass)
{
}

static gboolean
gst_codec_sink_start (GstBaseSink * bsink)
{
  GstCodecSink *sink = (GstCodecSink *) bsink;

  sink->n_raw = 0;
  sink->n_compressed = 0;

  return TRUE;
}

static GstFlowReturn
gst_codec_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstCodecSink *sink = (GstCodecSink *) bsink;

  if (sink->raw)
    sink->n_raw++;
  else
    sink->n_compressed++;

  return GST_FLOW_OK;
}

static void
gst_codec_sink_class_init (GstCodecSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;

  basesink_class->start = gst_codec_sink_start;
  basesink_class->render = gst_codec_sink_render;
}

static void
gst_codec_sink_init (GstCodecSink * sink, GstCodecSinkClass * klass)
{
  gst_base_sink_set_sync (GST_BASE_SINK (sink), TRUE);
}

#undef parent_class
#define parent_class audio_codec_sink_parent_class

typedef GstCodecSink GstAudioCodecSink;
typedef GstCodecSinkClass GstAudioCodecSinkClass;

GST_BOILERPLATE (GstAudioCodecSink, gst_audio_codec_sink, GstBaseSink,
    gst_codec_sink_get_type ());

static void
gst_audio_codec_sink_base_init (gpointer klass)
{
  static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS,
      GST_STATIC_CAPS ("audio/x-raw-int; audio/x-compressed")
      );
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_templ);
  gst_element_class_set_details_simple (element_class,
      "AudioCodecSink", "Sink/Audio", "yep", "me");
}

static gboolean
gst_audio_codec_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstAudioCodecSink *sink = (GstAudioCodecSink *) bsink;
  GstStructure *s;

  s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (s, "audio/x-raw-int")) {
    sink->raw = TRUE;
  } else if (gst_structure_has_name (s, "audio/x-compressed")) {
    sink->raw = FALSE;
  } else {
    return FALSE;
  }

  return TRUE;
}

static void
gst_audio_codec_sink_class_init (GstAudioCodecSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;

  basesink_class->set_caps = gst_audio_codec_sink_set_caps;
}

static void
gst_audio_codec_sink_init (GstAudioCodecSink * sink,
    GstAudioCodecSinkClass * klass)
{
  sink->audio = TRUE;
}

#undef parent_class
#define parent_class video_codec_sink_parent_class

typedef GstCodecSink GstVideoCodecSink;
typedef GstCodecSinkClass GstVideoCodecSinkClass;

GST_BOILERPLATE (GstVideoCodecSink, gst_video_codec_sink, GstBaseSink,
    gst_codec_sink_get_type ());

static void
gst_video_codec_sink_base_init (gpointer klass)
{
  static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS,
      GST_STATIC_CAPS ("video/x-raw-rgb; video/x-compressed")
      );
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class, &sink_templ);
  gst_element_class_set_details_simple (element_class,
      "VideoCodecSink", "Sink/Video", "yep", "me");
}

static gboolean
gst_video_codec_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstVideoCodecSink *sink = (GstVideoCodecSink *) bsink;
  GstStructure *s;

  s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (s, "video/x-raw-rgb")) {
    sink->raw = TRUE;
  } else if (gst_structure_has_name (s, "video/x-compressed")) {
    sink->raw = FALSE;
  } else {
    return FALSE;
  }

  return TRUE;
}

static void
gst_video_codec_sink_class_init (GstVideoCodecSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;

  basesink_class->set_caps = gst_video_codec_sink_set_caps;
}

static void
gst_video_codec_sink_init (GstVideoCodecSink * sink,
    GstVideoCodecSinkClass * klass)
{
  sink->audio = FALSE;
}

/***** The actual test *****/
#define SWITCH_TIMEOUT (15)
#define NSWITCHES (100)
#define MAX_STREAMS 8
static GstElement *pipeline;

static GStaticMutex global_mutex = G_STATIC_MUTEX_INIT;
#define TEST_LOCK() g_static_mutex_lock (&global_mutex)
#define TEST_UNLOCK() g_static_mutex_unlock (&global_mutex)

/* State that is set in start/stop */
typedef enum
{
  CURRENT_STEP_WAIT_PLAYING,
  CURRENT_STEP_WAIT_INITIAL_MARKERS,
  CURRENT_STEP_SWITCH_SUCCESSIVE,
  CURRENT_STEP_SWITCH,
  CURRENT_STEP_SWITCH_SIMULTANOUS
} CurrentStep;

static CurrentStep current_step = CURRENT_STEP_WAIT_PLAYING;
static gint current_step_switch = 0;
static GstPad *sinks[MAX_STREAMS] = { NULL };
static gulong probes[MAX_STREAMS] = { 0 };

static guint nsinks = 0;
static gboolean stream_switch_correct = TRUE;
static gboolean stream_switch_constant_correct = TRUE;
static gboolean unique_markers_correct = TRUE;
static gulong stream_switch_timeout_id = 0;
static gint64 max_stream_switch_time = -1;
static gint64 current_stream_switch_start_time = -1;

static struct
{
  gint current_marker;
  gboolean wait_switch;
} streams[3];
static gint n_audio, n_video, n_text, n_other, n_streams;
static gint n_nonraw_audio, n_nonraw_video;

static gint *markers[3];

/* State that is set in setup() */
static struct
{
  StreamType type;
  gint to_idx;
} switches[NSWITCHES];
static struct
{
  gint to_idx[3];
} simultanous_switches[NSWITCHES];

static GstPipeline *
stream_switch_test_create_pipeline (InsanityGstPipelineTest * ptest,
    gpointer userdata)
{
  const char *launch_line =
      "playbin2 audio-sink=\"audiocodecsink name=asink\" video-sink=\"videocodecsink name=vsink\" text-sink=\"capsfilter caps=\\\"text/plain\\\" ! fakesink name=tsink sync=true\"";
  GError *error = NULL;

  gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
      gst_caps_src_get_type ());
  gst_element_register (NULL, "multiplestreamdemux", GST_RANK_PRIMARY,
      gst_multiple_stream_demux_get_type ());
  gst_element_register (NULL, "audiocodecsink",
      GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ());
  gst_element_register (NULL, "videocodecsink",
      GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ());

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

  return GST_PIPELINE (pipeline);
}

static gboolean
stream_switch_test_setup (InsanityTest * test)
{
  GValue v = { 0 };
  guint32 seed;
  GRand *prg;
  gint i;

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

  /* Generate random stream switches from that seed */
  prg = g_rand_new_with_seed (seed);

  do {
    n_audio = g_rand_int_range (prg, 0, 10);
    n_nonraw_audio = g_rand_int_range (prg, 0, n_audio + 1);
    n_video = g_rand_int_range (prg, 0, 10);
    n_nonraw_video = g_rand_int_range (prg, 0, n_video + 1);
    n_text = g_rand_int_range (prg, 0, 10);
    n_other = g_rand_int_range (prg, 0, 10);
    n_streams = n_audio + n_video + n_text + n_other;
  } while (n_audio + n_video + n_text == 0);

  for (i = 0; i < NSWITCHES; i++) {
    do {
      switches[i].type = g_rand_int_range (prg, 0, 3);
      if (switches[i].type == STREAM_TYPE_AUDIO) {
        if (n_audio <= 1)
          continue;
        switches[i].to_idx = g_rand_int_range (prg, 0, n_audio);
        if (i > 0 && switches[i - 1].to_idx == switches[i].to_idx)
          continue;
      } else if (switches[i].type == STREAM_TYPE_VIDEO) {
        if (n_video <= 1)
          continue;
        switches[i].to_idx = g_rand_int_range (prg, 0, n_video);
        if (i > 0 && switches[i - 1].to_idx == switches[i].to_idx)
          continue;
      } else if (switches[i].type == STREAM_TYPE_TEXT) {
        if (n_text <= 1)
          continue;
        switches[i].to_idx = g_rand_int_range (prg, 0, n_text);
        if (i > 0 && switches[i - 1].to_idx == switches[i].to_idx)
          continue;
      }
      break;
    } while (TRUE);
  }

  for (i = 0; i < NSWITCHES; i++) {
    do {
      simultanous_switches[i].to_idx[STREAM_TYPE_AUDIO] =
          n_audio > 1 ? g_rand_int_range (prg, 0, n_audio) : -1;
      if (n_audio > 1 && i > 0
          && simultanous_switches[i - 1].to_idx[STREAM_TYPE_AUDIO] ==
          simultanous_switches[i].to_idx[STREAM_TYPE_AUDIO])
        continue;
      simultanous_switches[i].to_idx[STREAM_TYPE_VIDEO] =
          n_video > 1 ? g_rand_int_range (prg, 0, n_video) : -1;
      if (n_video > 1 && i > 0
          && simultanous_switches[i - 1].to_idx[STREAM_TYPE_VIDEO] ==
          simultanous_switches[i].to_idx[STREAM_TYPE_VIDEO])
        continue;
      simultanous_switches[i].to_idx[STREAM_TYPE_TEXT] =
          n_text > 1 ? g_rand_int_range (prg, 0, n_text) : -1;
      if (n_text > 1 && i > 0
          && simultanous_switches[i - 1].to_idx[STREAM_TYPE_TEXT] ==
          simultanous_switches[i].to_idx[STREAM_TYPE_TEXT])
        continue;
      break;
    } while (TRUE);
  }

  g_rand_free (prg);

  return TRUE;
}

static gboolean
stream_switch_test_start (InsanityTest * test)
{
  gchar *uri;

  TEST_LOCK ();

  uri = g_strdup_printf ("caps:application/x-multiple-streams, "
      "n-audio=(int)%d, n-non-raw-audio=(int)%d, "
      "n-video=(int)%d, n-non-raw-video=(int)%d, "
      "n-text=(int)%d, n-other=(int)%d",
      n_audio - n_nonraw_audio, n_nonraw_audio, n_video - n_nonraw_video,
      n_nonraw_video, n_text, n_other);
  g_object_set (pipeline, "uri", uri, NULL);
  g_free (uri);

  TEST_UNLOCK ();
  return TRUE;
}

static gboolean
stream_switch_test_stop (InsanityTest * test)
{
  GValue v = { 0, };
  int n;

  TEST_LOCK ();
  current_step = CURRENT_STEP_WAIT_PLAYING;
  current_step_switch = 0;

  for (n = 0; n < nsinks; n++) {
    insanity_gst_test_remove_data_probe (INSANITY_GST_TEST (test),
        sinks[n], probes[n]);
    gst_object_unref (sinks[n]);
    sinks[n] = NULL;
    probes[n] = 0;
  }
  nsinks = 0;
  if (stream_switch_correct)
    insanity_test_validate_checklist_item (test, "stream-switch", TRUE, NULL);
  stream_switch_correct = TRUE;
  if (stream_switch_constant_correct)
    insanity_test_validate_checklist_item (test, "streams-constant", TRUE,
        NULL);
  stream_switch_constant_correct = TRUE;
  if (unique_markers_correct)
    insanity_test_validate_checklist_item (test, "unique-markers", TRUE, NULL);
  unique_markers_correct = TRUE;

  g_value_init (&v, G_TYPE_INT64);
  g_value_set_int64 (&v, max_stream_switch_time);
  insanity_test_set_extra_info (test, "max-stream-switch-time", &v);
  g_value_unset (&v);

  current_stream_switch_start_time = -1;
  max_stream_switch_time = -1;
  stream_switch_timeout_id = 0;

  g_free (markers[STREAM_TYPE_AUDIO]);
  markers[STREAM_TYPE_AUDIO] = NULL;
  g_free (markers[STREAM_TYPE_VIDEO]);
  markers[STREAM_TYPE_VIDEO] = NULL;
  g_free (markers[STREAM_TYPE_TEXT]);
  markers[STREAM_TYPE_TEXT] = NULL;

  TEST_UNLOCK ();
  return TRUE;
}

static gboolean
stream_switch_timeout (InsanityTest * test)
{
  insanity_test_printf (test, "stream switch timeout\n");
  stream_switch_correct = FALSE;
  insanity_test_validate_checklist_item (test, "stream-switch", FALSE,
      "No stream switch happened after some time");
  insanity_test_done (test);
  return FALSE;
}

static gint
get_last_switch_for_stream (gint type)
{
  gint ret = -1;
  gint i;

  for (i = current_step_switch - 2; i >= 0; i--) {
    if (switches[i].type == type) {
      ret = switches[i].to_idx;
      break;
    }
  }

  return ret;
}

static gboolean
update_successive_markers (InsanityTest * test)
{
  StreamType type;
  gint idx, i, n;
  gint current_marker;

  if (current_step_switch == 0)
    return TRUE;

  if (current_step_switch <= n_audio) {
    type = STREAM_TYPE_AUDIO;
    idx = current_step_switch - 1;
    g_assert (idx < n_audio);
    n = n_audio;
  } else if (current_step_switch <= n_audio + n_video) {
    type = STREAM_TYPE_VIDEO;
    idx = current_step_switch - n_audio - 1;
    g_assert (idx < n_video);
    n = n_video;
  } else if (current_step_switch <= n_audio + n_video + n_text) {
    type = STREAM_TYPE_TEXT;
    idx = current_step_switch - n_audio - n_video - 1;
    g_assert (idx < n_text);
    n = n_text;
  } else {
    g_assert_not_reached ();
  }

  current_marker = streams[type].current_marker;
  for (i = 0; i < n; i++) {
    if (markers[type][i] == -1)
      break;
    if (markers[type][i] == current_marker) {
      insanity_test_printf (test,
          "Same marker %d for multiple streams (%d, %d) of type %d\n",
          current_marker, i, idx, type);
      insanity_test_validate_checklist_item (test, "unique-markers", FALSE,
          "Same marker for multiple streams");
      unique_markers_correct = FALSE;
      insanity_test_done (test);
      return FALSE;
    }
  }

  markers[type][idx] = current_marker;
  return TRUE;
}

static gboolean
validate_marker (InsanityTest * test, StreamType type, gint idx, gint marker)
{
  if (markers[type][idx] != -1 && markers[type][idx] != marker) {
    insanity_test_printf (test,
        "Wrong marker for stream %d of type %d (%d != %d)\n", idx, type, marker,
        markers[type][idx]);
    insanity_test_validate_checklist_item (test, "streams-constant", FALSE,
        "Wrong marker for index");
    stream_switch_constant_correct = FALSE;
    insanity_test_done (test);
    return FALSE;
  }

  return TRUE;
}

static void
do_next_switch (InsanityTest * test)
{
  if (current_stream_switch_start_time != -1) {
    gint64 stop_time = g_get_monotonic_time ();
    gint64 diff = stop_time - current_stream_switch_start_time;

    if (diff > max_stream_switch_time || max_stream_switch_time == -1)
      max_stream_switch_time = diff;
    current_stream_switch_start_time = -1;
    insanity_test_printf (test,
        "Current maximum stream switch time: %" G_GINT64_FORMAT "\n",
        max_stream_switch_time);
  }
  if (stream_switch_timeout_id)
    g_source_remove (stream_switch_timeout_id);
  stream_switch_timeout_id = 0;

  if (current_step == CURRENT_STEP_WAIT_PLAYING) {
    gint na, nv, nt, i;

    insanity_test_printf (test, "Reached PLAYING\n");

    g_object_get (pipeline, "n-audio", &na, "n-video", &nv, "n-text", &nt,
        NULL);

    if (na != n_audio) {
      insanity_test_printf (test,
          "Wrong number of audio streams (expected %d, got %d)\n", n_audio, na);
      insanity_test_validate_checklist_item (test, "found-all-streams", FALSE,
          NULL);
      insanity_test_done (test);
    } else if (nv != n_video) {
      insanity_test_printf (test,
          "Wrong number of video streams (expected %d, got %d)\n", n_video, nv);
      insanity_test_validate_checklist_item (test, "found-all-streams", FALSE,
          NULL);
      insanity_test_done (test);
    } else if (nt != n_text) {
      insanity_test_printf (test,
          "Wrong number of text streams (expected %d, got %d)\n", n_text, nt);
      insanity_test_validate_checklist_item (test, "found-all-streams", FALSE,
          NULL);
      insanity_test_done (test);
    } else {
      insanity_test_printf (test,
          "Found all streams, waiting for initial markers now\n");
      insanity_test_printf (test, "Audio %d, Video %d, Text %d\n", n_audio,
          n_video, n_text);

      insanity_test_validate_checklist_item (test, "found-all-streams", TRUE,
          NULL);
      current_step = CURRENT_STEP_WAIT_INITIAL_MARKERS;
      streams[STREAM_TYPE_AUDIO].current_marker = -1;
      if (n_audio > 0)
        streams[STREAM_TYPE_AUDIO].wait_switch = TRUE;
      streams[STREAM_TYPE_VIDEO].current_marker = -1;
      if (n_video > 0)
        streams[STREAM_TYPE_VIDEO].wait_switch = TRUE;
      streams[STREAM_TYPE_TEXT].current_marker = -1;
      if (n_text > 0)
        streams[STREAM_TYPE_TEXT].wait_switch = TRUE;
      stream_switch_timeout_id =
          g_timeout_add_seconds (SWITCH_TIMEOUT,
          (GSourceFunc) stream_switch_timeout, test);
    }

    markers[STREAM_TYPE_AUDIO] = g_new (gint, n_audio);
    for (i = 0; i < n_audio; i++)
      markers[STREAM_TYPE_AUDIO][i] = -1;
    markers[STREAM_TYPE_VIDEO] = g_new (gint, n_video);
    for (i = 0; i < n_video; i++)
      markers[STREAM_TYPE_VIDEO][i] = -1;
    markers[STREAM_TYPE_TEXT] = g_new (gint, n_text);
    for (i = 0; i < n_text; i++)
      markers[STREAM_TYPE_TEXT][i] = -1;
  } else if (current_step == CURRENT_STEP_WAIT_INITIAL_MARKERS) {
    g_assert (n_audio == 0 || streams[STREAM_TYPE_AUDIO].current_marker != -1);
    g_assert (n_video == 0 || streams[STREAM_TYPE_VIDEO].current_marker != -1);
    g_assert (n_text == 0 || streams[STREAM_TYPE_TEXT].current_marker != -1);

    insanity_test_printf (test,
        "Found all initial markers, switching streams now\n");
    current_step = CURRENT_STEP_SWITCH_SUCCESSIVE;
    current_step_switch = 0;

    do_next_switch (test);
  } else if (current_step == CURRENT_STEP_SWITCH_SUCCESSIVE) {
    if (!update_successive_markers (test))
      return;

    current_step_switch++;
    if (current_step_switch == n_audio + n_video + n_text + 1) {
      gint i;

      insanity_test_printf (test,
          "Finished all successive stream switches, doing random single stream switches now\n");

      for (i = 0; i < n_audio; i++)
        insanity_test_printf (test, "Audio marker %d: %d\n", i,
            markers[STREAM_TYPE_AUDIO][i]);
      for (i = 0; i < n_video; i++)
        insanity_test_printf (test, "Video marker %d: %d\n", i,
            markers[STREAM_TYPE_VIDEO][i]);
      for (i = 0; i < n_text; i++)
        insanity_test_printf (test, "Text marker %d: %d\n", i,
            markers[STREAM_TYPE_TEXT][i]);

      current_step = CURRENT_STEP_SWITCH;
      current_step_switch = 0;
      do_next_switch (test);
    } else {
      gint current;

      current_stream_switch_start_time = g_get_monotonic_time ();
      stream_switch_timeout_id =
          g_timeout_add_seconds (SWITCH_TIMEOUT,
          (GSourceFunc) stream_switch_timeout, test);

      if (current_step_switch <= n_audio) {
        insanity_test_printf (test,
            "Doing incremental switch for audio from %d -> %d\n",
            current_step_switch - 2, current_step_switch - 1);

        g_object_get (pipeline, "current-audio", &current, NULL);
        if (current != current_step_switch - 1) {
          g_object_set (pipeline, "current-audio",
              current_step_switch - 1, NULL);
          streams[STREAM_TYPE_AUDIO].wait_switch = TRUE;
        } else {
          insanity_test_printf (test, "Switching to same stream\n");
          do_next_switch (test);
        }
      } else if (current_step_switch <= n_audio + n_video) {
        insanity_test_printf (test,
            "Doing incremental switch for video from %d -> %d\n",
            current_step_switch - n_audio - 2,
            current_step_switch - n_audio - 1);

        g_object_get (pipeline, "current-video", &current, NULL);
        if (current != current_step_switch - n_audio - 1) {
          g_object_set (pipeline, "current-video",
              current_step_switch - n_audio - 1, NULL);
          streams[STREAM_TYPE_VIDEO].wait_switch = TRUE;
        } else {
          insanity_test_printf (test, "Switching to same stream\n");
          do_next_switch (test);
        }
      } else if (current_step_switch <= n_audio + n_video + n_text) {
        insanity_test_printf (test,
            "Doing incremental switch for text from %d -> %d\n",
            current_step_switch - n_audio - n_video - 2,
            current_step_switch - n_audio - n_video - 1);

        g_object_get (pipeline, "current-text", &current, NULL);
        if (current != current_step_switch - n_audio - n_video - 1) {
          g_object_set (pipeline, "current-text",
              current_step_switch - n_audio - n_video - 1, NULL);
          streams[STREAM_TYPE_TEXT].wait_switch = TRUE;
        } else {
          insanity_test_printf (test, "Switching to same stream\n");
          do_next_switch (test);
        }
      } else {
        g_assert_not_reached ();
      }
    }
  } else if (current_step == CURRENT_STEP_SWITCH) {
    current_step_switch++;

    if (current_step_switch == NSWITCHES + 1) {
      insanity_test_printf (test,
          "Finished all single stream switches, doing simultanous stream switches now\n");
      current_step = CURRENT_STEP_SWITCH_SIMULTANOUS;
      current_step_switch = 0;
      do_next_switch (test);
    } else {
      gint current;

      current_stream_switch_start_time = g_get_monotonic_time ();
      stream_switch_timeout_id =
          g_timeout_add_seconds (SWITCH_TIMEOUT,
          (GSourceFunc) stream_switch_timeout, test);

      insanity_test_printf (test,
          "Doing single stream switch %d for stream type %d: %d -> %d\n",
          current_step_switch - 1, switches[current_step_switch - 1].type,
          get_last_switch_for_stream (switches[current_step_switch - 1].type),
          switches[current_step_switch - 1].to_idx);
      switch (switches[current_step_switch - 1].type) {
        case STREAM_TYPE_AUDIO:
          g_object_get (pipeline, "current-audio", &current, NULL);
          if (current != switches[current_step_switch - 1].to_idx) {
            g_object_set (pipeline, "current-audio",
                switches[current_step_switch - 1].to_idx, NULL);
            streams[STREAM_TYPE_AUDIO].wait_switch = TRUE;
          } else {
            insanity_test_printf (test, "Switching to same stream\n");
            do_next_switch (test);
          }
          break;
        case STREAM_TYPE_VIDEO:
          g_object_get (pipeline, "current-video", &current, NULL);
          if (current != switches[current_step_switch - 1].to_idx) {
            g_object_set (pipeline, "current-video",
                switches[current_step_switch - 1].to_idx, NULL);
            streams[STREAM_TYPE_VIDEO].wait_switch = TRUE;
          } else {
            insanity_test_printf (test, "Switching to same stream\n");
            do_next_switch (test);
          }
          break;
        case STREAM_TYPE_TEXT:
          g_object_get (pipeline, "current-text", &current, NULL);
          if (current != switches[current_step_switch - 1].to_idx) {
            g_object_set (pipeline, "current-text",
                switches[current_step_switch - 1].to_idx, NULL);
            streams[STREAM_TYPE_TEXT].wait_switch = TRUE;
          } else {
            insanity_test_printf (test, "Switching to same stream\n");
            do_next_switch (test);
          }
          break;
        default:
          g_assert_not_reached ();
      }
    }
  } else if (current_step == CURRENT_STEP_SWITCH_SIMULTANOUS) {
    current_step_switch++;

    if (current_step_switch >= NSWITCHES + 1) {
      /* We're done now */
      insanity_test_printf (test, "Finished all simultanous stream switches\n");
      insanity_test_done (test);
    } else {
      gint current;
      gboolean all_same = TRUE;

      current_stream_switch_start_time = g_get_monotonic_time ();
      stream_switch_timeout_id =
          g_timeout_add_seconds (SWITCH_TIMEOUT,
          (GSourceFunc) stream_switch_timeout, test);

      insanity_test_printf (test,
          "Doing simultanous stream switch %d: %d -> %d, %d -> %d, %d -> %d\n",
          current_step_switch - 1, (current_step_switch - 1 > 0
              && n_audio >
              1 ? simultanous_switches[current_step_switch -
                  2].to_idx[STREAM_TYPE_AUDIO] : -1),
          (n_audio >
              1 ?
              simultanous_switches[current_step_switch - 1].to_idx
              [STREAM_TYPE_AUDIO] : -1), (current_step_switch - 1 > 0
              && n_video >
              1 ? simultanous_switches[current_step_switch -
                  2].to_idx[STREAM_TYPE_VIDEO] : -1),
          (n_video >
              1 ?
              simultanous_switches[current_step_switch - 1].to_idx
              [STREAM_TYPE_VIDEO] : -1), (current_step_switch - 1 > 0
              && n_text >
              1 ? simultanous_switches[current_step_switch -
                  2].to_idx[STREAM_TYPE_TEXT] : -1),
          (n_text >
              1 ?
              simultanous_switches[current_step_switch -
                  1].to_idx[STREAM_TYPE_TEXT]
              : -1));

      if (n_audio > 1) {
        g_object_get (pipeline, "current-audio", &current, NULL);
        if (current !=
            simultanous_switches[current_step_switch -
                1].to_idx[STREAM_TYPE_AUDIO]) {
          g_object_set (pipeline, "current-audio",
              simultanous_switches[current_step_switch -
                  1].to_idx[STREAM_TYPE_AUDIO], NULL);
          streams[STREAM_TYPE_AUDIO].wait_switch = TRUE;
          all_same = FALSE;
        }
      }
      if (n_video > 1) {
        g_object_get (pipeline, "current-video", &current, NULL);
        if (current !=
            simultanous_switches[current_step_switch -
                1].to_idx[STREAM_TYPE_VIDEO]) {
          g_object_set (pipeline, "current-video",
              simultanous_switches[current_step_switch -
                  1].to_idx[STREAM_TYPE_VIDEO], NULL);
          streams[STREAM_TYPE_VIDEO].wait_switch = TRUE;
          all_same = FALSE;
        }
      }
      if (n_text > 1) {
        g_object_get (pipeline, "current-text", &current, NULL);
        if (current !=
            simultanous_switches[current_step_switch -
                1].to_idx[STREAM_TYPE_TEXT]) {
          g_object_set (pipeline, "current-text",
              simultanous_switches[current_step_switch -
                  1].to_idx[STREAM_TYPE_TEXT], NULL);
          streams[STREAM_TYPE_TEXT].wait_switch = TRUE;
          all_same = FALSE;
        }
      }

      if (all_same) {
        insanity_test_printf (test,
            "Switching to all same streams, doing next switch\n");
        do_next_switch (test);
      }
    }
  }
}

static gboolean
probe (InsanityGstTest * gtest, GstPad * pad, GstMiniObject * object,
    gpointer userdata)
{
  StreamType type;
  GstBuffer *buffer;
  GstCaps *caps;
  GstStructure *s;
  guint8 marker;
  InsanityTest *test = INSANITY_TEST (gtest);

  insanity_test_ping (test);

  if (!GST_IS_BUFFER (object))
    return TRUE;

  TEST_LOCK ();

  buffer = GST_BUFFER (object);
  caps = GST_BUFFER_CAPS (object);
  s = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_name (s, "video/x-raw-rgb")
      || gst_structure_has_name (s, "video/x-compressed"))
    type = STREAM_TYPE_VIDEO;
  else if (gst_structure_has_name (s, "audio/x-raw-int")
      || gst_structure_has_name (s, "audio/x-compressed"))
    type = STREAM_TYPE_AUDIO;
  else if (gst_structure_has_name (s, "text/plain"))
    type = STREAM_TYPE_TEXT;
  else
    g_assert_not_reached ();

  g_assert (GST_BUFFER_SIZE (buffer) > 0);

  marker = GST_BUFFER_DATA (buffer)[0];

  insanity_test_printf (test,
      "%d: Found marker %d, current marker %d (wait switch: %d)\n", type,
      marker, streams[type].current_marker, streams[type].wait_switch);

  if (streams[type].wait_switch) {
    gboolean done = FALSE;
    gint idx;

    if (streams[type].current_marker == -1) {
      streams[type].current_marker = marker;
      streams[type].wait_switch = FALSE;
      insanity_test_printf (test, "%d: Stream switch done\n", type);
      done = TRUE;
    } else if (streams[type].current_marker != marker) {
      streams[type].current_marker = marker;
      streams[type].wait_switch = FALSE;
      insanity_test_printf (test, "%d: Stream switch done\n", type);
      done = TRUE;
    }

    if (type == STREAM_TYPE_AUDIO)
      g_object_get (pipeline, "current-audio", &idx, NULL);
    else if (type == STREAM_TYPE_VIDEO)
      g_object_get (pipeline, "current-video", &idx, NULL);
    else if (type == STREAM_TYPE_TEXT)
      g_object_get (pipeline, "current-text", &idx, NULL);
    else
      g_assert_not_reached ();

    if (done && !validate_marker (test, type, idx, marker))
      goto done;
  } else {
    if (streams[type].current_marker == -1) {
      streams[type].current_marker = marker;
      insanity_test_printf (test, "%d: Found initial marker\n", type);
    } else if (streams[type].current_marker != marker) {
      insanity_test_printf (test, "%d: Unexpected stream switch\n", type);
      stream_switch_constant_correct = FALSE;
      insanity_test_validate_checklist_item (test, "streams-constant", FALSE,
          "Streams switched although not waiting for a stream switch");
      streams[type].current_marker = marker;
    }
  }

  if (!streams[STREAM_TYPE_AUDIO].wait_switch &&
      !streams[STREAM_TYPE_VIDEO].wait_switch &&
      !streams[STREAM_TYPE_TEXT].wait_switch) {
    insanity_test_printf (test, "No stream switches pending, next switch\n");
    do_next_switch (test);
  }

done:
  TEST_UNLOCK ();

  return TRUE;
}

static gboolean
stream_switch_test_reached_initial_state (InsanityGstPipelineTest * test)
{
  GstElement *e;
  gboolean error = FALSE;
  size_t n;
  static const char *const sink_names[] = { "asink", "vsink", "tsink" };

  TEST_LOCK ();
  stream_switch_timeout_id =
      g_timeout_add_seconds (SWITCH_TIMEOUT,
      (GSourceFunc) stream_switch_timeout, test);

  /* Look for sinks and add probes */
  nsinks = 0;
  for (n = 0; n < G_N_ELEMENTS (sink_names); n++) {
    e = gst_bin_get_by_name (GST_BIN (pipeline), sink_names[n]);
    if (e) {
      gboolean ok = insanity_gst_test_add_data_probe (INSANITY_GST_TEST (test),
          GST_BIN (pipeline), sink_names[n], "sink", &sinks[nsinks],
          &probes[nsinks],
          &probe, NULL, NULL);
      if (ok) {
        nsinks++;
      } else {
        insanity_test_validate_checklist_item (INSANITY_TEST (test),
            "install-probes", FALSE, "Failed to attach probe to fakesink");
        error = TRUE;
      }
      gst_object_unref (e);
    }
  }

  if (!error) {
    insanity_test_validate_checklist_item (INSANITY_TEST (test),
        "install-probes", nsinks > 0, NULL);
  }

  if (nsinks == 0) {
    insanity_test_done (INSANITY_TEST (test));
    TEST_UNLOCK ();
    return FALSE;
  }

  do_next_switch (INSANITY_TEST (test));

  TEST_UNLOCK ();

  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;
  GValue vdef = { 0 };

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("stream-switch-test",
      "Tests stream switching inside playbin2", NULL);
  test = INSANITY_TEST (ptest);

  g_value_init (&vdef, G_TYPE_UINT);
  g_value_set_uint (&vdef, 0);
  insanity_test_add_argument (test, "seed",
      "A random seed to generate random switch targets",
      "0 means a randomly chosen seed; the seed will be saved as extra-info",
      TRUE, &vdef);
  g_value_unset (&vdef);

  insanity_test_add_checklist_item (test, "install-probes",
      "Probes were installed on the sinks", NULL, FALSE);
  insanity_test_add_checklist_item (test, "found-all-streams",
      "Streams were successfully detected by playbin2", NULL, FALSE);
  insanity_test_add_checklist_item (test, "stream-switch",
      "Streams were successfully switched", NULL, FALSE);
  insanity_test_add_checklist_item (test, "streams-constant",
      "Streams didn't switch without requesting", NULL, FALSE);
  insanity_test_add_checklist_item (test, "unique-markers",
      "Streams all had unique markers", NULL, FALSE);

  insanity_test_add_extra_info (test, "max-stream-switch-time",
      "The maximum amount of time taken to perform a stream switch (in nanoseconds)");
  insanity_test_add_extra_info (test, "seed",
      "The seed used to generate random stream switches");

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &stream_switch_test_create_pipeline, NULL, NULL);
  g_signal_connect_after (test, "setup", G_CALLBACK (&stream_switch_test_setup),
      NULL);
  g_signal_connect (test, "start", G_CALLBACK (&stream_switch_test_start),
      NULL);
  g_signal_connect_after (test, "stop", G_CALLBACK (&stream_switch_test_stop),
      NULL);
  g_signal_connect_after (test, "reached-initial-state",
      G_CALLBACK (&stream_switch_test_reached_initial_state), NULL);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
