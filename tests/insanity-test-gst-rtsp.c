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

static GstElement *global_pipeline = NULL;
static GstRTSPServer *global_server = NULL;

static GstPipeline*
rtsp_test_create_pipeline (InsanityGstPipelineTest *ptest, gpointer userdata)
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
    insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
      error ? error->message : NULL);
    if (error)
      g_error_free (error);
    return NULL;
  }
  else if (error) {
    /* Do we get a dangling pointer here ? gst-launch.c does not unref */
    pipeline = NULL;
    insanity_test_validate_step (INSANITY_TEST (ptest), "valid-pipeline", FALSE,
      error->message);
    g_error_free (error);
    return NULL;
  }

  global_pipeline = pipeline;

  return GST_PIPELINE (pipeline);
}

static const char *
rtsp_test_create_rtsp_server(void)
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
rtsp_test_setup(InsanityTest *test)
{
  const char *error = rtsp_test_create_rtsp_server ();
  insanity_test_validate_step (test, "server-created", error == NULL, error);
  return error == NULL;
}

static void
rtsp_test_teardown (InsanityTest *test)
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
  /*gst_rtsp_media_factory_set_shared (factory, TRUE);*/

  /* attach the test factory to the /test url; any previous factory will be unreffed */
  gst_rtsp_media_mapping_add_factory (mapping, "/test", GST_RTSP_MEDIA_FACTORY (factory));
  g_object_unref (mapping);

  return TRUE;
}

static char *
rtsp_test_make_path (const char *source,const char *transform,const char *pay,unsigned int *payidx, unsigned int *ptidx)
{
  if (pay)
    return g_strdup_printf ("%s is-live=1 ! %s ! %s name=pay%u pt=%u",
        source, transform ? transform : "identity", pay, *payidx++, *ptidx++);
  else
    return g_strdup_printf ("%s is-live=1 ! %s name=%s-path",
        source, transform ? transform : "identity", source);
}

