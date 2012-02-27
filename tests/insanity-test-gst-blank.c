/* Insanity QA system

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

#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <insanity-gst/insanity-gst.h>

static void
blank_gst_test_test (InsanityTest * test)
{
  printf ("blank_gst_test_test\n");
}

static GstPipeline*
blank_gst_test_create_pipeline (InsanityGstPipelineTest *ptest, gpointer userdata)
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

int
main (int argc, char **argv)
{
  InsanityTest *test;
  gboolean ret;
  GValue empty_string = {0};


  g_type_init ();

  test =
      INSANITY_TEST (insanity_gst_pipeline_test_new ("blank-c-gst-test",
          "Sample GStreamer test that does nothing", NULL));

  g_value_init (&empty_string, G_TYPE_STRING);
  g_value_set_string (&empty_string, "");
  insanity_test_add_argument (test, "pipeline-launch-line", "The launch line to parse to create the pipeline", NULL, TRUE, &empty_string);
  g_value_unset (&empty_string);

  insanity_gst_pipeline_test_set_create_pipeline_function (INSANITY_GST_PIPELINE_TEST (test),
      &blank_gst_test_create_pipeline, NULL, NULL);
  //g_signal_connect_after (test, "test", G_CALLBACK (&blank_gst_test_test), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
