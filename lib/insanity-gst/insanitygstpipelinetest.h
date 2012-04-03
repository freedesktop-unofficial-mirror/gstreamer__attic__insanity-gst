/* Insanity QA system

       insanitygstpipelinetest.h

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

#ifndef INSANITY_GST_PIPELINE_TEST_H_GUARD
#define INSANITY_GST_PIPELINE_TEST_H_GUARD

#include <glib.h>
#include <glib-object.h>
#include <gst/gst.h>

#include <insanity/insanitydefs.h>
#include <insanity-gst/insanitygsttest.h>

typedef struct _InsanityGstPipelineTest InsanityGstPipelineTest;
typedef struct _InsanityGstPipelineTestClass InsanityGstPipelineTestClass;
typedef struct _InsanityGstPipelineTestPrivateData InsanityGstPipelineTestPrivateData;
typedef GstPipeline *(*InsanityGstCreatePipelineFunction) (InsanityGstPipelineTest*, gpointer userdata);

/**
 * InsanityGstPipelineTest:
 *
 * The opaque #InsanityGstPipelineTest data structure.
 */
struct _InsanityGstPipelineTest {
  InsanityGstTest parent;

  /*< private >*/
  InsanityGstPipelineTestPrivateData *priv;

  gpointer _insanity_reserved[INSANITY_PADDING];
};

/**
 * InsanityGstPipelineTestClass:
 * @parent_class: the parent class structure
 * @test: Start the test
 *
 * Insanity GStreamer test class. Override the vmethods to customize
 * functionality.
 */
struct _InsanityGstPipelineTestClass
{
  InsanityGstTestClass parent_class;

  /*< public >*/
  /* vtable */
  GstPipeline *(*create_pipeline) (InsanityGstPipelineTest *test);
  gboolean (*bus_message) (InsanityGstPipelineTest *test, GstMessage *msg);
  gboolean (*reached_initial_state) (InsanityGstPipelineTest *test);

  /*< private >*/
  gpointer _insanity_reserved[INSANITY_PADDING];
};

InsanityGstPipelineTest *insanity_gst_pipeline_test_new(const char *name, const char *description, const char *full_description);

void insanity_gst_pipeline_test_set_initial_state (InsanityGstPipelineTest *test, GstState state);
void insanity_gst_pipeline_test_set_live (InsanityGstPipelineTest *test, gboolean live);
void insanity_gst_pipeline_test_enable_buffering (InsanityGstPipelineTest *test, gboolean buffering);
void insanity_gst_pipeline_test_set_create_pipeline_function (InsanityGstPipelineTest *test, InsanityGstCreatePipelineFunction func, gpointer userdata, GDestroyNotify dnotify);
GstClockTime insanity_gst_pipeline_test_query_duration(InsanityGstPipelineTest *test);

/* Handy macros */
#define INSANITY_TYPE_GST_PIPELINE_TEST                (insanity_gst_pipeline_test_get_type ())
#define INSANITY_GST_PIPELINE_TEST(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTest))
#define INSANITY_GST_PIPELINE_TEST_CLASS(c)            (G_TYPE_CHECK_CLASS_CAST ((c), INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTestClass))
#define INSANITY_IS_GST_PIPELINE_TEST(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj), INSANITY_TYPE_GST_PIPELINE_TEST))
#define INSANITY_IS_GST_PIPELINE_TEST_CLASS(c)         (G_TYPE_CHECK_CLASS_TYPE ((c), INSANITY_TYPE_GST_PIPELINE_TEST))
#define INSANITY_GST_PIPELINE_TEST_GET_CLASS(obj)      (G_TYPE_INSTANCE_GET_CLASS ((obj), INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTestClass))

GType insanity_gst_pipeline_test_get_type (void);

#endif

