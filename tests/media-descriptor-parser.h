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

#ifndef MEDIA_DESCRIPTOR_PARSER_h
#define MEDIA_DESCRIPTOR_PARSER_h

#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>

G_BEGIN_DECLS

GType media_descriptor_parser_get_type (void);

#define MEDIA_DESCRIPTOR_PARSER_TYPE            (media_descriptor_parser_get_type ())
#define MEDIA_DESCRIPTOR_PARSER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MEDIA_DESCRIPTOR_PARSER_TYPE, MediaDescriptorParser))
#define MEDIA_DESCRIPTOR_PARSER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MEDIA_DESCRIPTOR_PARSER_TYPE, MediaDescriptorParserClass))
#define IS_MEDIA_DESCRIPTOR_PARSER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MEDIA_DESCRIPTOR_PARSER_TYPE))
#define IS_MEDIA_DESCRIPTOR_PARSER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MEDIA_DESCRIPTOR_PARSER_TYPE))
#define MEDIA_DESCRIPTOR_PARSER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MEDIA_DESCRIPTOR_PARSER_TYPE, MediaDescriptorParserClass))

typedef struct _MediaDescriptorParserPrivate MediaDescriptorParserPrivate;


typedef struct {
  GObject parent;

  MediaDescriptorParserPrivate *priv;
} MediaDescriptorParser;

typedef struct {

  GObjectClass parent;

} MediaDescriptorParserClass;

MediaDescriptorParser * media_descriptor_parser_new (InsanityTest *test,
                                                     const gchar * xmlpath,
                                                     GError **error);

gchar * media_descriptor_parser_get_xml_path        (MediaDescriptorParser *parser);

gboolean media_descriptor_parser_detects_frames     (MediaDescriptorParser *parser);
GstClockTime media_descriptor_parser_get_duration   (MediaDescriptorParser *parser);
gboolean media_descriptor_parser_get_seekable       (MediaDescriptorParser * parser);

gboolean media_descriptor_parser_add_stream         (MediaDescriptorParser *parser,
                                                     GstPad *pad);
gboolean media_descriptor_parser_add_taglist        (MediaDescriptorParser *parser,
                                                     GstTagList *taglist);
gboolean media_descriptor_parser_all_stream_found   (MediaDescriptorParser *parser);
gboolean media_descriptor_parser_all_tags_found     (MediaDescriptorParser *parser);

gboolean media_descriptor_parser_add_frame          (MediaDescriptorParser *parser,
                                                     GstPad *pad,
                                                     GstBuffer *buf,
                                                     GstBuffer *expected);

GList * media_descriptor_parser_get_buffers         (MediaDescriptorParser * parser,
                                                     GstPad *pad,
                                                     GCompareFunc compare_func);

GList * media_descriptor_parser_get_pads           (MediaDescriptorParser * parser);

G_END_DECLS

#endif /* MEDIA_DESCRIPTOR_PARSER_h */
