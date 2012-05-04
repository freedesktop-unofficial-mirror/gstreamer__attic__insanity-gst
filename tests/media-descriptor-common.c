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

#include "media-descriptor-common.h"

static inline void
free_tagnode (TagNode * tagnode)
{
  g_free (tagnode->str_open);
  g_free (tagnode->str_close);
  if (tagnode->taglist)
    gst_tag_list_free (tagnode->taglist);

  g_slice_free (TagNode, tagnode);
}

static inline void
free_tagsnode (TagsNode * tagsnode)
{
  g_free (tagsnode->str_open);
  g_free (tagsnode->str_close);
  g_list_free_full (tagsnode->tags, (GDestroyNotify) free_tagnode);
  g_slice_free (TagsNode, tagsnode);
}

static inline void
free_framenode (FrameNode * framenode)
{
  g_free (framenode->str_open);
  g_free (framenode->str_close);

  if (framenode->buf)
    gst_buffer_unref (framenode->buf);

  g_slice_free (FrameNode, framenode);
}

static inline void
free_streamnode (StreamNode * streamnode)
{
  if (streamnode->caps)
    gst_caps_unref (streamnode->caps);

  g_list_free_full (streamnode->frames, (GDestroyNotify) free_framenode);

  if (streamnode->pad)
    gst_object_unref (streamnode->pad);

  g_free (streamnode->padname);

  g_free (streamnode->str_open);
  g_free (streamnode->str_close);
  g_slice_free (StreamNode, streamnode);
}

void
free_filenode (FileNode * filenode)
{
  g_list_free_full (filenode->streams, (GDestroyNotify) free_streamnode);
  g_list_free_full (filenode->tags, (GDestroyNotify) free_tagsnode);

  g_free (filenode->str_open);
  g_free (filenode->str_close);

  g_slice_free (FileNode, filenode);
}

gboolean
tag_node_compare (TagNode * tnode, const GstTagList * tlist)
{
  if (gst_structure_is_equal (GST_STRUCTURE (tlist),
          GST_STRUCTURE (tnode->taglist)) == FALSE) {
    return FALSE;
  }

  tnode->found = TRUE;

  return TRUE;
}
