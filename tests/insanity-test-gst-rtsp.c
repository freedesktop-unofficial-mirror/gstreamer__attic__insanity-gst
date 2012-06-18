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
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <insanity-gst/insanity-gst.h>

typedef enum
{
  NEXT_STEP_NOW,
  NEXT_STEP_ON_PLAYING,
  NEXT_STEP_RESTART_ON_PLAYING,
  NEXT_STEP_ON_PAUSED,
  NEXT_STEP_RESTART_ON_TICK,
} NextStepTrigger;

static GstElement *global_pipeline = NULL;
static GstRTSPServer *global_server = NULL;
static guint global_state_change_timeout = 0;
static unsigned int global_state = 0;
static unsigned int global_next_state = 0;
static GstState global_waiting_on_state = GST_STATE_VOID_PENDING;
static GstClockTime global_wait_time = GST_CLOCK_TIME_NONE;
static GstClockTime global_playback_time = GST_CLOCK_TIME_NONE;
static guint global_timer_id = 0;
static gboolean global_live = FALSE;

static GstClockTime
rtsp_test_get_position (InsanityTest * test)
{
  gint64 pos = 0;
  gboolean res;
  GstFormat format = GST_FORMAT_TIME;

  res = gst_element_query_position (global_pipeline, &format, &pos);
  if (format != GST_FORMAT_TIME)
    res = FALSE;
  insanity_test_validate_checklist_item (test, "position-queried", res, NULL);
  if (!res) {
    pos = GST_CLOCK_TIME_NONE;
  }
  return pos;
}

static GstClockTime
rtsp_test_get_wait_time (InsanityTest * test)
{
  gint64 pos = rtsp_test_get_position (test);
  if (GST_CLOCK_TIME_IS_VALID (pos)) {
    pos += global_playback_time;
  }
  return pos;
}

static void on_ready_for_next_state (InsanityGstPipelineTest * ptest,
    gboolean timeout);

static GstPipeline *
rtsp_test_create_pipeline (InsanityGstPipelineTest * ptest, gpointer userdata)
{
  GstElement *pipeline = NULL;
  const char *launch_line = "playbin2 uri=rtsp://127.0.0.1:8554/test"
#if 0
      " audio-sink=fakesink video-sink=fakesink"
#endif
      ;
  GError *error = NULL;

  pipeline = gst_parse_launch (launch_line, &error);
  if (!pipeline) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, error ? error->message : NULL);
    if (error)
      g_error_free (error);
    return NULL;
  } else if (error) {
    /* Do we get a dangling pointer here ? gst-launch.c does not unref */
    pipeline = NULL;
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        "valid-pipeline", FALSE, error->message);
    g_error_free (error);
    return NULL;
  }

  global_pipeline = pipeline;

  return GST_PIPELINE (pipeline);
}

static const char *
rtsp_test_create_rtsp_server (void)
{
  global_server = gst_rtsp_server_new ();

  g_object_set (global_server, "address", "127.0.0.1", NULL);

  /* attach the server to the default maincontext */
  gst_rtsp_server_attach (global_server, NULL);

  return NULL;
}

static void
rtsp_test_destroy_server (void)
{
  /* examples do not free this
     g_object_unref (global_server);
     global_server = NULL;
   */
}

static gboolean
rtsp_test_setup (InsanityTest * test)
{
  const char *error;
  gint secs;

  error = rtsp_test_create_rtsp_server ();
  insanity_test_validate_checklist_item (test, "server-created", error == NULL,
      error);
  if (error)
    return FALSE;

  insanity_test_get_int_argument (test, "playback-time", &secs);
  global_playback_time = secs * GST_SECOND;

  return TRUE;
}

static void
rtsp_test_teardown (InsanityTest * test)
{
  rtsp_test_destroy_server ();
}

static void
rtsp_test_reset_server (void)
{
  GstRTSPMediaMapping *mapping;

  mapping = gst_rtsp_server_get_media_mapping (global_server);
  gst_rtsp_media_mapping_remove_factory (mapping, "/test");
  g_object_unref (mapping);
}