static gboolean
rtsp_test_configure_server_for_multiple_streams(InsanityTest *test,
    const char *video_encoder, const char *video_payloader,
    const char *audio_encoder, const char *audio_payloader)
{
  GstRTSPMediaMapping *mapping;
  GstRTSPMediaFactory *factory;
  char *video_path = NULL, *audio_path = NULL, *sbin = NULL;
  unsigned payloader_index = 0, pt_index = 96;
  GValue vsbin = {0};

  if (video_payloader) {
    video_path = rtsp_test_make_path ("videotestsrc", video_encoder, video_payloader, &payloader_index, &pt_index);
  }
  if (audio_payloader) {
    audio_path = rtsp_test_make_path ("audiotestsrc", audio_encoder, audio_payloader, &payloader_index, &pt_index);
  }

  /* get the mapping for this server, every server has a default mapper object
   * that be used to map uri mount points to media factories */
  mapping = gst_rtsp_server_get_media_mapping (global_server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines. 
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */
  factory = gst_rtsp_media_factory_new ();
  sbin = g_strdup_printf ("( %s %s )", video_path ? video_path : "", audio_path ? audio_path : "");
  gst_rtsp_media_factory_set_launch (factory, sbin);
  /*gst_rtsp_media_factory_set_shared (factory, TRUE);*/

  /* attach the test factory to the /test url; any previous factory will be unreffed */
  gst_rtsp_media_mapping_add_factory (mapping, "/test", GST_RTSP_MEDIA_FACTORY (factory));
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
rtsp_test_get_string (const GValue *v)
{
  const char *s = g_value_get_string (v);
  if (!s || !*s) return NULL;
  return s;
}

static const char *
rtsp_test_check_arguments (const GValue *vuri,
    const GValue *vvideo_encoder, const GValue *vvideo_payloader,
    const GValue *vaudio_encoder, const GValue *vaudio_payloader)
{
  const char *uri = rtsp_test_get_string (vuri);
  const char *video_encoder = rtsp_test_get_string (vvideo_encoder);
  const char *video_payloader = rtsp_test_get_string (vvideo_payloader);
  const char *audio_encoder = rtsp_test_get_string (vaudio_encoder);
  const char *audio_payloader = rtsp_test_get_string (vaudio_payloader);

  /* URI must be set if all others are not, and not if any other is */
  if (uri) {
    if (video_encoder || video_payloader)
      return "URI and video encoder/payloader are conflicting";
    if (audio_encoder || audio_payloader)
      return "URI and video encoder/payloader are conflicting";
  }
  else {
    /* payloader for audio or video must be specified */
    if (!video_payloader && !audio_payloader)
      return "Must specify at least either a URI, or an audio and/or video payloader";

    /* payloaders must be specified when an encoder is */
    if (video_encoder && !video_payloader)
      return "A video payloader must be specified when a video encoder is";
    if (audio_encoder && !audio_payloader)
      return "An audio payloader must be specified when an audio encoder is";
  }

  return NULL;
}

static gboolean
rtsp_test_start(InsanityTest *test)
{
  GValue uri = {0};
  GValue video_encoder = {0}, audio_encoder = {0}, video_payloader = {0}, audio_payloader = {0};
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

  /* Check the setup makes sense */
  error = rtsp_test_check_arguments (&uri, &video_encoder, &video_payloader, &audio_encoder, &audio_payloader);
  if (error) {
    insanity_test_validate_step (test, "valid-setup", FALSE, error);
    goto done;
  }

  uri_string = rtsp_test_get_string (&uri);
  if (!uri_string) {
    configured = rtsp_test_configure_server_for_multiple_streams(test,
        rtsp_test_get_string (&video_encoder), rtsp_test_get_string (&video_payloader),
        rtsp_test_get_string (&audio_encoder), rtsp_test_get_string (&audio_payloader));
  }
  else {
    if (!gst_uri_is_valid (uri_string)) {
      insanity_test_validate_step (test, "valid-setup", FALSE, "URI is invalid");
      goto done;
    }
    protocol = gst_uri_get_protocol (uri_string);
    if (!protocol || g_ascii_strcasecmp (protocol, "file")) {
      insanity_test_validate_step (test, "valid-setup", FALSE, "URI protocol must be file");
      goto done;
    }

    configured = rtsp_test_configure_server_for_uri (uri_string);
  }

  insanity_test_validate_step (test, "valid-setup", configured, NULL);

done:
  g_value_unset (&uri);
  g_value_unset (&video_encoder);
  g_value_unset (&video_payloader);
  g_value_unset (&audio_encoder);
  g_value_unset (&audio_payloader);
  return configured;
}

static void
rtsp_test_stop (InsanityTest *test)
{
  rtsp_test_reset_server ();
}

static void
rtsp_test_test (InsanityTest *test)
{
}

int
main (int argc, char **argv)
{
  InsanityGstPipelineTest *ptest;
  InsanityTest *test;
  gboolean ret;

  g_type_init ();

  ptest = insanity_gst_pipeline_test_new ("gst-rtsp", "Test RTSP using gst-rtsp-server", NULL);
  test = INSANITY_TEST (ptest);

  insanity_test_add_string_argument (test, "uri", "The file URI to use for streaming (file only)", NULL, FALSE, "");
  insanity_test_add_string_argument (test, "video-encoder", "The optional video encoder element to use to encode test video when not streaming a URI", NULL, FALSE, "");
  insanity_test_add_string_argument (test, "video-payloader", "The video payloader element to use to payload test video when not streaming a URI", NULL, FALSE, "");
  insanity_test_add_string_argument (test, "audio-encoder", "The optional audio encoder element to use to encode test audio when not streaming a URI", NULL, FALSE, "");
  insanity_test_add_string_argument (test, "audio-payloader", "The audio payloader element to use to payload test audio when not streaming a URI", NULL, FALSE, "");

  insanity_test_add_checklist_item (test, "valid-setup", "The setup given in arguments makes sense", NULL);
  insanity_test_add_checklist_item (test, "server-created", "The RTSP server was created succesfully", NULL);
  insanity_test_add_checklist_item (test, "launch-line", "The launch line gst-rtsp-server was configued with", NULL);

  insanity_gst_pipeline_test_set_create_pipeline_function (ptest,
      &rtsp_test_create_pipeline, NULL, NULL);

  g_signal_connect_after (test, "setup", G_CALLBACK (&rtsp_test_setup), 0);
  g_signal_connect_after (test, "teardown", G_CALLBACK (&rtsp_test_teardown), 0);
  g_signal_connect_after (test, "start", G_CALLBACK (&rtsp_test_start), 0);
  g_signal_connect_after (test, "stop", G_CALLBACK (&rtsp_test_stop), 0);
  g_signal_connect_after (test, "test", G_CALLBACK (&rtsp_test_test), 0);

  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}