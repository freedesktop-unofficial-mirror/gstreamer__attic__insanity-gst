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

#include "media-descriptor-writer.h"
#include "media-descriptor-common.h"
#include <string.h>

G_DEFINE_TYPE (MediaDescriptorWriter, media_descriptor_writer, G_TYPE_OBJECT);

#define LOG(test, format, args...) \
  INSANITY_LOG (test, "mediadescwriter", INSANITY_LOG_LEVEL_DEBUG, format, ##args)
#define ERROR(test, format, args...) \
  INSANITY_LOG (test, "mediadescwriter", INSANITY_LOG_LEVEL_SPAM, format, ##args)

#define STR_APPEND(arg, nb_white)  \
  tmpstr = res; \
  res = g_strdup_printf ("%s%*s%s%s", res, (nb_white), " ", (arg), "\n"); \
  g_free (tmpstr);

#define STR_APPEND0(arg) STR_APPEND((arg), 0)
#define STR_APPEND1(arg) STR_APPEND((arg), 2)
#define STR_APPEND2(arg) STR_APPEND((arg), 4)
#define STR_APPEND3(arg) STR_APPEND((arg), 6)

enum
{
  PROP_0,
  PROP_PATH,
  N_PROPERTIES
};

struct _MediaDescriptorWriterPrivate
{
  InsanityTest *test;

  FileNode *filenode;

  GList *serialized_string;
  guint stream_id;
};

static void
finalize (MediaDescriptorWriter * parser)
{
  MediaDescriptorWriterPrivate *priv;

  priv = parser->priv;

  if (priv->filenode)
    free_filenode (priv->filenode);
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
media_descriptor_writer_init (MediaDescriptorWriter * writer)
{
  MediaDescriptorWriterPrivate *priv;

  writer->priv = priv = G_TYPE_INSTANCE_GET_PRIVATE (writer,
      MEDIA_DESCRIPTOR_WRITER_TYPE, MediaDescriptorWriterPrivate);

  priv->filenode = g_slice_new0 (FileNode);
  priv->test = NULL;
  priv->serialized_string = NULL;
  priv->stream_id = 0;
}

static void
media_descriptor_writer_class_init (MediaDescriptorWriterClass * self_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (self_class);

  g_type_class_add_private (self_class, sizeof (MediaDescriptorWriterPrivate));
  object_class->finalize = (void (*)(GObject * object)) finalize;
  object_class->get_property = get_property;
  object_class->set_property = set_property;
}

/* Private methods */
static gchar *
serialize_filenode (MediaDescriptorWriter * writer)
{
  gchar *res, *tmpstr;
  GList *tmp, *tmp2;
  FileNode *filenode = writer->priv->filenode;

  res = g_markup_printf_escaped ("<file duration=\"%" G_GUINT64_FORMAT
      "\" frame-detection=\"%i\" location=\"%s\" seekable=\"%i\">",
      filenode->duration, filenode->frame_detection, filenode->location,
      filenode->seekable);

  STR_APPEND1 ("<streams>");
  for (tmp = filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *snode = ((StreamNode *) tmp->data);

    STR_APPEND2 (snode->str_open);

    for (tmp2 = snode->frames; tmp2; tmp2 = tmp2->next) {
      STR_APPEND3 (((FrameNode *) tmp2->data)->str_open);
    }
    STR_APPEND2 (snode->str_close);
  }
  STR_APPEND1 ("</streams>");

  for (tmp = filenode->tags; tmp; tmp = tmp->next) {
    TagsNode *tagsnode = ((TagsNode *) tmp->data);

    STR_APPEND1 (tagsnode->str_open);
    for (tmp2 = tagsnode->tags; tmp2; tmp2 = tmp2->next) {
      STR_APPEND2 (((TagNode *) tmp2->data)->str_open);
    }
    STR_APPEND1 (tagsnode->str_close);
  }

  STR_APPEND0 (filenode->str_close);

  return res;
}

/* Public methods */
MediaDescriptorWriter *
media_descriptor_writer_new (InsanityTest * test,
    const gchar * location, GstClockTime duration, gboolean seekable)
{
  MediaDescriptorWriter *writer;
  FileNode *fnode;

  g_return_val_if_fail (INSANITY_IS_TEST (test), NULL);

  writer = g_object_new (MEDIA_DESCRIPTOR_WRITER_TYPE, NULL);
  writer->priv->test = test;

  fnode = writer->priv->filenode;
  fnode->location = g_strdup (location);
  fnode->duration = duration;
  fnode->seekable = seekable;
  fnode->str_open = NULL;

  fnode->str_close = g_markup_printf_escaped ("</file>");

  return writer;
}

gboolean
media_descriptor_writer_add_stream (MediaDescriptorWriter * writer,
    GstPad * pad)
{
  guint id = 0;
  GList *tmp;
  gboolean ret = FALSE;
  GstCaps *caps;
  gchar *capsstr = NULL, *padname = NULL;
  StreamNode *snode = NULL;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_WRITER (writer), FALSE);
  g_return_val_if_fail (writer->priv->filenode, FALSE);

  caps = gst_pad_get_caps (pad);
  for (tmp = writer->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *streamnode = (StreamNode *) tmp->data;

    if (streamnode->pad == pad) {
      goto done;
    }
    id++;
  }

  snode = g_slice_new0 (StreamNode);
  snode->frames = NULL;
  snode->cframe = NULL;

  snode->caps = gst_caps_ref (caps);
  snode->pad = gst_object_ref (pad);
  snode->id = id;

  capsstr = gst_caps_to_string (caps);
  padname = gst_pad_get_name (pad);
  snode->str_open =
      g_markup_printf_escaped
      ("<stream padname=\"%s\" caps=\"%s\" id=\"%i\">", padname, capsstr, id);

  snode->str_close = g_markup_printf_escaped ("</stream>");

  writer->priv->filenode->streams =
      g_list_prepend (writer->priv->filenode->streams, snode);

done:
  if (caps != NULL)
    gst_caps_unref (caps);
  g_free (capsstr);
  g_free (padname);

  return ret;
}

gboolean
media_descriptor_writer_add_taglist (MediaDescriptorWriter * writer,
    const GstTagList * taglist)
{
  gchar *str_str = NULL;
  TagsNode *tagsnode;
  TagNode *tagnode;
  GList *tmp, *tmptag;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_WRITER (writer), FALSE);
  g_return_val_if_fail (writer->priv->filenode, FALSE);
  g_return_val_if_fail (GST_IS_STRUCTURE (taglist), FALSE);

  for (tmp = writer->priv->filenode->tags; tmp; tmp = tmp->next) {
    tagsnode = (TagsNode *) tmp->data;

    for (tmptag = tagsnode->tags; tmptag; tmptag = tmptag->next) {
      if (tag_node_compare ((TagNode *) tmptag->data, taglist)) {
        LOG (writer->priv->test,
            "Tag already in... not adding again %" GST_PTR_FORMAT, taglist);
        return TRUE;
      }
    }
  }

  if (writer->priv->filenode->tags == NULL) {
    tagsnode = g_slice_new0 (TagsNode);
    tagsnode->str_open = g_markup_printf_escaped ("<tags>");
    tagsnode->str_close = g_markup_printf_escaped ("</tags>");
    writer->priv->filenode->tags =
        g_list_prepend (writer->priv->filenode->tags, tagsnode);
  } else {
    tagsnode = (TagsNode *) writer->priv->filenode->tags->data;
  }

  tagnode = g_slice_new0 (TagNode);
  tagnode->taglist = gst_tag_list_copy (taglist);
  gst_structure_remove_field (tagnode->taglist, "source-pad");
  str_str = gst_tag_list_to_string (tagnode->taglist);
  tagnode->str_open =
      g_markup_printf_escaped ("<tag content=\"%s\"/>", str_str);
  tagsnode->tags = g_list_prepend (tagsnode->tags, tagnode);

  g_free (str_str);

  return FALSE;
}

gboolean
media_descriptor_writer_add_frame (MediaDescriptorWriter * writer,
    GstPad * pad, GstBuffer * buf)
{
  GList *tmp;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_WRITER (writer), FALSE);
  g_return_val_if_fail (writer->priv->filenode, FALSE);

  writer->priv->filenode->frame_detection = TRUE;

  for (tmp = writer->priv->filenode->streams; tmp; tmp = tmp->next) {
    StreamNode *streamnode = (StreamNode *) tmp->data;

    if (streamnode->pad == pad) {
      guint id = g_list_length (streamnode->frames);
      FrameNode *fnode = g_slice_new0 (FrameNode);

      fnode->id = id;
      fnode->offset = GST_BUFFER_OFFSET (buf);
      fnode->offset_end = GST_BUFFER_OFFSET_END (buf);
      fnode->duration = GST_BUFFER_DURATION (buf);
      fnode->timestamp = GST_BUFFER_TIMESTAMP (buf);
      fnode->is_keyframe = (GST_BUFFER_FLAG_IS_SET (buf,
              GST_BUFFER_FLAG_DELTA_UNIT) == FALSE);

      fnode->str_open =
          g_markup_printf_escaped (" <frame duration=\"%" G_GUINT64_FORMAT
          "\" id=\"%i\" is-keyframe=\"%i\" offset=\"%" G_GUINT64_FORMAT
          "\" offset-end=\"%" G_GUINT64_FORMAT "\" timestamp=\"%"
          G_GUINT64_FORMAT "\" />", fnode->duration, id, fnode->is_keyframe,
          fnode->offset, fnode->offset_end, fnode->timestamp);

      fnode->str_close = NULL;

      streamnode->frames = g_list_append (streamnode->frames, fnode);
      return TRUE;
    }
  }

  return FALSE;
}


gboolean
media_descriptor_writer_write (MediaDescriptorWriter * writer,
    const gchar * filename)
{
  gboolean ret = FALSE;
  gchar *serialized;

  g_return_val_if_fail (IS_MEDIA_DESCRIPTOR_WRITER (writer), FALSE);
  g_return_val_if_fail (writer->priv->filenode, FALSE);

  serialized = serialize_filenode (writer);

  if (g_file_set_contents (filename, serialized, -1, NULL) == TRUE)
    ret = TRUE;


  g_free (serialized);

  return ret;
}
