# All tests MUST start with "insanity-test-". Failure to do so
# will cause failure to be considered by Insanity. Reason is
# to avoid inadvertently running random binaries when probing
# for tests in the wrong place. Only works for mistakes.

LIBTOOLFLAGS=--silent

AM_CPPFLAGS=-I $(top_srcdir)/lib

testdir=$(libexecdir)/insanity-gst/tests

common_cflags=$(INSANITY_CFLAGS) $(GST_CFLAGS) $(GLIB_CFLAGS) $(GOBJECT_CFLAGS) $(GTHREAD_CFLAGS) $(WARNING_CFLAGS)
common_ldadd=$(INSANITY_LIBS) $(GST_LIBS) $(GLIB_LIBS) $(GOBJECT_LIBS) $(GTHREAD_LIBS)

insanity_test_gst_generic_pipeline_SOURCES=insanity-test-gst-generic-pipeline.c
insanity_test_gst_generic_pipeline_CFLAGS=$(common_cflags)
insanity_test_gst_generic_pipeline_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_play_SOURCES=insanity-test-gst-play.c
insanity_test_gst_play_CFLAGS=$(common_cflags)
insanity_test_gst_play_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_seek_SOURCES=insanity-test-gst-seek.c
insanity_test_gst_seek_CFLAGS=$(common_cflags)
insanity_test_gst_seek_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_dvd_SOURCES=insanity-test-gst-dvd.c
insanity_test_gst_dvd_CFLAGS=$(GST_INTERFACES_CFLAGS) $(common_cflags)
insanity_test_gst_dvd_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_INTERFACES_LIBS) $(common_ldadd)

insanity_test_gst_stream_switch_SOURCES=insanity-test-gst-stream-switch.c
insanity_test_gst_stream_switch_CFLAGS=$(GST_VIDEO_CFLAGS) $(GST_BASE_CFLAGS) $(common_cflags)
insanity_test_gst_stream_switch_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_VIDEO_LIBS) $(GST_BASE_LIBS) $(common_ldadd)

if HAVE_SOUP
insanity_test_gst_http_SOURCES=insanity-test-gst-http.c
insanity_test_gst_http_CFLAGS=$(SOUP_CFLAGS) $(common_cflags)
insanity_test_gst_http_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(SOUP_LIBS) $(common_ldadd)
http_test=insanity-test-gst-http
endif

test_PROGRAMS=\
    insanity-test-gst-generic-pipeline \
    insanity-test-gst-play \
    insanity-test-gst-seek \
    insanity-test-gst-dvd \
    insanity-test-gst-stream-switch \
    $(http_test)

TESTS=run-insanity-test-gst-generic-pipeline

EXTRA_DIST=run-insanity-test-gst-generic-pipeline
