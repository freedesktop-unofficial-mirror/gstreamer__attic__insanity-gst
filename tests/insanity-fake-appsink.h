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

#ifndef INSANITY_FAKE_APPSINK_H
#define INSANITY_FAKE_APPSINK_H

#include <gst/gst.h>

G_BEGIN_DECLS

guint64
insanity_fake_appsink_get_buffers_received (GstElement * sink);

gboolean
insanity_fake_appsink_check_bufcount (GstElement * sink);

GstElement *
insanity_fake_appsink_new (const gchar * name, InsanityTest * test);

G_END_DECLS

#endif
