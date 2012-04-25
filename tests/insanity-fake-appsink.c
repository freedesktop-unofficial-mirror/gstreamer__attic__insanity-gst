/**
 * Insanity QA system
 *
 * Copyright (c) 2012, Collabora Ltd
 *    Author: Vivia Nikolaidou <vivia.nikolaidou@collabora.com>
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

#include <gst/gst.h>
#include <insanity-gst/insanity-gst.h>
#include "insanity-fake-appsink.h"

#define LOG(format, args...) \
  INSANITY_LOG (app->test, "appsink", INSANITY_LOG_LEVEL_DEBUG, format, ##args)

typedef struct _App App;

struct _App
{
  GstElement *appsink;

  InsanityTest *test;

  guint64 bufcount;
  guint64 cbcount;
};

static void
on_new_buffer (GstElement * appsink, gpointer userdata)
{

  GstBuffer *buffer = NULL;
  App *app = NULL;

  app = (App *) g_object_get_qdata ((GObject *) appsink,
      g_quark_from_static_string ("app"));

  g_signal_emit_by_name (appsink, "pull-buffer", &buffer);
  if (buffer) {
    gst_buffer_unref (buffer);
    app->bufcount++;
  }

}

static gboolean
cb_have_data (GstPad * pad, GstBuffer * buffer, gpointer u_data)
{

  App *app = NULL;
  GstElement *appsink = NULL;

  appsink = gst_pad_get_parent_element (pad);

  app = (App *) g_object_get_qdata ((GObject *) appsink,
      g_quark_from_static_string ("app"));

  app->cbcount++;

  gst_object_unref (appsink);

  return TRUE;

}

static void
cleanup (App * app)
{

  g_free (app);

}

guint64
insanity_fake_appsink_get_buffers_received (GstElement * sink)
{

  App *app;

  app = (App *) g_object_get_qdata ((GObject *) sink,
      g_quark_from_static_string ("app"));
  return app->bufcount;

}

gboolean
insanity_fake_appsink_check_bufcount (GstElement * sink)
{

  App *app;

  app = (App *) g_object_get_qdata ((GObject *) sink,
      g_quark_from_static_string ("app"));
  LOG ("Element %s: Received %" G_GUINT64_FORMAT " buffers (probe says %"
      G_GUINT64_FORMAT ")\n", GST_OBJECT_NAME (sink),
      app->bufcount, app->cbcount);
  return (app->bufcount == app->cbcount);

}

GstElement *
insanity_fake_appsink_new (const gchar * name, InsanityTest * test)
{

  GstElement *appsink;
  App *app;
  GstPad *pad;

  appsink = gst_element_factory_make ("appsink", name);
  g_object_set (G_OBJECT (appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect (appsink, "new-buffer", G_CALLBACK (on_new_buffer), NULL);

  app = g_new0 (App, 1);
  app->bufcount = 0;
  app->cbcount = 0;
  app->appsink = appsink;
  app->test = test;

  pad = gst_element_get_pad (appsink, "sink");
  gst_pad_add_buffer_probe (pad, G_CALLBACK (cb_have_data), NULL);
  gst_object_unref (pad);

  g_object_set_qdata_full ((GObject *) appsink,
      g_quark_from_static_string ("app"), app, (GDestroyNotify) cleanup);

  return appsink;

}
