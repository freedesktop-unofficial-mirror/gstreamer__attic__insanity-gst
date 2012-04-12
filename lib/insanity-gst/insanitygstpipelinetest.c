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

static guint bus_message_signal;
static guint reached_initial_state_signal;
static guint duration_signal;

G_DEFINE_TYPE (InsanityGstPipelineTest, insanity_gst_pipeline_test,
    INSANITY_TYPE_GST_TEST);

struct _InsanityGstPipelineTestPrivateData
{
  GstPipeline *pipeline;
  GstBus *bus;

  GstState initial_state;
  gboolean reached_initial_state;
  unsigned int error_count;
  unsigned int tag_count;
  unsigned int element_count;
  gboolean is_live;
  gboolean buffering;
  gboolean enable_buffering;
  gboolean create_pipeline_in_start;

  guint wait_timeout_id;

  InsanityGstCreatePipelineFunction create_pipeline;
  gpointer create_pipeline_user_data;
  GDestroyNotify create_pipeline_destroy_notify;

  GMainLoop *loop;

  GHashTable *elements_used;

  gboolean done;
};

static void
add_element_used (InsanityGstPipelineTest * ptest, GstElement * element)
{
  GstElementFactory *factory;
  const char *factory_name;
  char label[32], *element_name;
  GValue string_value = { 0 };
  GstElement *parent;

  /* Only add once */
  element_name = gst_element_get_name (element);
  if (g_hash_table_lookup_extended (ptest->priv->elements_used, element_name,
          NULL, NULL)) {
    g_free (element_name);
    return;
  }
  g_hash_table_insert (ptest->priv->elements_used, g_strdup (element_name),
      NULL);

  ptest->priv->element_count++;
  g_value_init (&string_value, G_TYPE_STRING);

  factory = gst_element_get_factory (element);
  factory_name =
      factory ? gst_element_factory_get_longname (factory) : "(no factory)";

  g_value_take_string (&string_value, element_name);
  snprintf (label, sizeof (label), "elements-used.%u.name",
      ptest->priv->element_count);
  insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
  g_value_reset (&string_value);

  g_value_set_string (&string_value, factory_name);
  snprintf (label, sizeof (label), "elements-used.%u.factory",
      ptest->priv->element_count);
  insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
  g_value_reset (&string_value);

  parent = GST_ELEMENT (gst_element_get_parent (element));
  if (parent) {
    g_value_take_string (&string_value, gst_element_get_name (parent));
    snprintf (label, sizeof (label), "elements-used.%u.parent",
        ptest->priv->element_count);
    insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
    g_value_reset (&string_value);
    gst_object_unref (parent);
  }
}

static void
send_error (InsanityGstPipelineTest * ptest, const GError * error,
    const char *debug)
{
  char label[32];
  GValue string_value = { 0 };

  ptest->priv->error_count++;
  g_value_init (&string_value, G_TYPE_STRING);

  g_value_set_string (&string_value, g_quark_to_string (error->domain));
  snprintf (label, sizeof (label), "errors.%u.domain",
      ptest->priv->error_count);
  insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
  g_value_reset (&string_value);

  g_value_set_string (&string_value, error->message);
  snprintf (label, sizeof (label), "errors.%u.message",
      ptest->priv->error_count);
  insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
  g_value_reset (&string_value);

  if (debug) {
    g_value_set_string (&string_value, debug);
    snprintf (label, sizeof (label), "errors.%u.debug",
        ptest->priv->error_count);
    insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
    g_value_reset (&string_value);
  }
}

static void on_element_added (GstElement * bin, GstElement * element,
    InsanityGstPipelineTest * ptest);

static guint
watch_container (InsanityGstPipelineTest * ptest, GstBin * bin)
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

  return g_signal_connect (bin, "element-added", (GCallback) on_element_added,
      ptest);
}

static void
on_element_added (GstElement * bin, GstElement * element,
    InsanityGstPipelineTest * ptest)
{
  add_element_used (ptest, element);
  if (GST_IS_BIN (element))
    watch_container (ptest, GST_BIN (element));
}

