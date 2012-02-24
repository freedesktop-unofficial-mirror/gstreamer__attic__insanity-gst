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

static guint create_pipeline_signal;

G_DEFINE_TYPE (InsanityGstPipelineTest, insanity_gst_pipeline_test,
    INSANITY_TYPE_GST_TEST);

struct _InsanityGstPipelineTestPrivateData
{
  GstPipeline *pipeline;

  gboolean reached_initial_state;

  GHashTable *elements_used;
};

static void
destroy_trackers (InsanityGstPipelineTest *ptest)
{
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  if (priv->elements_used) {
    g_hash_table_destroy (priv->elements_used);
  }
}

static void
create_trackers (InsanityGstPipelineTest *ptest)
{
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  priv->elements_used = g_hash_table_new_full (&g_str_hash, &g_str_equal, &g_free, &g_free);
}

static void
add_element_used (InsanityGstPipelineTest *ptest, GstElement *element)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  const char *factory_name = factory ? gst_element_factory_get_longname (factory) : "(no factory)";
  g_hash_table_insert (ptest->priv->elements_used, gst_element_get_name (element), g_strdup(factory_name));
}

static void on_element_added (GstElement *bin, GstElement *element, InsanityGstPipelineTest *ptest);

static guint
watch_container (InsanityGstPipelineTest *ptest, GstBin *bin)
{
  GstIterator *it;
  gboolean done = FALSE;
  gpointer data;
  GstElement *e;

  it = gst_bin_iterate_elements (bin);
  while (!done) {
    switch (gst_iterator_next (it, &data)) {
      case GST_ITERATOR_OK:
        e = GST_ELEMENT_CAST (data);
        add_element_used (ptest, e);
        if (GST_IS_BIN (e)) {
          watch_container (ptest, GST_BIN (e));
        }
        gst_object_unref (e);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_DONE:
      default:
        done = TRUE;
        break;
    }
  }
  gst_iterator_free (it);

  return g_signal_connect (bin, "element-added", (GCallback) on_element_added, ptest);
}

static void
on_element_added (GstElement *bin, GstElement *element, InsanityGstPipelineTest *ptest)
{
  add_element_used (ptest, element);
  if (GST_IS_BIN (element))
    watch_container (ptest, GST_BIN (element));
}

static gboolean
insanity_gst_pipeline_test_setup (InsanityTest *test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->setup (test))
    return FALSE;

  printf("insanity_gst_pipeline_test_setup\n");

  priv->pipeline = NULL;
  g_signal_emit (ptest, create_pipeline_signal, 0, &priv->pipeline);
  insanity_test_validate_step (test, "valid-pipeline", priv->pipeline != NULL, NULL);
  if (!priv->pipeline)
    return FALSE;
  watch_container (ptest, GST_BIN (priv->pipeline));

  return TRUE;
}

static gboolean
insanity_gst_pipeline_test_start (InsanityTest *test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);

  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->start (test))
    return FALSE;

  printf("insanity_gst_pipeline_test_start\n");
  create_trackers (ptest);
  add_element_used (ptest, GST_ELEMENT (ptest->priv->pipeline));

  return TRUE;
}

static void
insanity_gst_pipeline_test_stop (InsanityTest *test)
{
  printf("insanity_gst_pipeline_test_stop\n");
  destroy_trackers (INSANITY_GST_PIPELINE_TEST (test));

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

static GstPipeline *
insanity_gst_pipeline_test_create_pipeline (InsanityGstPipelineTest *ptest)
{
  GstPipeline *pipeline = NULL;
  GValue launch_line = {0};
  GError *error = NULL;

  if (insanity_test_get_argument (INSANITY_TEST (ptest), "pipeline-launch-line", &launch_line)) {
    pipeline = GST_PIPELINE (gst_parse_launch (g_value_get_string (&launch_line), &error));
    g_value_unset (&launch_line);
    if (!pipeline) {
      insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
        error ? error->message : NULL);
      if (error)
        g_error_free (error);
    }
    else if (error) {
      /* Do we get a dangling pointer here ? gst-launch.c does not unref */
      pipeline = NULL;
      insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
        error->message);
      g_error_free (error);
    }
  }

  return pipeline;
}