static gboolean
rtsp_test_configure_server_for_uri (const char *uri)
{
  GstRTSPMediaMapping *mapping;
  GstRTSPMediaFactoryURI *factory;

  /* get the mapping for this server, every server has a default mapper object
   * that be used to map uri mount points to media factories */
  mapping = gst_rtsp_server_get_media_mapping (global_server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines. 
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_uri_new ();
  gst_rtsp_media_factory_uri_set_uri (factory, uri);
  /*gst_rtsp_media_factory_set_shared (factory, TRUE); */

  /* attach the test factory to the /test url; any previous factory will be unreffed */
  gst_rtsp_media_mapping_add_factory (mapping, "/test",
      GST_RTSP_MEDIA_FACTORY (factory));
  g_object_unref (mapping);

  return TRUE;
}

static char *
rtsp_test_make_source_path (const char *source, const char *transform,
    const char *pay, unsigned int *payidx, unsigned int *ptidx)
{
  if (pay)
    return g_strdup_printf ("%s is-live=1 ! %s ! %s name=pay%u pt=%u",
        source, transform ? transform : "identity", pay, *payidx++, *ptidx++);
  else
    return g_strdup_printf ("%s is-live=1 ! %s name=%s-path",
        source, transform ? transform : "identity", source);
}

static char *
rtsp_test_make_muxer_path (const char *src0, const char *src1,
    const char *muxer, const char *pay, unsigned int *payidx,
    unsigned int *ptidx)
{
  char *base, *path0 = NULL, *path1 = NULL;

  if (src0) {
    path0 = g_strdup_printf ("%s-path. ! queue ! mux.", src0);
  }
  if (src1) {
    path1 = g_strdup_printf ("%s-path. ! queue ! mux.", src1);
  }
  base = g_strdup_printf ("%s name=mux ! %s name=pay%u pt=%u %s %s",
      muxer, pay, *payidx++, *ptidx++, path0 ? path0 : "", path1 ? path1 : "");
  g_free (path1);
  g_free (path0);
  return base;
}

static gboolean
rtsp_test_configure_server_for_multiple_streams (InsanityTest * test,
    const char *video_encoder, const char *video_payloader,
    const char *audio_encoder, const char *audio_payloader,
    const char *muxer, const char *muxer_payloader)
{
  GstRTSPMediaMapping *mapping;
  GstRTSPMediaFactory *factory;
  char *video_path = NULL, *audio_path = NULL, *muxer_path = NULL, *sbin = NULL;
  unsigned payloader_index = 0, pt_index = 96;
  GValue vsbin = { 0 };

  if (video_encoder) {
    video_path = rtsp_test_make_source_path ("videotestsrc",
        video_encoder, video_payloader, &payloader_index, &pt_index);
  }
  if (audio_encoder) {
    audio_path = rtsp_test_make_source_path ("audiotestsrc",
        audio_encoder, audio_payloader, &payloader_index, &pt_index);
  }
  if (muxer) {
    muxer_path = rtsp_test_make_muxer_path (video_path ? "videotestsrc" : NULL,
        audio_path ? "audiotestsrc" : NULL, muxer, muxer_payloader,
        &payloader_index, &pt_index);
  }

  /* get the mapping for this server, every server has a default mapper object
   * that be used to map uri mount points to media factories */
  mapping = gst_rtsp_server_get_media_mapping (global_server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines. 
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_new ();
  sbin = g_strdup_printf ("( %s %s %s )",
      video_path ? video_path : "",
      audio_path ? audio_path : "", muxer_path ? muxer_path : "");
  gst_rtsp_media_factory_set_launch (factory, sbin);
  /*gst_rtsp_media_factory_set_shared (factory, TRUE); */

  /* attach the test factory to the /test url; any previous factory will be unreffed */
  gst_rtsp_media_mapping_add_factory (mapping, "/test",
      GST_RTSP_MEDIA_FACTORY (factory));
  g_object_unref (mapping);

  g_value_init (&vsbin, G_TYPE_STRING);
  g_value_set_string (&vsbin, sbin);
  insanity_test_set_extra_info (test, "launch-line", &vsbin);
  g_value_unset (&vsbin);

  g_free (sbin);
  g_free (video_path);
  g_free (audio_path);

  return TRUE;
}

static const char *
rtsp_test_get_string (const GValue * v)
{
  const char *s = g_value_get_string (v);
  if (!s || !*s)
    return NULL;
  return s;
}

static const char *
rtsp_test_check_arguments (const GValue * vuri,
    const GValue * vvideo_encoder, const GValue * vvideo_payloader,
    const GValue * vaudio_encoder, const GValue * vaudio_payloader,
    const GValue * vmuxer, const GValue * vmuxer_payloader)
{
  const char *uri = rtsp_test_get_string (vuri);
  const char *video_encoder = rtsp_test_get_string (vvideo_encoder);
  const char *video_payloader = rtsp_test_get_string (vvideo_payloader);
  const char *audio_encoder = rtsp_test_get_string (vaudio_encoder);
  const char *audio_payloader = rtsp_test_get_string (vaudio_payloader);
  const char *muxer = rtsp_test_get_string (vmuxer);
  const char *muxer_payloader = rtsp_test_get_string (vmuxer_payloader);

  /* URI must be set if all others are not, and not if any other is */
  if (uri) {
    if (video_encoder || video_payloader)
      return "URI and video encoder/payloader are conflicting";
    if (audio_encoder || audio_payloader)
      return "URI and video encoder/payloader are conflicting";
    if (muxer || muxer_payloader)
      return "URI and muxer/payloader are conflicting";
  } else {
    if (muxer_payloader) {
      if (video_payloader || audio_payloader)
        return
            "Must specify no A/V payloaders when a muxer payloader is specified";
    } else {
      /* payloader for audio or video must be specified */
      if (!video_payloader && !audio_payloader)
        return
            "Must specify at least either a URI, or an audio and/or video payloader, or a muxer + payloader";
    }

    /* payloaders must be specified when an encoder is but no muxer is */
    if (video_encoder && !video_payloader && !muxer)
      return
          "A video payloader or muxer must be specified when a video encoder is";
    if (audio_encoder && !audio_payloader && !muxer)
      return
          "An audio payloader or muxer must be specified when an audio encoder is";
    if (muxer && !muxer_payloader)
      return "An muxer payloader must be specified when a muxer is";
  }

  return NULL;
}

static gboolean
rtsp_test_start (InsanityTest * test)
{
  GValue uri = { 0 };
  GValue video_encoder = { 0 }, audio_encoder = {
  0}, video_payloader = {
  0}, audio_payloader = {
  0};
  GValue muxer = { 0 }, muxer_payloader = {
  0};
  const char *protocol;
  gboolean configured = FALSE;
  const char *uri_string;
  const char *error;

  /* Fetch arguments */
  if (!insanity_test_get_argument (test, "uri", &uri))
    return FALSE;
  if (!insanity_test_get_argument (test, "video-encoder", &video_encoder))
    return FALSE;
  if (!insanity_test_get_argument (test, "video-payloader", &video_payloader))
    return FALSE;
  if (!insanity_test_get_argument (test, "audio-encoder", &audio_encoder))
    return FALSE;
  if (!insanity_test_get_argument (test, "audio-payloader", &audio_payloader))
    return FALSE;
  if (!insanity_test_get_argument (test, "muxer", &muxer))
    return FALSE;
  if (!insanity_test_get_argument (test, "muxer-payloader", &muxer_payloader))
    return FALSE;

  /* Check the setup makes sense */
  error = rtsp_test_check_arguments (&uri,
      &video_encoder, &video_payloader, &audio_encoder, &audio_payloader,
      &muxer, &muxer_payloader);
  if (error) {
    insanity_test_validate_checklist_item (test, "valid-setup", FALSE, error);
    goto done;
  }

  uri_string = rtsp_test_get_string (&uri);
  if (!uri_string) {
    configured = rtsp_test_configure_server_for_multiple_streams (test,
        rtsp_test_get_string (&video_encoder),
        rtsp_test_get_string (&video_payloader),
        rtsp_test_get_string (&audio_encoder),
        rtsp_test_get_string (&audio_payloader), rtsp_test_get_string (&muxer),
        rtsp_test_get_string (&muxer_payloader));
    global_live = TRUE;
  } else {
    if (!gst_uri_is_valid (uri_string)) {
      insanity_test_validate_checklist_item (test, "valid-setup", FALSE,
          "URI is invalid");
      goto done;
    }
    protocol = gst_uri_get_protocol (uri_string);
    if (!protocol || g_ascii_strcasecmp (protocol, "file")) {
      insanity_test_validate_checklist_item (test, "valid-setup", FALSE,
          "URI protocol must be file");
      goto done;
    }

    configured = rtsp_test_configure_server_for_uri (uri_string);
    global_live = FALSE;
  }

  insanity_test_validate_checklist_item (test, "valid-setup", configured, NULL);

  global_state = 0;
  global_next_state = 0;
  global_waiting_on_state = GST_STATE_PLAYING;

done:
  g_value_unset (&uri);
  g_value_unset (&video_encoder);
  g_value_unset (&video_payloader);
  g_value_unset (&audio_encoder);
  g_value_unset (&audio_payloader);
  g_value_unset (&muxer);
  g_value_unset (&muxer_payloader);
  return configured;
}

static void
rtsp_test_stop (InsanityTest * test)
{
  if (global_timer_id) {
    g_source_remove (global_timer_id);
    global_timer_id = 0;
  }

  /* This seems too late and causes deleted data in the rtsp server to be accessed,
     though I'm not sure just why */
  /* rtsp_test_reset_server (); */
}

static gboolean
state_change_timeout (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;

  on_ready_for_next_state (ptest, TRUE);

  return FALSE;
}

static NextStepTrigger
rtsp_test_pause (InsanityGstPipelineTest * ptest, const char *step,
    guintptr data)
{
  GstStateChangeReturn sret;

  global_state_change_timeout =
      g_timeout_add (5000, (GSourceFunc) & state_change_timeout, ptest);
  sret = gst_element_set_state (global_pipeline, GST_STATE_PAUSED);
  if (sret == GST_STATE_CHANGE_SUCCESS) {
    /* If this was done already, we can switch now */
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest), step, TRUE,
        NULL);
    return NEXT_STEP_NOW;
  }

  return NEXT_STEP_ON_PAUSED;
}