static void
send_tag (const GstTagList * list, const gchar * tag, gpointer data)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (data);
  gint i, count;
  GValue string_value = { 0 };
  char label[48];

  count = gst_tag_list_get_tag_size (list, tag);
  g_value_init (&string_value, G_TYPE_STRING);

  ptest->priv->tag_count++;

  for (i = 0; i < count; i++) {
    gchar *str;

    if (gst_tag_get_type (tag) == G_TYPE_STRING) {
      if (!gst_tag_list_get_string_index (list, tag, i, &str))
        g_assert_not_reached ();
    } else if (gst_tag_get_type (tag) == GST_TYPE_BUFFER) {
      GstBuffer *img;

      img = gst_value_get_buffer (gst_tag_list_get_value_index (list, tag, i));
      if (img) {
        gchar *caps_str;

        caps_str = GST_BUFFER_CAPS (img) ?
            gst_caps_to_string (GST_BUFFER_CAPS (img)) : g_strdup ("unknown");
        str = g_strdup_printf ("buffer of %u bytes, type: %s",
            GST_BUFFER_SIZE (img), caps_str);
        g_free (caps_str);
      } else {
        str = g_strdup ("NULL buffer");
      }
    } else if (gst_tag_get_type (tag) == GST_TYPE_DATE_TIME) {
      GstDateTime *dt = NULL;

      gst_tag_list_get_date_time_index (list, tag, i, &dt);
      if (gst_date_time_get_hour (dt) < 0) {
        str = g_strdup_printf ("%02u-%02u-%04u", gst_date_time_get_day (dt),
            gst_date_time_get_month (dt), gst_date_time_get_year (dt));
      } else {
        gdouble tz_offset = gst_date_time_get_time_zone_offset (dt);
        gchar tz_str[32];

        if (tz_offset != 0.0) {
          g_snprintf (tz_str, sizeof (tz_str), "(UTC %s%gh)",
              (tz_offset > 0.0) ? "+" : "", tz_offset);
        } else {
          g_snprintf (tz_str, sizeof (tz_str), "(UTC)");
        }

        str = g_strdup_printf ("%04u-%02u-%02u %02u:%02u:%02u %s",
            gst_date_time_get_year (dt), gst_date_time_get_month (dt),
            gst_date_time_get_day (dt), gst_date_time_get_hour (dt),
            gst_date_time_get_minute (dt), gst_date_time_get_second (dt),
            tz_str);
      }
      gst_date_time_unref (dt);
    } else {
      str =
          g_strdup_value_contents (gst_tag_list_get_value_index (list, tag, i));
    }

    if (i == 0) {
      g_value_set_string (&string_value, gst_tag_get_nick (tag));
      snprintf (label, sizeof (label), "tags.%u.id", ptest->priv->tag_count);
      insanity_test_set_extra_info (INSANITY_TEST (ptest), label,
          &string_value);
      g_value_reset (&string_value);
    }
    g_value_set_string (&string_value, str);
    if (count > 1)
      snprintf (label, sizeof (label), "tags.%u.value.%u",
          ptest->priv->tag_count, i);
    else
      snprintf (label, sizeof (label), "tags.%u.value", ptest->priv->tag_count);
    insanity_test_set_extra_info (INSANITY_TEST (ptest), label, &string_value);
    g_value_reset (&string_value);

    g_free (str);
  }
}

static gboolean
waiting_for_state_change (InsanityGstPipelineTest * ptest)
{
  insanity_test_printf (INSANITY_TEST (ptest),
      "State change did not happen, quitting anyway\n");
  g_main_loop_quit (ptest->priv->loop);

  /* one shot */
  return FALSE;
}

gboolean
insanity_gst_pipeline_test_query_duration (InsanityGstPipelineTest * ptest,
    GstFormat fmt, gint64 * duration)
{
  gboolean res;
  GstFormat fmt_tmp = fmt;
  gint64 dur = -1;

  g_return_val_if_fail (INSANITY_IS_GST_PIPELINE_TEST (ptest), FALSE);

  res =
      gst_element_query_duration (GST_ELEMENT (ptest->priv->pipeline), &fmt_tmp,
      &dur);

  if (res && fmt == fmt_tmp && dur != -1) {
    g_signal_emit (ptest, duration_signal, gst_format_to_quark (fmt), fmt, dur,
        NULL);
    if (duration)
      *duration = dur;
    return TRUE;
  } else {
    return FALSE;
  }
}

