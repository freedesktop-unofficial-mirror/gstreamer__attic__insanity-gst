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
insanity_test_gst_play_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd) $(GIO_LIBS) libinsanityhelper.la

insanity_test_gst_seek_SOURCES=insanity-test-gst-seek.c
insanity_test_gst_seek_CFLAGS=$(common_cflags)
insanity_test_gst_seek_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd) $(GIO_LIBS) libinsanityhelper.la

insanity_test_gst_dvd_SOURCES=insanity-test-gst-dvd.c
insanity_test_gst_dvd_CFLAGS=$(GST_INTERFACES_CFLAGS) $(common_cflags)
insanity_test_gst_dvd_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_INTERFACES_LIBS) $(common_ldadd)

insanity_test_gst_stream_switch_SOURCES=insanity-test-gst-stream-switch.c
insanity_test_gst_stream_switch_CFLAGS=$(GST_VIDEO_CFLAGS) $(GST_BASE_CFLAGS) $(common_cflags)
insanity_test_gst_stream_switch_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_VIDEO_LIBS) $(GST_BASE_LIBS) $(common_ldadd)

insanity_test_gst_discoverer_SOURCES=insanity-test-gst-discoverer.c
insanity_test_gst_discoverer_CFLAGS=$(GST_PBUTILS_CFLAGS) $(common_cflags)
insanity_test_gst_discoverer_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_PBUTILS_LIBS) $(common_ldadd)

if HAVE_GST_RTSP_SERVER
insanity_test_gst_rtsp_SOURCES=insanity-test-gst-rtsp.c
insanity_test_gst_rtsp_CFLAGS=$(GST_RTSP_SERVER_CFLAGS) $(common_cflags)
insanity_test_gst_rtsp_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(GST_RTSP_SERVER_LIBS) $(common_ldadd)
rtsp_test=insanity-test-gst-rtsp
endif

if HAVE_SOUP
libinsanityhttphelper_la_LIBADD=$(GIO_LIBS) $(SOUP_LIBS) $(common_ldadd)
libinsanityhttphelper_la_CFLAGS=$(GIO_CFLAGS) $(SOUP_CFLAGS) $(common_cflags)
libinsanityhttphelper_la_SOURCES = insanity-http-server.c
http_noinst_libraries=libinsanityhttphelper.la
http_noinst_headers=insanity-http-server.h

insanity_test_gst_http_SOURCES=insanity-test-gst-http.c
insanity_test_gst_http_CFLAGS=$(GIO_CFLAGS) $(SOUP_CFLAGS) $(common_cflags)
insanity_test_gst_http_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la libinsanityhttphelper.la $(GIO_LIBS) $(SOUP_LIBS) $(common_ldadd)

insanity_test_gst_hls_SOURCES=insanity-test-gst-hls.c
insanity_test_gst_hls_CFLAGS=$(GIO_CFLAGS) $(SOUP_CFLAGS) $(common_cflags)
insanity_test_gst_hls_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la libinsanityhttphelper.la $(GIO_LIBS) $(SOUP_LIBS) $(common_ldadd)

soup_tests=insanity-test-gst-http \
    insanity-test-gst-hls
endif

libinsanityhelper_la_LIBADD=$(GIO_LIBS) $(common_ldadd)
libinsanityhelper_la_CFLAGS=$(GIO_CFLAGS) $(common_cflags)
libinsanityhelper_la_SOURCES = insanity-file-appsrc.c
appsrc_noinst_libraries=libinsanityhelper.la
appsrc_noinst_headers=insanity-file-appsrc.h

noinst_LTLIBRARIES=$(http_noinst_libraries) $(appsrc_noinst_libraries)
noinst_HEADERS=$(http_noinst_headers) $(appsrc_noinst_headers)

BUILT_SOURCES = $(built_headers) $(built_sources)
EXTRA_DIST = run-insanity-test-gst-generic-pipeline
CLEANFILES = $(BUILT_SOURCES)

test_PROGRAMS=\
    insanity-test-gst-generic-pipeline \
    insanity-test-gst-play \
    insanity-test-gst-seek \
    insanity-test-gst-dvd \
    insanity-test-gst-stream-switch \
    insanity-test-gst-discoverer \
    $(soup_tests) \
    $(rtsp_test)

TESTS=run-insanity-test-gst-generic-pipeline