static NextStepTrigger
rtsp_test_play (InsanityGstPipelineTest * ptest, const char *step,
    guintptr data)
{
  GstStateChangeReturn sret;

  global_state_change_timeout =
      g_timeout_add (5000, (GSourceFunc) & state_change_timeout, ptest);
  sret = gst_element_set_state (global_pipeline, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_SUCCESS) {
    /* If this was done already, we can switch now */
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest), step, TRUE,
        NULL);
    return NEXT_STEP_NOW;
  }

  return NEXT_STEP_ON_PLAYING;
}

static NextStepTrigger
rtsp_test_wait (InsanityGstPipelineTest * ptest, const char *step,
    guintptr data)
{
  GstClockTime *t = (GstClockTime *) data;

  /* First call, generate a stream time to wait for */
  if (*t == GST_CLOCK_TIME_NONE) {
    *t = rtsp_test_get_wait_time (INSANITY_TEST (ptest));
    if (*t == GST_CLOCK_TIME_NONE) {
      insanity_test_validate_checklist_item (INSANITY_TEST (ptest), step, FALSE,
          "Failed to generate a wait time");
      return NEXT_STEP_NOW;
    }
    return NEXT_STEP_RESTART_ON_TICK;
  }

  /* Wait till next tick if we're not there yet */
  if (GST_CLOCK_TIME_IS_VALID (*t)
      && rtsp_test_get_position (INSANITY_TEST (ptest)) < *t)
    return NEXT_STEP_RESTART_ON_TICK;

  /* We reached the time */
  *t = GST_CLOCK_TIME_NONE;
  insanity_test_validate_checklist_item (INSANITY_TEST (ptest), step, TRUE,
      NULL);
  return NEXT_STEP_NOW;
}