static gboolean
handle_message (InsanityGstPipelineTest * ptest, GstMessage * message)
{
  gboolean ret = FALSE, done = FALSE;

  /* Allow the test code to handle the message instead */
  g_signal_emit (ptest, bus_message_signal, 0, message, &ret);
  if (!ret)
    return FALSE;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *error = NULL;
      char *debug = NULL;

      gst_message_parse_error (message, &error, &debug);
      send_error (ptest, error, debug);
      insanity_test_printf (INSANITY_TEST (ptest), "Error (%s, %s), quitting\n",
          error->message, debug);
      g_error_free (error);
      g_free (debug);
      done = TRUE;
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (ptest->priv->pipeline)) {
        GstState oldstate, newstate, pending;
        gst_message_parse_state_changed (message, &oldstate, &newstate,
            &pending);

        if (newstate >= GST_STATE_PAUSED) {
          insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_TIME,
              NULL);
          insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_BYTES,
              NULL);
          insanity_gst_pipeline_test_query_duration (ptest, GST_FORMAT_DEFAULT,
              NULL);
        }

        if (newstate == ptest->priv->initial_state
            && pending == GST_STATE_VOID_PENDING
            && !ptest->priv->reached_initial_state) {
          gboolean ret = TRUE;

          ptest->priv->reached_initial_state = TRUE;
          insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
              "reached-initial-state", TRUE, NULL);

          /* Tell the test we reached our initial state */
          g_signal_emit (ptest, reached_initial_state_signal, 0, &ret);
          if (!ret) {
            insanity_test_printf (INSANITY_TEST (ptest),
                "Reached initial state, and asked to quit, quitting\n");
            done = TRUE;
          }
        }
      }
      break;
    case GST_MESSAGE_TAG:{
      GstTagList *tags;
      gst_message_parse_tag (message, &tags);
      gst_tag_list_foreach (tags, &send_tag, (gpointer) ptest);
      gst_tag_list_free (tags);
      break;
    }
    case GST_MESSAGE_DURATION:{
      GstFormat fmt;

      gst_message_parse_duration (message, &fmt, NULL);
      insanity_gst_pipeline_test_query_duration (ptest, fmt, NULL);
      break;
    }
    case GST_MESSAGE_EOS:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (ptest->priv->pipeline)) {
        /* Warning from the original Python source:
           # it's not 100% sure we want to stop here, because of the
           # race between the final state-change message and the eos message
           # arriving on the bus.
         */
        if (ptest->priv->reached_initial_state) {
          insanity_test_printf (INSANITY_TEST (ptest),
              "Got EOS from pipeline, and we reached initial state, quitting\n");
          done = TRUE;
        } else {
          /* If we've not seen the state change to initial state yet, we give
             an extra 3 seconds for it to complete */
          insanity_test_printf (INSANITY_TEST (ptest),
              "Got EOS from pipeline, and we did not reach initial state, delaying quit\n");
          ptest->priv->wait_timeout_id =
              g_timeout_add (3000, (GSourceFunc) & waiting_for_state_change,
              ptest);
        }
      }
      break;
    case GST_MESSAGE_BUFFERING:{
      gint percent;

      gst_message_parse_buffering (message, &percent);

      /* no state management needed for live pipelines */
      if (ptest->priv->is_live || !ptest->priv->enable_buffering)
        break;

      if (percent == 100) {
        /* a 100% message means buffering is done */
        ptest->priv->buffering = FALSE;
        /* if the desired state is playing, go back */
        if (ptest->priv->initial_state == GST_STATE_PLAYING) {
          gst_element_set_state (GST_ELEMENT (ptest->priv->pipeline),
              GST_STATE_PLAYING);
        }
      } else {
        /* buffering busy */
        if (ptest->priv->buffering == FALSE
            && ptest->priv->initial_state == GST_STATE_PLAYING) {
          /* we were not buffering but PLAYING, PAUSE  the pipeline. */
          gst_element_set_state (GST_ELEMENT (ptest->priv->pipeline),
              GST_STATE_PAUSED);
        }
        ptest->priv->buffering = TRUE;
      }
      break;
    case GST_MESSAGE_CLOCK_LOST:
      gst_element_set_state (GST_ELEMENT (ptest->priv->pipeline),
          GST_STATE_PAUSED);
      gst_element_set_state (GST_ELEMENT (ptest->priv->pipeline),
          GST_STATE_PLAYING);
      break;
    }
    default:
      break;
  }

  return done;
}

