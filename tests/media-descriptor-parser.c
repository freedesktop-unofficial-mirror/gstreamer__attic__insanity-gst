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

#include "media-descriptor-parser.h"
#include "media-descriptor-common.h"

G_DEFINE_TYPE (MediaDescriptorParser, media_descriptor_parser, G_TYPE_OBJECT);

#define LOG(test, format , args...) \
  INSANITY_LOG (test, "mediadescparser", INSANITY_LOG_LEVEL_DEBUG, format "\n", ##args)
#define ERROR(test, format, args...) \
  INSANITY_LOG (test, "mediadescparser", INSANITY_LOG_LEVEL_SPAM, format "\n", ##args)

enum
{
  PROP_0,
  PROP_PATH,
  N_PROPERTIES
};

struct _MediaDescriptorParserPrivate
{
  gchar *xmlpath;
  InsanityTest *test;

  gchar *xmlcontent;
  FileNode *filenode;
  GMarkupParseContext *parsecontext;
};

/* Private methods  and callbacks */
static gint
compare_frames (FrameNode * frm, FrameNode * frm1)
{
  if (frm->id < frm1->id)
    return -1;

  else if (frm->id == frm1->id)
    return 0;

  return 1;
}

static inline FileNode *
deserialize_filenode (const gchar ** names, const gchar ** values)
{
  gint i;
  FileNode *filenode = g_slice_new0 (FileNode);

  for (i = 0; names[i] != NULL; i++) {
    if (g_strcmp0 (names[i], "location") == 0)
      filenode->location = g_strdup (values[i]);
    else if (g_strcmp0 (names[i], "id") == 0)
      filenode->id = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "frame-detection") == 0)
      filenode->frame_detection = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "duration") == 0)
      filenode->duration = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "seekable") == 0)
      filenode->seekable = g_ascii_strtoull (values[i], NULL, 0);
  }

  return filenode;
}

static inline StreamNode *
deserialize_streamnode (const gchar ** names, const gchar ** values)
{
  gint i;
  StreamNode *streamnode = g_slice_new0 (StreamNode);

  for (i = 0; names[i] != NULL; i++) {
    if (g_strcmp0 (names[i], "id") == 0)
      streamnode->id = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "caps") == 0)
      streamnode->caps = gst_caps_from_string (values[i]);
    else if (g_strcmp0 (names[i], "padname") == 0)
      streamnode->padname = g_strdup (values[i]);
  }

  return streamnode;
}

static inline TagsNode *
deserialize_tagsnode (const gchar ** names, const gchar ** values)
{
  TagsNode *tagsnode = g_slice_new0 (TagsNode);

  return tagsnode;
}

static inline TagNode *
deserialize_tagnode (const gchar ** names, const gchar ** values)
{
  gint i;
  TagNode *tagnode = g_slice_new0 (TagNode);

  for (i = 0; names[i] != NULL; i++) {
    if (g_strcmp0 (names[i], "content") == 0)
      tagnode->taglist = gst_structure_from_string (values[i], NULL);
  }

  return tagnode;
}

static inline FrameNode *
deserialize_framenode (const gchar ** names, const gchar ** values)
{
  gint i;

  FrameNode *framenode = g_slice_new0 (FrameNode);

  for (i = 0; names[i] != NULL; i++) {
    if (g_strcmp0 (names[i], "id") == 0)
      framenode->id = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "offset") == 0)
      framenode->offset = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "offset-end") == 0)
      framenode->offset_end = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "duration") == 0)
      framenode->duration = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "timestamp") == 0)
      framenode->timestamp = g_ascii_strtoull (values[i], NULL, 0);
    else if (g_strcmp0 (names[i], "is-keyframe") == 0)
      framenode->is_keyframe = g_ascii_strtoull (values[i], NULL, 0);
  }

  framenode->buf = gst_buffer_new ();

  GST_BUFFER_OFFSET (framenode->buf) = framenode->offset;
  GST_BUFFER_OFFSET_END (framenode->buf) = framenode->offset_end;
  GST_BUFFER_DURATION (framenode->buf) = framenode->duration;
  GST_BUFFER_TIMESTAMP (framenode->buf) = framenode->timestamp;

  if (framenode->is_keyframe == FALSE)
    GST_BUFFER_FLAG_SET (framenode->buf, GST_BUFFER_FLAG_DELTA_UNIT);

  return framenode;
}