static NextStepTrigger
rtsp_test_seek (InsanityGstPipelineTest * ptest, const char *step,
    guintptr data)
{
  GstClockTime t = GST_SECOND * 10;
  gboolean res;

  /* We can't seek in a live pipeline */
  if (global_live) {
    insanity_test_printf (INSANITY_TEST (ptest),
        "Skipping seek test in live pipeline");
    return NEXT_STEP_NOW;
  }

  res =
      gst_element_seek (global_pipeline, 1.0, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, t, GST_SEEK_TYPE_NONE,
      GST_CLOCK_TIME_NONE);
  if (!res) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest), step,
        FALSE, "Failed to send seek event");
    return NEXT_STEP_NOW;
  }
  global_state_change_timeout =
      g_timeout_add (5000, (GSourceFunc) & state_change_timeout, ptest);
  return NEXT_STEP_ON_PLAYING;
}

static NextStepTrigger
rtsp_test_set_protocols (InsanityGstPipelineTest * ptest, const char *step,
    guintptr data)
{
  guint protocols = data;
  GstElement *source;

  /* We can't set certain protocols that with a live pipeline */
  if (global_live) {
    if (protocols == 1 || protocols == 2) {     /* UDP multicast/unicast - unicast might be OK ? */
      insanity_test_printf (INSANITY_TEST (ptest),
          "Skipping protocols %u in live pipeline", protocols);
      return NEXT_STEP_NOW;
    }
  }

  gst_element_set_state (global_pipeline, GST_STATE_READY);
  g_object_get (global_pipeline, "source", &source, NULL);
  g_object_set (source, "protocols", protocols, NULL);
  gst_object_unref (source);
  gst_element_set_state (global_pipeline, GST_STATE_PAUSED);
  gst_element_get_state (global_pipeline, NULL, NULL, GST_SECOND);
  gst_element_set_state (global_pipeline, GST_STATE_PLAYING);

  global_state_change_timeout =
      g_timeout_add (5000, (GSourceFunc) & state_change_timeout, ptest);
  return NEXT_STEP_ON_PLAYING;
}