static gboolean
on_message (GstBus * bus, GstMessage * msg, gpointer userdata)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (userdata);
  if (handle_message (ptest, msg))
    g_main_loop_quit (ptest->priv->loop);
  return TRUE;
}

static gboolean
create_pipeline (InsanityGstPipelineTest * ptest)
{
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  priv->pipeline =
      INSANITY_GST_PIPELINE_TEST_GET_CLASS (ptest)->create_pipeline (ptest);
  insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
      "valid-pipeline", priv->pipeline != NULL, NULL);

  if (!priv->pipeline)
    return FALSE;

  watch_container (ptest, GST_BIN (priv->pipeline));

  priv->bus = gst_element_get_bus (GST_ELEMENT (priv->pipeline));

  return TRUE;
}

static gboolean
insanity_gst_pipeline_test_setup (InsanityTest * test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->setup
      (test))
    return FALSE;

  priv->elements_used =
      g_hash_table_new_full (&g_str_hash, &g_str_equal, &g_free, &g_free);

  if (!priv->create_pipeline_in_start)
    return create_pipeline (ptest);

  return TRUE;
}

static gboolean
insanity_gst_pipeline_test_start (InsanityTest * test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  g_assert (priv->loop == NULL);
  priv->loop = g_main_loop_new (NULL, FALSE);

  if (priv->create_pipeline_in_start) {
    if (!create_pipeline (ptest))
      return FALSE;
  }

  if (!INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->start
      (test))
    return FALSE;

  priv->reached_initial_state = FALSE;
  priv->error_count = 0;
  priv->tag_count = 0;
  priv->element_count = 0;
  priv->wait_timeout_id = 0;
  priv->is_live = FALSE;
  priv->buffering = FALSE;
  priv->enable_buffering = TRUE;
  priv->done = FALSE;

  add_element_used (ptest, GST_ELEMENT (ptest->priv->pipeline));

  return TRUE;
}

static void
insanity_gst_pipeline_test_stop (InsanityTest * test)
{
  InsanityGstPipelineTestPrivateData *priv =
      INSANITY_GST_PIPELINE_TEST (test)->priv;
  GstState state, pending;

  if (priv->wait_timeout_id) {
    g_source_remove (priv->wait_timeout_id);
    priv->wait_timeout_id = 0;
  }

  if (priv->pipeline) {
    gst_element_set_state (GST_ELEMENT (priv->pipeline), GST_STATE_NULL);
    gst_element_get_state (GST_ELEMENT (priv->pipeline), &state,
        &pending, GST_CLOCK_TIME_NONE);
  }

  if (priv->create_pipeline_in_start) {
    if (priv->bus) {
      gst_object_unref (priv->bus);
      priv->bus = NULL;
    }
    if (priv->pipeline) {
      gst_object_unref (priv->pipeline);
      priv->pipeline = NULL;
    }

    if (priv->elements_used)
      g_hash_table_destroy (priv->elements_used);
    priv->elements_used = NULL;
  }

  if (priv->loop) {
    g_main_loop_quit (priv->loop);
    while (g_main_loop_is_running (priv->loop))
      g_usleep (20000);
    g_main_loop_unref (priv->loop);
    priv->loop = NULL;
  }

  INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->stop (test);
}

