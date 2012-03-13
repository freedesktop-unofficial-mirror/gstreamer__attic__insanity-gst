# All tests MUST start with "insanity-test-". Failure to do so
# will cause failure to be considered by Insanity. Reason is
# to avoid inadvertently running random binaries when probing
# for tests in the wrong place. Only works for mistakes.

LIBTOOLFLAGS=--silent

AM_CPPFLAGS=-I $(top_srcdir)/lib

testdir=$(libexecdir)/insanity-gst/tests

common_cflags=$(INSANITY_CFLAGS) $(GST_CFLAGS) $(GLIB_CFLAGS) $(GOBJECT_CFLAGS) $(GTHREAD_CFLAGS) $(DBUS_CFLAGS)
common_ldadd=$(INSANITY_LIBS) $(GST_LIBS) $(GLIB_LIBS) $(GOBJECT_LIBS) $(GTHREAD_LIBS) $(DBUS_LIBS)

insanity_test_gst_blank_SOURCES=insanity-test-gst-blank.c
insanity_test_gst_blank_CFLAGS=$(common_cflags)
insanity_test_gst_blank_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_play_SOURCES=insanity-test-gst-play.c
insanity_test_gst_play_CFLAGS=$(common_cflags)
insanity_test_gst_play_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_seek_SOURCES=insanity-test-gst-seek.c
insanity_test_gst_seek_CFLAGS=$(common_cflags)
insanity_test_gst_seek_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

insanity_test_gst_dvd_SOURCES=insanity-test-gst-dvd.c
insanity_test_gst_dvd_CFLAGS=$(common_cflags)
insanity_test_gst_dvd_LDADD=../lib/insanity-gst/libinsanity-gst-@GST_TARGET@.la $(common_ldadd)

test_PROGRAMS=\
    insanity-test-gst-blank \
    insanity-test-gst-play \
    insanity-test-gst-seek \
    insanity-test-gst-dvd

TESTS=run-insanity-test-gst-blank

EXTRA_DIST=run-insanity-test-gst-blank