static inline gboolean
frame_node_compare (FrameNode * fnode, GstBuffer * buf, GstBuffer * expected)
{
  if (expected != NULL) {
    GST_BUFFER_OFFSET (expected) = fnode->offset;
    GST_BUFFER_OFFSET_END (expected) = fnode->offset_end;
    GST_BUFFER_DURATION (expected) = fnode->duration;
    GST_BUFFER_TIMESTAMP (expected) = fnode->timestamp;
    if (fnode->is_keyframe)
      GST_BUFFER_FLAG_SET (expected, GST_BUFFER_FLAG_DELTA_UNIT);
  }

  if ((fnode->offset == GST_BUFFER_OFFSET (buf) &&
          fnode->offset_end == GST_BUFFER_OFFSET_END (buf) &&
          fnode->duration == GST_BUFFER_DURATION (buf) &&
          fnode->timestamp == GST_BUFFER_TIMESTAMP (buf) &&
          fnode->is_keyframe == GST_BUFFER_FLAG_IS_SET (buf,
              GST_BUFFER_FLAG_DELTA_UNIT)) == FALSE) {
    return TRUE;
  }

  return FALSE;
}

static void
on_start_element_cb (GMarkupParseContext * context,
    const gchar * element_name, const gchar ** attribute_names,
    const gchar ** attribute_values, gpointer user_data, GError ** error)
{
  MediaDescriptorParserPrivate *priv;

  priv = MEDIA_DESCRIPTOR_PARSER (user_data)->priv;

  if (g_strcmp0 (element_name, "file") == 0) {
    priv->filenode = deserialize_filenode (attribute_names, attribute_values);
  } else if (g_strcmp0 (element_name, "stream") == 0) {
    priv->filenode->streams = g_list_prepend (priv->filenode->streams,
        deserialize_streamnode (attribute_names, attribute_values));
  } else if (g_strcmp0 (element_name, "frame") == 0) {
    StreamNode *streamnode = priv->filenode->streams->data;

    streamnode->cframe = streamnode->frames =
        g_list_insert_sorted (streamnode->frames,
        deserialize_framenode (attribute_names, attribute_values),
        (GCompareFunc) compare_frames);
  } else if (g_strcmp0 (element_name, "tags") == 0) {
    priv->filenode->tags = g_list_prepend (priv->filenode->tags,
        deserialize_tagsnode (attribute_names, attribute_values));
  } else if (g_strcmp0 (element_name, "tag") == 0) {
    TagsNode *tagsnode = priv->filenode->tags->data;

    tagsnode->tags = g_list_prepend (tagsnode->tags,
        deserialize_tagnode (attribute_names, attribute_values));
  }
}

static void
on_error_cb (GMarkupParseContext * context, GError * error, gpointer user_data)
{
  ERROR (MEDIA_DESCRIPTOR_PARSER (user_data)->priv->test,
      "Error parsing file: %s", error->message);
}

static const GMarkupParser content_parser = {
  on_start_element_cb,
  NULL,
  NULL,
  NULL,
  &on_error_cb
};

static gboolean
set_xml_path (MediaDescriptorParser * parser, const gchar * path,
    GError ** error)
{
  gsize xmlsize;
  GError *err = NULL;
  MediaDescriptorParserPrivate *priv = parser->priv;

  if (!g_file_get_contents (path, &priv->xmlcontent, &xmlsize, &err))
    goto failed;

  priv->xmlpath = g_strdup (path);
  priv->parsecontext = g_markup_parse_context_new (&content_parser,
      G_MARKUP_TREAT_CDATA_AS_TEXT, parser, NULL);

  if (g_markup_parse_context_parse (priv->parsecontext, priv->xmlcontent,
          xmlsize, &err) == FALSE)
    goto failed;

  return TRUE;

failed:
  g_propagate_error (error, err);
  return FALSE;
}