static void
insanity_gst_pipeline_test_teardown (InsanityTest * test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  InsanityGstPipelineTestPrivateData *priv = ptest->priv;

  insanity_test_validate_checklist_item (test, "no-errors-seen",
      priv->error_count == 0, NULL);

  if (priv->bus) {
    gst_object_unref (priv->bus);
    priv->bus = NULL;
  }
  if (priv->pipeline) {
    gst_object_unref (priv->pipeline);
    priv->pipeline = NULL;
  }

  if (priv->elements_used)
    g_hash_table_destroy (ptest->priv->elements_used);
  ptest->priv->elements_used = NULL;

  INSANITY_TEST_CLASS (insanity_gst_pipeline_test_parent_class)->teardown
      (test);
}

static void
insanity_gst_pipeline_test_test (InsanityThreadedTest * test)
{
  InsanityGstPipelineTest *ptest = INSANITY_GST_PIPELINE_TEST (test);
  guint id;
  GstStateChangeReturn sret;

  sret =
      gst_element_set_state (GST_ELEMENT (ptest->priv->pipeline),
      ptest->priv->initial_state);
  insanity_test_validate_checklist_item (INSANITY_TEST (test),
      "pipeline-change-state", (sret != GST_STATE_CHANGE_FAILURE), NULL);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    insanity_test_done (INSANITY_TEST (ptest));
    return;
  }
  if (sret == GST_STATE_CHANGE_NO_PREROLL) {
    ptest->priv->is_live = TRUE;
  }

  gst_bus_add_signal_watch (ptest->priv->bus);
  id = g_signal_connect (G_OBJECT (ptest->priv->bus), "message",
      (GCallback) & on_message, ptest);
  g_main_loop_run (ptest->priv->loop);

  if (ptest->priv->bus)
    g_signal_handler_disconnect (G_OBJECT (ptest->priv->bus), id);

  insanity_test_done (INSANITY_TEST (ptest));
}

static GstPipeline *
insanity_gst_pipeline_test_create_pipeline (InsanityGstPipelineTest * ptest)
{
  GstPipeline *pipeline = NULL;

  if (ptest->priv->create_pipeline) {
    pipeline =
        (ptest->priv->create_pipeline) (ptest,
        ptest->priv->create_pipeline_user_data);
  }

  return pipeline;
}

static gboolean
insanity_gst_pipeline_test_bus_message (InsanityGstPipelineTest * ptest,
    GstMessage * msg)
{
  /* By default, we do not ignore the message */
  return TRUE;
}

static gboolean
insanity_gst_pipeline_test_reached_initial_state (InsanityGstPipelineTest *
    ptest)
{
  /* By default, we continue */
  return TRUE;
}

static void
insanity_gst_pipeline_test_init (InsanityGstPipelineTest * gsttest)
{
  InsanityTest *test = INSANITY_TEST (gsttest);
  InsanityGstPipelineTestPrivateData *priv =
      G_TYPE_INSTANCE_GET_PRIVATE (gsttest,
      INSANITY_TYPE_GST_PIPELINE_TEST, InsanityGstPipelineTestPrivateData);

  gsttest->priv = priv;

  priv->pipeline = NULL;
  priv->bus = NULL;
  priv->reached_initial_state = FALSE;
  priv->error_count = 0;
  priv->tag_count = 0;
  priv->element_count = 0;
  priv->initial_state = GST_STATE_PLAYING;
  priv->wait_timeout_id = 0;
  priv->create_pipeline = NULL;
  priv->create_pipeline_user_data = NULL;
  priv->create_pipeline_destroy_notify = NULL;
  priv->done = FALSE;

  /* Add our own items, etc */
  insanity_test_add_checklist_item (test, "valid-pipeline",
      "The test pipeline was properly created", NULL);
  insanity_test_add_checklist_item (test, "pipeline-change-state",
      "The initial state_change happened succesfully", NULL);
  insanity_test_add_checklist_item (test, "reached-initial-state",
      "The pipeline reached the initial GstElementState", NULL);
  insanity_test_add_checklist_item (test, "no-errors-seen",
      "No errors were emitted from the pipeline", NULL);

  insanity_test_add_extra_info (test, "errors",
      "List of errors emitted by the pipeline");
  insanity_test_add_extra_info (test, "tags",
      "List of tags emitted by the pipeline");
  insanity_test_add_extra_info (test, "elements-used", "List of elements used");
}