static void
insanity_gst_pipeline_test_init (InsanityGstPipelineTest * gsttest)
{
  InsanityTest *test = INSANITY_TEST (gsttest);
  InsanityGstPipelineTestPrivateData *priv = G_TYPE_INSTANCE_GET_PRIVATE (gsttest,
      INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTestPrivateData);
  GValue empty_string = {0};

  gsttest->priv = priv;

  priv->pipeline = NULL;
  priv->elements_used = g_hash_table_new_full (&g_str_hash, &g_str_equal, &g_free, &g_free);
  priv->reached_initial_state = FALSE;

  /* Add our own items, etc */
  g_value_init (&empty_string, G_TYPE_STRING);
  g_value_set_string (&empty_string, "");

  insanity_test_add_argument (test, "pipeline-launch-line", "The launch line to parse to create the pipeline", NULL, &empty_string);
  insanity_test_add_checklist_item (test, "valid-pipeline", "The test pipeline was properly created", NULL);
  insanity_test_add_checklist_item (test, "pipeline-change-state", "The initial state_change happened succesfully", NULL);
  insanity_test_add_checklist_item (test, "reached-initial-state", "The pipeline reached the initial GstElementState", NULL);
  insanity_test_add_checklist_item (test, "no-errors-seen", "No errors were emitted from the pipeline", NULL);


  g_value_unset (&empty_string);
}

void
insanity_cclosure_user_marshal_OBJECT__VOID (GClosure     *closure,
                                      GValue       *return_value G_GNUC_UNUSED,
                                      guint         n_param_values,
                                      const GValue *param_values,
                                      gpointer      invocation_hint G_GNUC_UNUSED,
                                      gpointer      marshal_data)
{
  typedef GObject* (*GMarshalFunc_OBJECT__VOID) (gpointer     data1,
                                                 gpointer     data2);
  register GMarshalFunc_OBJECT__VOID callback;
  register GCClosure *cc = (GCClosure*) closure;
  register gpointer data1, data2;
  GObject* v_return;

  g_return_if_fail (return_value != NULL);
  g_return_if_fail (n_param_values == 1);

  if (G_CCLOSURE_SWAP_DATA (closure))
    {
      data1 = closure->data;
      data2 = g_value_peek_pointer (param_values + 0);
    }
  else
    {
      data1 = g_value_peek_pointer (param_values + 0);
      data2 = closure->data;
    }
  callback = (GMarshalFunc_OBJECT__VOID) (marshal_data ? marshal_data : cc->callback);

  v_return = callback (data1,
                       data2);

  g_value_take_object (return_value, v_return);
}

static gboolean
stop_accumulator (GSignalInvocationHint * ihint,
    GValue * return_accu, const GValue * handler_return, gpointer data)
{
  gboolean v;

  (void) ihint;
  (void) data;
  v = g_value_get_boolean (handler_return);
  g_value_set_boolean (return_accu, v);
  return v;
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

  klass->create_pipeline = &insanity_gst_pipeline_test_create_pipeline;

  g_type_class_add_private (klass, sizeof (InsanityGstPipelineTestPrivateData));

  create_pipeline_signal = g_signal_new ("create_pipeline",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      G_STRUCT_OFFSET (InsanityGstPipelineTestClass, create_pipeline),
  /* Setting the stop accumulator causes g_value asserts... mistake in marshall ? */
      /*&stop_accumulator*/NULL, NULL,
      insanity_cclosure_user_marshal_OBJECT__VOID,
      GST_TYPE_PIPELINE /* return_type */ ,
      0, NULL);
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
