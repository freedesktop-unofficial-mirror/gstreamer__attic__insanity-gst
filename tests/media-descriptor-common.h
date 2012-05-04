/**
 * Insanity QA system
 *
 * Copyright (c) 2012, Collabora Ltd
 *    Author: Thibault Saunier <thibault.saunier@collabora.com>
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

#ifndef MEDIA_DESCRIPTOR_COMMON_H
#define MEDIA_DESCRIPTOR_COMMON_H

#include <glib.h>
#include <insanity-gst/insanity-gst.h>

/* Parsing structures */
typedef struct
{
  /* Children */
  /* StreamNode */
  GList *streams;
  /* TagsNode */
  GList *tags;

  /* attributes */
  guint64 id;
  gchar *location;
  GstClockTime duration;
  gboolean frame_detection;
  gboolean seekable;


  gchar *str_open;
  gchar *str_close;
} FileNode;

typedef struct
{
  /* Children */
  /* TagNode */
  GList *tags;

  gchar *str_open;
  gchar *str_close;
} TagsNode;

typedef struct
{
  /* Children */
  GstTagList *taglist;

  /* Testing infos */
  gboolean found;

  gchar *str_open;
  gchar *str_close;
} TagNode;

typedef struct
{
  /* Children */
  /* FrameNode */
  GList *frames;

  /* Attributes */
  GstCaps *caps;
  guint64 id;
  gchar *padname;

  /* Testing infos */
  GstPad *pad;
  GList *cframe;

  gchar *str_open;
  gchar *str_close;
} StreamNode;

typedef struct
{
  /* Attributes */
  guint64 id;
  guint64 offset;
  guint64 offset_end;
  GstClockTime duration;
  GstClockTime timestamp;
  gboolean is_keyframe;

  GstBuffer *buf;

  gchar *str_open;
  gchar *str_close;
} FrameNode;

void free_filenode (FileNode * filenode);
gboolean tag_node_compare (TagNode * tnode, const GstTagList * tlist);

#endif /* MEDIA_DESCRIPTOR_COMMON_H */
