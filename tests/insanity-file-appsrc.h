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

#ifndef INSANITY_FILE_APPSRC_H
#define INSANITY_FILE_APPSRC_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

G_BEGIN_DECLS

void
insanity_file_appsrc_prepare (GstElement *appsrc, gchar *uri);

G_END_DECLS

#endif