/*
  This is an ordered list of steps to perform.
  A test function needs the passed step if needed, and validate it UNLESS it returns
  NEXT_STEP_ON_PLAYING, in which case validation will be automatically done whenever
  next state change to PLAYING happens. A step can return NEXT_STEP_RESTART_ON_PLAYING,
  which will cause it to be called again upon next state change to PLAYING. The step
  function is then responsible for keeping its own private state so it can end up
  returning something else at some point.
*/
static const struct
{
  const char *step;
    NextStepTrigger (*f) (InsanityGstPipelineTest *, const char *, guintptr);
  guintptr data;
} steps[] = {
  {
  "play", &rtsp_test_play, 0}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time}, {
  "pause", &rtsp_test_pause, 0}, {
  "play", &rtsp_test_play, 0}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time},
      /*{ "seek", &rtsp_test_seek, 0 }, *//* fails to send event, disabled for now */
  {
  "protocol-udp-unicast", &rtsp_test_set_protocols, 1}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time}, {
  "protocol-udp-multicast", &rtsp_test_set_protocols, 2}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time}, {
  "protocol-tcp", &rtsp_test_set_protocols, 4}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time}, {
  "protocol-http", &rtsp_test_set_protocols, 0x10}, {
  "wait", &rtsp_test_wait, (guintptr) & global_wait_time},
      /* add more here */
};

static gboolean
do_next_step (gpointer data)
{
  InsanityGstPipelineTest *ptest = data;
  InsanityTest *test = data;
  NextStepTrigger next;

  /* When out of steps to perform, end the test */
  if (global_state == sizeof (steps) / sizeof (steps[0])) {
    insanity_test_done (test);
    return FALSE;
  }

  insanity_test_printf (test, "Calling step %u/%zu (%s, data %u)\n",
      global_state + 1, sizeof (steps) / sizeof (steps[0]),
      steps[global_state].step, steps[global_state].data);
  next =
      (*steps[global_state].f) (ptest, steps[global_state].step,
      steps[global_state].data);
  switch (next) {
    default:
      g_assert (0);
      /* fall through */
    case NEXT_STEP_NOW:
      global_state++;
      g_idle_add ((GSourceFunc) & do_next_step, ptest);
      break;
    case NEXT_STEP_ON_PLAYING:
      global_next_state = global_state + 1;
      global_waiting_on_state = GST_STATE_PLAYING;
      break;
    case NEXT_STEP_ON_PAUSED:
      global_next_state = global_state + 1;
      global_waiting_on_state = GST_STATE_PAUSED;
      break;
    case NEXT_STEP_RESTART_ON_PLAYING:
      global_next_state = global_state;
      global_waiting_on_state = GST_STATE_PLAYING;
      break;
    case NEXT_STEP_RESTART_ON_TICK:
      global_next_state = global_state;
      global_timer_id = g_timeout_add (250, (GSourceFunc) & do_next_step, test);
      break;
  }

  return FALSE;
}

static void
on_ready_for_next_state (InsanityGstPipelineTest * ptest, gboolean timeout)
{
  if (global_next_state != global_state) {
    insanity_test_validate_checklist_item (INSANITY_TEST (ptest),
        steps[global_state].step, TRUE, NULL);
    global_state = global_next_state;
  }
  insanity_test_printf (INSANITY_TEST (ptest), "Got %s, going to next step\n",
      timeout ? "timeout" :
      gst_element_state_get_name (global_waiting_on_state));
  global_waiting_on_state = GST_STATE_VOID_PENDING;
  g_idle_add ((GSourceFunc) & do_next_step, ptest);
}

