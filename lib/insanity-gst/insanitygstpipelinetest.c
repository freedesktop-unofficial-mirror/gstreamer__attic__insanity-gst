/* Insanity QA system

       insanitygstpipelinetest.c

 Copyright (c) 2012, Collabora Ltd <vincent@collabora.co.uk>

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
/**
 * SECTION:insanitygstpipelinetest
 * @short_description: GStreamer Pipeline Test
 * @see_also: #InsanityTest, #InsanityGstTest
 *
 * %TODO.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <gst/gst.h>

#include <insanity-gst/insanitygstpipelinetest.h>

G_DEFINE_TYPE (InsanityGstPipelineTest, insanity_gst_pipeline_test,
    INSANITY_TYPE_GST_TEST);

struct _InsanityGstPipelineTestPrivateData
{
  GstPipeline *pipeline;
};

static gboolean
insanity_gst_pipeline_test_setup (InsanityTest *test)
{
  InsanityGstPipelineTestPrivateData *priv = INSANITY_GST_PIPELINE_TEST (test)->priv;

  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->setup (test))
    return FALSE;

  printf("insanity_gst_pipeline_test_setup\n");

  priv->pipeline = GST_PIPELINE (gst_pipeline_new ("test-pipeline"));

  return TRUE;
}

static gboolean
insanity_gst_pipeline_test_start (InsanityTest *test)
{
  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->start (test))
    return FALSE;

  printf("insanity_gst_pipeline_test_start\n");

  return TRUE;
}

static void
insanity_gst_pipeline_test_stop (InsanityTest *test)
{
  printf("insanity_gst_pipeline_test_stop\n");

  INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->stop (test);
}

static void
insanity_gst_pipeline_test_teardown (InsanityTest *test)
{
  InsanityGstPipelineTestPrivateData *priv = INSANITY_GST_PIPELINE_TEST (test)->priv;

  printf("insanity_gst_pipeline_test_teardown\n");

  gst_object_unref (priv->pipeline);

  INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->teardown (test);
}

static void
insanity_gst_pipeline_test_init (InsanityGstPipelineTest * gsttest)
{
  InsanityTest *test = INSANITY_TEST (gsttest);
  InsanityGstPipelineTestPrivateData *priv = G_TYPE_INSTANCE_GET_PRIVATE (gsttest,
      INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTestPrivateData);

  gsttest->priv = priv;

  /* Add our own items, etc */
}

static void
insanity_gst_pipeline_test_class_init (InsanityGstPipelineTestClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  InsanityTestClass *test_class = INSANITY_TEST_CLASS (klass);

  test_class->setup = &insanity_gst_pipeline_test_setup;
  test_class->start = &insanity_gst_pipeline_test_start;
  test_class->stop = &insanity_gst_pipeline_test_stop;
  test_class->teardown = &insanity_gst_pipeline_test_teardown;

  g_type_class_add_private (klass, sizeof (InsanityGstPipelineTestPrivateData));
}

/**
 * insanity_gst_pipeline_test_new:
 * @name: the short name of the test.
 * @description: a one line description of the test.
 * @full_description: (allow-none): an optional longer description of the test.
 *
 * This function creates a new GStreamer pipeline test with the given properties.
 *
 * Returns: (transfer full): a new #InsanityGstPipelineTest instance.
 */
InsanityGstPipelineTest *
insanity_gst_pipeline_test_new (const char *name, const char *description, const char *full_description)
{
  InsanityGstPipelineTest *test = g_object_new (insanity_gst_pipeline_test_get_type (),
      "name", name, "description", description, NULL);
  if (full_description)
    g_object_set (test, "full-description", full_description, NULL);
  return test;
}
