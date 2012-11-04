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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef INSANITY_HTTP_SERVER_H
#define INSANITY_HTTP_SERVER_H

#include <glib.h>
#include <gio/gio.h>
#include <glib-object.h>

#include <insanity-gst/insanity-gst.h>
#include <libsoup/soup-server.h>

G_BEGIN_DECLS

GType insanity_http_server_get_type (void);

#define INSANITY_TYPE_HTTP_SERVER                (insanity_http_server_get_type ())
#define INSANITY_HTTP_SERVER(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), INSANITY_TYPE_HTTP_SERVER, InsanityHttpServer))
#define INSANITY_HTTP_SERVER_CLASS(c)            (G_TYPE_CHECK_CLASS_CAST ((c), INSANITY_TYPE_HTTP_SERVER, InsanityHttpServerClass))
#define INSANITY_IS_HTTP_SERVER(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj), INSANITY_TYPE_HTTP_SERVER))
#define INSANITY_IS_HTTP_SERVER_CLASS(c)         (G_TYPE_CHECK_CLASS_TYPE ((c), INSANITY_TYPE_HTTP_SERVER))
#define INSANITY_HTTP_SERVER_GET_CLASS(obj)      (G_TYPE_INSTANCE_GET_CLASS ((obj), INSANITY_TYPE_HTTP_SERVER, InsanityHttpServerClass))

/* SSL setup */
extern const char *good_user;
extern const char *bad_user;
extern const char *good_pw;
extern const char *bad_pw;
extern const char *realm;
extern const char *basic_auth_path;
extern const char *digest_auth_path;

typedef struct _InsanityHttpServer InsanityHttpServer;
typedef struct _InsanityHttpServerClass InsanityHttpServerClass;
typedef struct _InsanityHttpServerPrivate InsanityHttpServerPrivate;

struct _InsanityHttpServer
{
  GObject parent;

  /*< private >*/
  InsanityHttpServerPrivate *priv;
};

struct _InsanityHttpServerClass
{
  GObjectClass parent_class;

  /*< private >*/
  gpointer _insanity_reserved[INSANITY_PADDING];
};

InsanityHttpServer *
insanity_http_server_new                 (InsanityTest *test);

gboolean
insanity_http_server_stop                (InsanityHttpServer *srv);

void insanity_http_server_reset          (InsanityHttpServer *srv);

gboolean
insanity_http_server_start               (InsanityHttpServer * srv,
                                          const char *ssl_cert_file,
                                          const char *ssl_key_file);

gboolean
insanity_http_server_start_async         (InsanityHttpServer *srv,
                                          GAsyncReadyCallback callback,
                                          const char *ssl_cert_file,
                                          const char *ssl_key_file,
                                          gpointer user_data);

gboolean
insanity_http_server_is_running          (InsanityHttpServer *srv);

guint insanity_http_server_get_port      (InsanityHttpServer *srv);

guint
insanity_http_server_get_ssl_port        (InsanityHttpServer *srv);

const char *
insanity_http_server_get_source_folder   (InsanityHttpServer *srv);

void
insanity_http_server_set_source_folder   (InsanityHttpServer *srv,
                                          const gchar * source_folder);

SoupServer *
insanity_http_server_get_soup_server     (InsanityHttpServer *srv);

SoupServer *
insanity_http_server_get_soup_ssl_server (InsanityHttpServer *srv);

G_END_DECLS

#endif /* INSANITY_GST_H_GUARD */