static gboolean
rtsp_test_bus_message (InsanityGstPipelineTest * ptest, GstMessage * msg)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (msg) == GST_OBJECT (global_pipeline)) {
        GstState oldstate, newstate, pending;
        gst_message_parse_state_changed (msg, &oldstate, &newstate, &pending);

        if (global_state_change_timeout) {
          g_source_remove (global_state_change_timeout);
          global_state_change_timeout = 0;
        }

        if (pending == GST_STATE_VOID_PENDING
            && global_waiting_on_state == newstate) {
          on_ready_for_next_state (ptest, FALSE);
        }
      }
      break;
    default:
      break;
  }

  return TRUE;
}

static gboolean
rtsp_test_reached_initial_state (InsanityThreadedTest * ttest)
{
  if (gst_element_set_state (global_pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    insanity_test_done (INSANITY_TEST (ttest));
    return FALSE;
  }
  if (gst_element_get_state (global_pipeline, NULL, NULL,
          GST_SECOND) == GST_STATE_CHANGE_FAILURE) {
    insanity_test_done (INSANITY_TEST (ttest));
    return FALSE;
  }

  return TRUE;
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;

  g_type_init ();

  ptest =
      insanity_gst_pipeline_test_new ("rtsp-test",
      "Test RTSP using gst-rtsp-server", NULL);
  test = INSANITY_TEST (ptest);

  insanity_test_add_string_argument (test, "uri",
      "The file URI to use for streaming (file only)", NULL, FALSE, "");
  insanity_test_add_string_argument (test, "video-encoder",
      "The optional video encoder element to use to encode test video when not streaming a URI",
      NULL, FALSE, "");
  insanity_test_add_string_argument (test, "video-payloader",
      "The video payloader element to use to payload test video when not streaming a URI",
      NULL, FALSE, "");
  insanity_test_add_string_argument (test, "audio-encoder",
      "The optional audio encoder element to use to encode test audio when not streaming a URI",
      NULL, FALSE, "");
  insanity_test_add_string_argument (test, "audio-payloader",
      "The audio payloader element to use to payload test audio when not streaming a URI",
      NULL, FALSE, "");
  insanity_test_add_string_argument (test, "muxer",
      "The muxer element to use, if any, when not streaming a URI", NULL, FALSE,
      "");
  insanity_test_add_string_argument (test, "muxer-payloader",
      "The payloader element to use, if using a muxer", NULL, FALSE, "");

  insanity_test_add_int_argument (test, "playback-time",
      "Stream time to playback for before seeking, in seconds", NULL, TRUE, 5);

  insanity_test_add_checklist_item (test, "valid-setup",
      "The setup given in arguments makes sense", NULL, FALSE);
  insanity_test_add_checklist_item (test, "server-created",
      "The RTSP server was created succesfully", NULL, FALSE);
  insanity_test_add_checklist_item (test, "pause",
      "The pipeline could be paused", NULL, FALSE);
  insanity_test_add_checklist_item (test, "play",
      "The pipeline could be played", NULL, FALSE);
  insanity_test_add_checklist_item (test, "wait",
      "The pipeline could stay in playback mode", NULL, FALSE);
  insanity_test_add_checklist_item (test, "seek", "The pipeline could seek",
      NULL, FALSE);
  insanity_test_add_checklist_item (test, "protocol-udp-unicast",
      "The RTP transport could be set to UDP unicast", NULL, FALSE);
  insanity_test_add_checklist_item (test, "protocol-udp-multicast",
      "The RTP transport could be set to UDP multicast", NULL, FALSE);
  insanity_test_add_checklist_item (test, "protocol-tcp",
      "The RTP transport could be set to TCP", NULL, FALSE);
  insanity_test_add_checklist_item (test, "protocol-http",
      "The RTP transport could be set to HTTP", NULL, FALSE);
  insanity_test_add_checklist_item (test, "position-queried",
      "Stream position could be determined", NULL, FALSE);

  insanity_test_add_extra_info (test, "launch-line",
      "The launch line gst-rtsp-server was configued with");

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &rtsp_test_create_pipeline, NULL, NULL);
  insanity_gst_pipeline_test_set_initial_state (ptest, GST_STATE_PAUSED);

  g_signal_connect_after (test, "setup", G_CALLBACK (&rtsp_test_setup), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&rtsp_test_teardown),
      0);
  g_signal_connect (test, "start", G_CALLBACK (&rtsp_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&rtsp_test_stop), 0);
  g_signal_connect_after (test, "reached-initial-state",
      G_CALLBACK (&rtsp_test_reached_initial_state), 0);
  g_signal_connect_after (test, "bus-message",
      G_CALLBACK (&rtsp_test_bus_message), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