static void
insanity_gst_pipeline_test_finalize (GObject * gobject)
{
  InsanityGstPipelineTest *gtest = (InsanityGstPipelineTest *) gobject;

  insanity_gst_pipeline_test_set_create_pipeline_function (gtest, NULL, NULL,
      NULL);

  G_OBJECT_CLASS (insanity_gst_pipeline_test_parent_class)->finalize (gobject);
}

#define g_marshal_value_peek_object(v)   g_value_get_object (v)
static void
insanity_cclosure_user_marshal_BOOLEAN__MINIOBJECT (GClosure * closure,
    GValue * return_value G_GNUC_UNUSED,
    guint n_param_values,
    const GValue * param_values,
    gpointer invocation_hint G_GNUC_UNUSED, gpointer marshal_data)
{
  typedef gboolean (*GMarshalFunc_BOOLEAN__MINIOBJECT) (gpointer data1,
      gpointer arg_1, gpointer data2);
  register GMarshalFunc_BOOLEAN__MINIOBJECT callback;
  register GCClosure *cc = (GCClosure *) closure;
  register gpointer data1, data2;
  gboolean v_return;

  g_return_if_fail (return_value != NULL);
  g_return_if_fail (n_param_values == 2);

  if (G_CCLOSURE_SWAP_DATA (closure)) {
    data1 = closure->data;
    data2 = g_value_peek_pointer (param_values + 0);
  } else {
    data1 = g_value_peek_pointer (param_values + 0);
    data2 = closure->data;
  }
  callback =
      (GMarshalFunc_BOOLEAN__MINIOBJECT) (marshal_data ? marshal_data :
      cc->callback);

  v_return = callback (data1,
      gst_value_get_mini_object (param_values + 1), data2);

  g_value_set_boolean (return_value, v_return);
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
  InsanityThreadedTestClass *threaded_test_class =
      INSANITY_THREADED_TEST_CLASS (klass);

  gobject_class->finalize = &insanity_gst_pipeline_test_finalize;

  test_class->setup = &insanity_gst_pipeline_test_setup;
  test_class->start = &insanity_gst_pipeline_test_start;
  test_class->stop = &insanity_gst_pipeline_test_stop;
  test_class->teardown = &insanity_gst_pipeline_test_teardown;

  threaded_test_class->test = &insanity_gst_pipeline_test_test;

  klass->create_pipeline = &insanity_gst_pipeline_test_create_pipeline;
  klass->bus_message = &insanity_gst_pipeline_test_bus_message;
  klass->reached_initial_state =
      &insanity_gst_pipeline_test_reached_initial_state;

  g_type_class_add_private (klass, sizeof (InsanityGstPipelineTestPrivateData));

  bus_message_signal = g_signal_new ("bus-message",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      G_STRUCT_OFFSET (InsanityGstPipelineTestClass, bus_message),
      &stop_accumulator, NULL,
      insanity_cclosure_user_marshal_BOOLEAN__MINIOBJECT,
      G_TYPE_BOOLEAN /* return_type */ ,
      1, GST_TYPE_MESSAGE, NULL);
  reached_initial_state_signal = g_signal_new ("reached-initial-state",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      G_STRUCT_OFFSET (InsanityGstPipelineTestClass, reached_initial_state),
      &stop_accumulator, NULL, NULL, G_TYPE_BOOLEAN /* return_type */ ,
      0, NULL);
  duration_signal = g_signal_new ("duration",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS |
      G_SIGNAL_DETAILED, 0, NULL, NULL, NULL, G_TYPE_NONE /* return_type */ ,
      2, GST_TYPE_FORMAT, G_TYPE_UINT64, NULL);
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
insanity_gst_pipeline_test_new (const char *name, const char *description,
    const char *full_description)
{
  InsanityGstPipelineTest *test;

  if (full_description) {
    test = g_object_new (insanity_gst_pipeline_test_get_type (),
        "name", name, "description", description, "full-description",
        full_description, NULL);
  } else {
    test = g_object_new (insanity_gst_pipeline_test_get_type (),
        "name", name, "description", description, NULL);
  }
  return test;
}

/**
 * insanity_gst_pipeline_test_set_initial_state:
 * @test: the #InsanityGstPipelineTest to change
 * @state: the #GstState to which to bring the pipeline at test start
 *
 * Selects the state to which the pipeline should be brought to at test
 * start. By default, it is GST_STATE_PLAYING.
 */
void
insanity_gst_pipeline_test_set_initial_state (InsanityGstPipelineTest * test,
    GstState state)
{
  g_return_if_fail (INSANITY_IS_GST_PIPELINE_TEST (test));

  test->priv->initial_state = state;
}

/**
 * insanity_gst_pipeline_test_set_create_pipeline_function:
 * @test: the #InsanityGstPipelineTest to change
 * @func: the function to call to create a new pipeline
 * @userdata: whatever data the user wants passed to the function
 * @dnotify: (allow-none): a function to be called on the previous data when replaced
 *
 * Set a function to call for creating a pipeline.
 * The function should return NULL if a pipeline fails to be created.
 */
void
insanity_gst_pipeline_test_set_create_pipeline_function (InsanityGstPipelineTest
    * test, InsanityGstCreatePipelineFunction func, gpointer userdata,
    GDestroyNotify dnotify)
{
  g_return_if_fail (INSANITY_IS_GST_PIPELINE_TEST (test));

  if (test->priv->create_pipeline) {
    if (test->priv->create_pipeline_destroy_notify)
      (*test->priv->create_pipeline_destroy_notify) (test->
          priv->create_pipeline_user_data);
  }
  test->priv->create_pipeline = func;
  test->priv->create_pipeline_user_data = userdata;
  test->priv->create_pipeline_destroy_notify = dnotify;
}

/**
 * insanity_gst_pipeline_test_set_live:
 * @test: the #InsanityGstPipelineTest to change
 * @live: %TRUE if the pipeline is live, %FALSE otherwise
 *
 * Tells the #InsanityGstPipelineTest whether the pipeline is live or not.
 * This is used to change behavior to match live or non live pipelines
 * (for instance, live pipelines never temporarily pause when receiving
 * buffering messages).
 */
void
insanity_gst_pipeline_test_set_live (InsanityGstPipelineTest * test,
    gboolean live)
{
  g_return_if_fail (INSANITY_IS_GST_PIPELINE_TEST (test));

  test->priv->is_live = live;
}

/**
 * insanity_gst_pipeline_test_enable_buffering:
 * @test: the #InsanityGstPipelineTest to change
 * @buffering: %TRUE if buffering should be enabled, %FALSE otherwise
 *
 * Enables buffering behavior on the #InsanityGstPipelineTest.
 * This temporarily pauses a playing pipeline when running out of
 * data, and starts playing again after enough data is available.
 *
 * Live pipelines will not do buffering, even when enabled.
 *
 * This behavior is likely to not work well if the test changes states
 * itself, so disabling buffering when doing so is suggested.
 */
void
insanity_gst_pipeline_test_enable_buffering (InsanityGstPipelineTest * test,
    gboolean buffering)
{
  g_return_if_fail (INSANITY_IS_GST_PIPELINE_TEST (test));

  test->priv->enable_buffering = buffering;
}

/**
 * insanity_gst_pipeline_test_set_create_pipeline_in_start:
 * @test: the #InsanityGstPipelineTest to change
 * @create_pipeline_in_start: Whether the pipeline should be created when starting
 * the test rather than when the test is created (setup).
 *
 * Sets whether the pipeline should be created on each iteration of start/stop
 * rather than once per process.
 */
void
insanity_gst_pipeline_test_set_create_pipeline_in_start (InsanityGstPipelineTest
    * test, gboolean create_pipeline_in_start)
{
  g_return_if_fail (INSANITY_IS_GST_PIPELINE_TEST (test));

  test->priv->create_pipeline_in_start = create_pipeline_in_start;
}