/* GObject standard vmethods */
static void
dispose (MediaDescriptorParser * parser)
{
}

static void
finalize (MediaDescriptorParser * parser)
{
  MediaDescriptorParserPrivate *priv;

  priv = parser->priv;

  g_free (priv->xmlpath);
  g_free (priv->xmlcontent);

  if (priv->filenode)
    free_filenode (priv->filenode);

  if (priv->parsecontext != NULL)
    g_markup_parse_context_free (priv->parsecontext);
}


static void
get_property (GObject * gobject, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      g_assert_not_reached ();
  }

}

static void
set_property (GObject * gobject, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      g_assert_not_reached ();
  }
}

static void
media_descriptor_parser_init (MediaDescriptorParser * parser)
{
  MediaDescriptorParserPrivate *priv;

  parser->priv = priv = G_TYPE_INSTANCE_GET_PRIVATE (parser,
      MEDIA_DESCRIPTOR_PARSER_TYPE, MediaDescriptorParserPrivate);

  priv->xmlpath = NULL;
  priv->filenode = NULL;
  priv->test = NULL;
}

static void
media_descriptor_parser_class_init (MediaDescriptorParserClass * self_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (self_class);

  g_type_class_add_private (self_class, sizeof (MediaDescriptorParserPrivate));
  object_class->dispose = (void (*)(GObject * object)) dispose;
  object_class->finalize = (void (*)(GObject * object)) finalize;
  object_class->get_property = get_property;
  object_class->set_property = set_property;
}

/* Public methods */
MediaDescriptorParser *
media_descriptor_parser_new (InsanityTest * test,
    const gchar * xmlpath, GError ** error)
{
  MediaDescriptorParser *parser;

  g_return_val_if_fail (INSANITY_IS_TEST (test), NULL);

  parser = g_object_new (MEDIA_DESCRIPTOR_PARSER_TYPE, NULL);
  parser->priv->test = test;

  if (set_xml_path (parser, xmlpath, error) == FALSE) {
    g_object_unref (parser);

    return NULL;
  }


  return parser;
}

gchar *
media_descriptor_parser_get_xml_path (MediaDescriptorParser * parser)
{
  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), NULL);

  return g_strdup (parser->priv->xmlpath);
}

gboolean
media_descriptor_parser_add_stream (MediaDescriptorParser * parser,
    GstPad * pad)
{
  GList *tmp;
  gboolean ret = FALSE;
  GstCaps *caps;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  caps = gst_pad_get_caps (pad);
  for (tmp = parser->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *streamnode = (StreamNode *) tmp->data;

    if (streamnode->pad == NULL && gst_caps_is_equal (streamnode->caps, caps)) {
      ret = TRUE;
      streamnode->pad = gst_object_ref (pad);

      goto done;
    }
  }

done:
  if (caps != NULL)
    gst_caps_unref (caps);

  return ret;
}

gboolean
media_descriptor_parser_all_stream_found (MediaDescriptorParser * parser)
{
  GList *tmp;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  for (tmp = parser->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *streamnode = (StreamNode *) tmp->data;

    if (streamnode->pad == NULL)
      return FALSE;

  }

  return TRUE;
}

gboolean
media_descriptor_parser_add_frame (MediaDescriptorParser * parser,
    GstPad * pad, GstBuffer * buf, GstBuffer * expected)
{
  GList *tmp;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  for (tmp = parser->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *streamnode = (StreamNode *) tmp->data;

    if (streamnode->pad == pad && streamnode->cframe) {
      FrameNode *fnode = streamnode->cframe->data;

      streamnode->cframe = streamnode->cframe->next;
      return frame_node_compare (fnode, buf, expected);
    }
  }

  return FALSE;
}

