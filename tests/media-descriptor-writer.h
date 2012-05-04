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

#ifndef MEDIA_DESCRIPTOR_WRITER_h
#define MEDIA_DESCRIPTOR_WRITER_h

#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>

G_BEGIN_DECLS

GType media_descriptor_writer_get_type (void);

#define MEDIA_DESCRIPTOR_WRITER_TYPE            (media_descriptor_writer_get_type ())
#define MEDIA_DESCRIPTOR_WRITER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MEDIA_DESCRIPTOR_WRITER_TYPE, MediaDescriptorWriter))
#define MEDIA_DESCRIPTOR_WRITER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MEDIA_DESCRIPTOR_WRITER_TYPE, MediaDescriptorWriterClass))
#define IS_MEDIA_DESCRIPTOR_WRITER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MEDIA_DESCRIPTOR_WRITER_TYPE))
#define IS_MEDIA_DESCRIPTOR_WRITER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MEDIA_DESCRIPTOR_WRITER_TYPE))
#define MEDIA_DESCRIPTOR_WRITER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MEDIA_DESCRIPTOR_WRITER_TYPE, MediaDescriptorWriterClass))

typedef struct _MediaDescriptorWriterPrivate MediaDescriptorWriterPrivate;


typedef struct {
  GObject parent;

  MediaDescriptorWriterPrivate *priv;
} MediaDescriptorWriter;

typedef struct {

  GObjectClass parent;

} MediaDescriptorWriterClass;

MediaDescriptorWriter * media_descriptor_writer_new (InsanityTest * test,
                                                     const gchar *location,
                                                     GstClockTime duration,
                                                     gboolean seekable);

gchar * media_descriptor_writer_get_xml_path        (MediaDescriptorWriter *writer);

gboolean media_descriptor_writer_detects_frames     (MediaDescriptorWriter *writer);
GstClockTime media_descriptor_writer_get_duration   (MediaDescriptorWriter *writer);
gboolean media_descriptor_writer_get_seekable       (MediaDescriptorWriter * writer);

gboolean media_descriptor_writer_add_stream         (MediaDescriptorWriter *writer,
                                                     GstPad *pad);
gboolean media_descriptor_writer_add_taglist        (MediaDescriptorWriter *writer,
                                                     const GstTagList *taglist);
gboolean media_descriptor_writer_add_frame          (MediaDescriptorWriter *writer,
                                                     GstPad *pad,
                                                     GstBuffer *buf);
gboolean media_descriptor_writer_add_tags           (MediaDescriptorWriter *writer,
                                                     GstPad *pad,
                                                     GstTagList *taglist);
gboolean media_descriptor_writer_write              (MediaDescriptorWriter * writer,
                                                     const gchar * filename);


G_END_DECLS

#endif /* MEDIA_DESCRIPTOR_WRITER_h */
