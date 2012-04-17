/* Insanity QA system

       insanitygsttest.h

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
 * SECTION:insanitygsttest
 * @short_description: GStreamer Test
 * @see_also: #InsanityTest
 *
 * %TODO.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>

#include <insanity-gst/insanitygsttest.h>

G_DEFINE_TYPE (InsanityGstTest, insanity_gst_test, INSANITY_TYPE_THREADED_TEST);

struct _InsanityGstTestPrivateData
{
  int dummy;
};

static void
init_gstreamer (void)
{
  int argc = 1;
  char **argv;

  argv = g_malloc (2 * sizeof (char *));
  argv[0] = g_get_prgname ();
  argv[1] = NULL;
  gst_init (&argc, &argv);
  g_free (argv);
}

static gboolean
insanity_gst_test_setup (InsanityTest * test)
{
  const char *debuglog, *registry;

  if (!INSANITY_TEST_CLASS (insanity_gst_test_parent_class)->setup (test))
    return FALSE;

  /* Set GST_DEBUG_FILE to the target filename */
  debuglog = insanity_test_get_output_filename (test, "gst-debug-log");
  g_setenv ("GST_DEBUG_FILE", debuglog, TRUE);

  /* Set GST_REGISTRY to the target filename */
  registry = insanity_test_get_output_filename (test, "gst-registry");
  g_setenv ("GST_REGISTRY", registry, TRUE);

  /* We don't want the tests to update the registry because:
   * it will make the tests start up faster
   * the tests across testrun should be using the same registry/plugins
   *
   * This feature is only available since 0.10.19.1 (24th April 2008) in
   * GStreamer core */
  g_setenv ("GST_REGISTRY_UPDATE", "no", TRUE);

  init_gstreamer ();

  return TRUE;
}

static gboolean
insanity_gst_test_start (InsanityTest * test)
{
  if (!INSANITY_TEST_CLASS (insanity_gst_test_parent_class)->start (test))
    return FALSE;

  return TRUE;
}

static void
insanity_gst_test_stop (InsanityTest * test)
{
  INSANITY_TEST_CLASS (insanity_gst_test_parent_class)->stop (test);
}

static void
insanity_gst_test_teardown (InsanityTest * test)
{
  gst_deinit ();

  INSANITY_TEST_CLASS (insanity_gst_test_parent_class)->teardown (test);
}

static void
insanity_gst_test_init (InsanityGstTest * gsttest)
{
  InsanityTest *test = INSANITY_TEST (gsttest);
  InsanityGstTestPrivateData *priv = G_TYPE_INSTANCE_GET_PRIVATE (gsttest,
      INSANITY_TYPE_GST_TEST, InsanityGstTestPrivateData);

  gsttest->priv = priv;

  /* Add our own items, etc */
  insanity_test_add_output_file (test, "gst-debug-log",
      "The GStreamer debug log", TRUE);
  insanity_test_add_output_file (test, "gst-registry",
      "The GStreamer registry file", TRUE);
}

static void
insanity_gst_test_class_init (InsanityGstTestClass * klass)
{
  InsanityTestClass *test_class = INSANITY_TEST_CLASS (klass);

  test_class->setup = &insanity_gst_test_setup;
  test_class->start = &insanity_gst_test_start;
  test_class->stop = &insanity_gst_test_stop;
  test_class->teardown = &insanity_gst_test_teardown;

  g_type_class_add_private (klass, sizeof (InsanityGstTestPrivateData));
}

/**
 * insanity_gst_test_new:
 * @name: the short name of the test.
 * @description: a one line description of the test.
 * @full_description: (allow-none): an optional longer description of the test.
 *
 * This function creates a new GStreamer test with the given properties.
 *
 * Returns: (transfer full): a new #InsanityGstTest instance.
 */
InsanityGstTest *
insanity_gst_test_new (const char *name, const char *description,
    const char *full_description)
{
  InsanityGstTest *test;

  if (full_description) {
    test = g_object_new (insanity_gst_test_get_type (),
        "name", name, "description", description, "full-description",
        full_description, NULL);
  } else {
    test = g_object_new (insanity_gst_test_get_type (),
        "name", name, "description", description, NULL);
  }
  return test;
}