gboolean
media_descriptor_parser_add_taglist (MediaDescriptorParser * parser,
    GstTagList * taglist)
{
  GList *tmp, *tmptag;
  TagsNode *tagsnode;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);
  g_return_val_if_fail (GST_IS_STRUCTURE (taglist), FALSE);

  for (tmp = parser->priv->filenode->tags; tmp; tmp = tmp->next) {
    tagsnode = (TagsNode *) tmp->data;

    for (tmptag = tagsnode->tags; tmptag; tmptag = tmptag->next) {
      if (tag_node_compare ((TagNode *) tmptag->data, taglist)) {
        LOG (parser->priv->test, "Adding tag %" GST_PTR_FORMAT, taglist);
        return TRUE;
      }
    }
  }

  return FALSE;
}

gboolean
media_descriptor_parser_all_tags_found (MediaDescriptorParser * parser)
{
  GList *tmp, *tmptag;
  TagsNode *tagsnode;
  gboolean ret = TRUE;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  for (tmp = parser->priv->filenode->tags; tmp; tmp = tmp->next) {
    tagsnode = (TagsNode *) tmp->data;

    for (tmptag = tagsnode->tags; tmptag; tmptag = tmptag->next) {
      gchar *tag = NULL;

      tag = gst_tag_list_to_string (((TagNode *) tmptag->data)->taglist);
      if (((TagNode *) tmptag->data)->found == FALSE) {

        if (((TagNode *) tmptag->data)->taglist != NULL) {
          LOG (parser->priv->test, "Tag not found %s", tag);
        } else {
          LOG (parser->priv->test, "Tag not not properly deserialized");
        }

        ret = FALSE;
      }

      LOG (parser->priv->test, "Tag properly found found %s", tag);
      g_free (tag);
    }
  }

  return ret;
}

gboolean
media_descriptor_parser_detects_frames (MediaDescriptorParser * parser)
{
  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  return parser->priv->filenode->frame_detection;
}

GstClockTime
media_descriptor_parser_get_duration (MediaDescriptorParser * parser)
{
  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  return parser->priv->filenode->duration;
}

gboolean
media_descriptor_parser_get_seekable (MediaDescriptorParser * parser)
{
  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  return parser->priv->filenode->seekable;
}

GList *
media_descriptor_parser_get_buffers (MediaDescriptorParser * parser,
    GstPad * pad, GCompareFunc compare_func)
{
  GList *ret = NULL, *tmpstream, *tmpframe;
  gboolean check = (pad == NULL);

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_PARSER (parser), FALSE);
  g_return_val_if_fail (parser->priv->filenode, FALSE);

  for (tmpstream = parser->priv->filenode->streams; tmpstream;
      tmpstream = tmpstream->next) {
    StreamNode *streamnode = (StreamNode *) tmpstream->data;

    if (pad && streamnode->pad == pad)
      check = TRUE;

    if (check) {
      for (tmpframe = streamnode->frames; tmpframe; tmpframe = tmpframe->next) {
        if (compare_func)
          ret =
              g_list_insert_sorted (ret,
              gst_buffer_ref (((FrameNode *) tmpframe->data)->buf),
              compare_func);
        else
          ret =
              g_list_prepend (ret,
              gst_buffer_ref (((FrameNode *) tmpframe->data)->buf));
      }

      if (pad != NULL)
        goto done;
    }
  }


done:
  return ret;
}

GList *
media_descriptor_parser_get_pads (MediaDescriptorParser * parser)
{
  GList *ret = NULL, *tmp;

  for (tmp = parser->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *snode = (StreamNode *) tmp->data;
    ret = g_list_append (ret, gst_pad_new (snode->padname, GST_PAD_UNKNOWN));
  }

  return ret;
}
