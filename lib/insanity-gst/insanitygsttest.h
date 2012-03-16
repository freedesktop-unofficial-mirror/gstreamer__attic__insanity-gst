/* Insanity QA system

       insanitygsttest.h

 Copyright (c) 2012, Collabora Ltd <slomo@collabora.co.uk>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this program; if not, write to the
 Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 Boston, MA 02111-1307, USA.
*/

#ifndef INSANITY_GST_TEST_H_GUARD
#define INSANITY_GST_TEST_H_GUARD

#include <glib.h>
#include <glib-object.h>
#include <gst/gst.h>

#include <insanity/insanitydefs.h>
#include <insanity/insanitythreadedtest.h>

typedef struct _InsanityGstTest InsanityGstTest;
typedef struct _InsanityGstTestClass InsanityGstTestClass;
typedef struct _InsanityGstTestPrivateData InsanityGstTestPrivateData;

/**
 * InsanityGstTest:
 *
 * The opaque #InsanityGstTest data structure.
 */
struct _InsanityGstTest {
  InsanityThreadedTest parent;

  /*< private >*/
  InsanityGstTestPrivateData *priv;

  gpointer _insanity_reserved[INSANITY_PADDING];
};

/**
 * InsanityGstTestClass:
 * @parent_class: the parent class structure
 * @test: Start the test
 *
 * Insanity GStreamer test class. Override the vmethods to customize
 * functionality.
 */
struct _InsanityGstTestClass
{
  InsanityThreadedTestClass parent_class;

  /*< private >*/
  gpointer _insanity_reserved[INSANITY_PADDING];
};

InsanityGstTest *insanity_gst_test_new(const char *name, const char *description, const char *full_description);

/* Handy macros */
#define INSANITY_TYPE_GST_TEST                (insanity_gst_test_get_type ())
#define INSANITY_GST_TEST(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), INSANITY_TYPE_GST_TEST, InsanityGstTest))
#define INSANITY_GST_TEST_CLASS(c)            (G_TYPE_CHECK_CLASS_CAST ((c), INSANITY_TYPE_GST_TEST, InsanityGstTestClass))
#define INSANITY_IS_GST_TEST(obj)             (G_TYPE_CHECK_TYPE ((obj), INSANITY_TYPE_GST_TEST))
#define INSANITY_IS_GST_TEST_CLASS(c)         (G_TYPE_CHECK_CLASS_TYPE ((c), INSANITY_TYPE_GST_TEST))
#define INSANITY_GST_TEST_GET_CLASS(obj)      (G_TYPE_INSTANCE_GET_CLASS ((obj), INSANITY_TYPE_GST_TEST, InsanityGstTestClass))

unsigned int insanity_gst_test_add_fakesink_probe (InsanityGstTest *test, GstBin *bin,
    gboolean (*probe) (GstPad *, GstMiniObject *, gpointer),
    GstPad ***pads, gulong **probes);
void insanity_gst_test_remove_fakesink_probe (InsanityGstTest *test, unsigned int nprobes,
    GstPad **pads, gulong *probes);

GType insanity_gst_test_get_type (void);

#endif