typedef struct
{
  InsanityGstTest *test;
  InsanityGstDataProbeFunction func;
  gpointer user_data;
  GDestroyNotify dnotify;
} DataProbeCtx;

static void
free_data_probe_ctx (DataProbeCtx * ctx)
{
  g_object_unref (ctx->test);
  if (ctx->dnotify)
    ctx->dnotify (ctx->user_data);
  g_slice_free (DataProbeCtx, ctx);
}

static GstPadProbeReturn
data_probe_cb (GstPad * pad, GstPadProbeInfo * info, DataProbeCtx * ctx)
{
  gboolean ret;

  ret = ctx->func (ctx->test, pad, info->data, ctx->user_data);

  return ret ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
}

/**
 * insanity_gst_test_add_data_probe:
 * @test: the #InsanityGstTest
 * @bin: (transfer none): a bin where to look for the sinks
 * @element_name: the name of the element on which to find the pad to add a probe to
 * @pad_name: the name of the pad to add a probe to
 * @pad: (out) (transfer full): a pointer where to place a pointer to the pad to which the probe was attached to
 * @probe_id: (out): a pointer where to place the identifier of the probe
 * @probe: the data probe function to call
 * @user_data: user data for @probe
 * @dnotify: #GDestroyNotify for @user_data
 *
 * This function adds a data probe to a named element and pad in the given bin.
 * The pad and probe should be passed to insanity_gst_test_remove_data_probe when done.
 *
 * Returns: %TRUE if the probe was placed, %FALSE otherwise
 */
gboolean
insanity_gst_test_add_data_probe (InsanityGstTest * test, GstBin * bin,
    const char *element_name, const char *pad_name,
    GstPad ** pad, gulong * probe_id, InsanityGstDataProbeFunction probe,
    gpointer user_data, GDestroyNotify dnotify)
{
  GstElement *e;
  DataProbeCtx *ctx;

  g_return_val_if_fail (INSANITY_IS_GST_TEST (test), FALSE);
  g_return_val_if_fail (GST_IS_BIN (bin), FALSE);
  g_return_val_if_fail (element_name != NULL, FALSE);
  g_return_val_if_fail (pad_name != NULL, FALSE);
  g_return_val_if_fail (pad != NULL, FALSE);
  g_return_val_if_fail (probe_id != NULL, FALSE);
  g_return_val_if_fail (probe != NULL, FALSE);

  *pad = NULL;
  *probe_id = 0;

  e = gst_bin_get_by_name (bin, element_name);
  if (!e) {
    if (dnotify)
      dnotify (user_data);
    return FALSE;
  }

  *pad = gst_element_get_static_pad (e, pad_name);
  gst_object_unref (e);
  if (!*pad) {
    if (dnotify)
      dnotify (user_data);
    return FALSE;
  }

  ctx = g_slice_new (DataProbeCtx);
  ctx->test = g_object_ref (test);
  ctx->func = probe;
  ctx->user_data = user_data;
  ctx->dnotify = dnotify;

  *probe_id = gst_pad_add_probe (*pad, GST_PAD_PROBE_TYPE_ALL_BOTH,
      (GstPadProbeCallback) data_probe_cb, ctx,
      (GDestroyNotify) free_data_probe_ctx);

  if (*probe_id != 0) {
    insanity_test_printf (INSANITY_TEST (test), "Probe %u connected to %s:%s\n",
        *probe_id, element_name, pad_name);
    return TRUE;
  } else {
    g_object_unref (ctx->test);
    g_slice_free (DataProbeCtx, ctx);
    if (dnotify)
      dnotify (user_data);
    gst_object_unref (*pad);
    *pad = NULL;
    return FALSE;
  }
}

/**
 * insanity_gst_test_remove_data_probe:
 * @test: the #InsanityGstTest
 * @pad: the pad as returned by insanity_gst_test_add_data_probe
 * @probe: the probe identifier as returned by insanity_gst_test_add_data_probe
 *
 * This function removes a data probe added by insanity_gst_test_add_data_probe.
 */
void
insanity_gst_test_remove_data_probe (InsanityGstTest * test,
    GstPad * pad, gulong probe)
{
  g_return_if_fail (INSANITY_IS_GST_TEST (test));
  g_return_if_fail (GST_IS_PAD (pad));
  g_return_if_fail (probe != 0);

  gst_pad_remove_probe (pad, probe);
}
