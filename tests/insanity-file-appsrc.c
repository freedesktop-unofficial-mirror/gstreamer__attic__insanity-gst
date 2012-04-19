/**
 * Insanity QA system
 *
 * Copyright (c) 2012, Collabora Ltd
 *    Author: Vivia Nikolaidou <vivia.nikolaidou@collabora.com>
 * appsrc code based on appsrc-stream2, appsrc-seekable and appsrc-ra examples
 * Copyright (C) 2008 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include <stdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

#include "insanity-file-appsrc.h"

typedef struct _App App;

struct _App
{
  GstElement *appsrc;

  GFileInputStream *stream;
  gint64 length;
  guint64 offset;
  gchar *mode;
};

/* This method is called by the need-data signal callback, we feed data into the
 * appsrc with an arbitrary size.
 */
static void
feed_data (GstElement * appsrc, guint size, App * app)
{
  GstBuffer *buffer;
  gsize len;
  GstFlowReturn ret;
  GError *error = NULL;

  if (app->offset >= app->length) {
    /* we are EOS, send end-of-stream */
    g_signal_emit_by_name (app->appsrc, "end-of-stream", &ret);
    return;
  }
  if (app->offset + size > app->length) {
    g_print ("Reached EOF\n");
    size = app->length - app->offset;
  }

  buffer = gst_buffer_new_and_alloc (size);

  if (g_str_has_prefix (app->mode, "random-access")) {
    /* read the amount of data, we are allowed to return less if we are EOS */
    g_input_stream_read_all (G_INPUT_STREAM (app->stream),
        GST_BUFFER_DATA (buffer), size, &len, NULL, &error);
    if (error) {
      g_print ("ERROR: %s\n", error->message);
      g_error_free (error);
    }
    /* we need to set an offset for random access */
    GST_BUFFER_OFFSET (buffer) = app->offset;
    GST_BUFFER_OFFSET_END (buffer) = app->offset + len;
  } else {
    /* read any amount of data, we are allowed to return less if we are EOS */
    len = g_input_stream_read (G_INPUT_STREAM (app->stream),
        GST_BUFFER_DATA (buffer), size, NULL, &error);
    if (error) {
      g_print ("ERROR: %s\n", error->message);
      g_error_free (error);
    }
  }

  GST_BUFFER_SIZE (buffer) = len;

  if (error) {
    GST_DEBUG ("Cannot read file: %s\n", error->message);
    /* probably explode? */
    g_error_free (error);
    return;
  }

  GST_DEBUG ("feed buffer %p, offset %" G_GUINT64_FORMAT "-%u", buffer,
      app->offset, len);
  g_signal_emit_by_name (app->appsrc, "push-buffer", buffer, &ret);
  gst_buffer_unref (buffer);

  app->offset += len;

  return;
}

/* called when appsrc wants us to return data from a new position with the next
 * call to push-buffer. */
static gboolean
seek_data (GstElement * appsrc, guint64 position, App * app)
{

  GError *error = NULL;

  GST_DEBUG ("seek to offset %" G_GUINT64_FORMAT, position);
  app->offset = position;
  g_seekable_seek (G_SEEKABLE (app->stream), position, G_SEEK_SET, NULL,
      &error);

  if (error) {
    GST_DEBUG ("Cannot seek: %s\n", error->message);
    g_error_free (error);
    return FALSE;
  }

  return TRUE;
}

static void
cleanup (App * app)
{

  g_object_unref (app->stream);
  g_free (app->mode);
  g_free (app);

}

void
insanity_file_appsrc_prepare (GstElement * appsrc, gchar * uri)
{

  App *app;
  GError *error = NULL;
  gchar *filename = NULL;
  gchar **dump = NULL;
  GFileInfo *info = NULL;
  GFile *file = NULL;

  app = g_new0 (App, 1);

  filename = uri;
  while (!g_str_has_prefix (filename, "/")) {
    filename++;
  }

  dump = g_strsplit (uri + 7, ":", 2);
  app->mode = g_strdup (dump[0]);
  g_strfreev (dump);
  app->appsrc = appsrc;

  GST_DEBUG ("Opening file %s\n", filename);

  /* try to open the file as an mmapped file */
  file = g_file_new_for_path (filename);
  app->stream = g_file_read (file, NULL, &error);
  if (error) {
    GST_DEBUG ("failed to open file: %s\n", error->message);
    g_error_free (error);
    g_object_unref (file);
    cleanup (app);
    return;
  }

  g_object_set_qdata_full ((GObject *) appsrc,
      g_quark_from_static_string ("app"), app, (GDestroyNotify) cleanup);

  /* get some vitals, this will be used to read data from the mmapped file and
   * feed it to appsrc. */
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (error) {
    GST_DEBUG ("failed to get file size: %s\n", error->message);
    g_error_free (error);
  } else {
    app->length = g_file_info_get_size (info);
    /* we can set the length in appsrc. This allows some elements to estimate the
     * total duration of the stream. It's a good idea to set the property when you
     * can but it's not required. */
    g_object_set (app->appsrc, "size", app->length, NULL);
  }
  app->offset = 0;

  g_object_unref (info);

  /* we are seekable in push mode, this means that the element usually pushes
   * out buffers of an undefined size and that seeks happen only occasionally
   * and only by request of the user. */
  if (g_str_has_prefix (app->mode, "stream")) {
    gst_util_set_object_arg (G_OBJECT (app->appsrc), "stream-type", "stream");
  } else if (g_str_has_prefix (app->mode, "seekable")) {
    gst_util_set_object_arg (G_OBJECT (app->appsrc), "stream-type", "seekable");
  } else if (g_str_has_prefix (app->mode, "random-access")) {
    gst_util_set_object_arg (G_OBJECT (app->appsrc), "stream-type",
        "random-access");
  } else {
    g_assert_not_reached ();
  }

  /* configure the appsrc, we will push a buffer to appsrc when it needs more
   * data */
  g_signal_connect (app->appsrc, "need-data", G_CALLBACK (feed_data), app);
  if (!g_str_has_prefix (app->mode, "stream")) {
    g_signal_connect (app->appsrc, "seek-data", G_CALLBACK (seek_data), app);
  }

  g_object_unref (file);

}
