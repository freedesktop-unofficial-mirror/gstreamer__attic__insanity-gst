LIBTOOLFLAGS=--silent

AM_CPPFLAGS=-I $(top_srcdir)/lib

insanityincdir=$(includedir)/insanity-gst-@LIBINSANITY_GST_API_VERSION@

lib_LTLIBRARIES=libinsanity-gst-@LIBINSANITY_GST_API_VERSION@.la

libinsanity_gst_@LIBINSANITY_GST_API_VERSION@_la_SOURCES=\
  insanitygsttest.c \
  insanitygstpipelinetest.c

insanityinc_HEADERS=\
  insanitygsttest.h \
  insanitygstpipelinetest.h

libinsanity_gst_@LIBINSANITY_GST_API_VERSION@_la_LDFLAGS = -version-info @LIBINSANITY_GST_SHARED_VERSION@ -no-undefined -export-symbols-regex \^insanity_gst_.*
libinsanity_gst_@LIBINSANITY_GST_API_VERSION@_la_LIBADD=$(GST_LIBS) $(INSANITY_LIBS) $(GLIB_LIBS) $(GOBJECT_LIBS) $(GTHREAD_LIBS)
libinsanity_gst_@LIBINSANITY_GST_API_VERSION@_la_CFLAGS=$(GST_CFLAGS) $(INSANITY_CFLAGS) $(GLIB_CFLAGS) $(GOBJECT_CFLAGS) $(GTHREAD_CFLAGS) $(WARNING_CFLAGS)

if HAVE_INTROSPECTION
BUILT_GIRSOURCES = InsanityGst-@LIBINSANITY_GST_API_VERSION@.gir

gir_headers=$(patsubst %,$(srcdir)/%, $(insanityinc_HEADERS))
gir_sources=$(patsubst %,$(srcdir)/%, $(libinsanity_gst_@LIBINSANITY_GST_API_VERSION@_la_SOURCES))

InsanityGst-@LIBINSANITY_GST_API_VERSION@.gir: $(INTROSPECTION_SCANNER) libinsanity-gst-@LIBINSANITY_GST_API_VERSION@.la
	echo $(AM_V_GEN) $(INTROSPECTION_SCANNER) -v --namespace InsanityGst \
		--nsversion=@LIBINSANITY_GST_API_VERSION@ \
		--warn-all \
		-I$(top_srcdir)/lib \
		-I$(top_builddir)/lib \
		-DIN_GOBJECT_INTROSPECTION=1 \
		--c-include='insanity-gst/insanitygst.h' \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir gstreamer-@GST_TARGET@` \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir insanity-1.0` \
		--library=libinsanity-gst-@LIBINSANITY_GST_API_VERSION@.la \
		--include=Gst-@GST_TARGET@ \
		--include=Insanity-1.0 \
		--include=GLib-2.0 \
		--include=GObject-2.0 \
		--libtool="$(top_builddir)/libtool" \
		--pkg glib-2.0 \
		--pkg gobject-2.0 \
		--pkg gmodule-no-export-2.0 \
		--pkg gthread-2.0 \
		--pkg insanity-1.0 \
		--pkg gstreamer-@GST_TARGET@ \
		--pkg-export insanity-gst-@LIBINSANITY_GST_API_VERSION@ \
		--output $@ \
		$(gir_headers) \
		$(gir_sources)

# INTROSPECTION_GIRDIR/INTROSPECTION_TYPELIBDIR aren't the right place to
# install anything - we need to install inside our prefix.
girdir = $(datadir)/gir-1.0
gir_DATA = $(BUILT_GIRSOURCES)

typelibsdir = $(libdir)/girepository-1.0/

typelibs_DATA = $(BUILT_GIRSOURCES:.gir=.typelib)

%.typelib: %.gir $(INTROSPECTION_COMPILER)
	$(AM_V_GEN)$(INTROSPECTION_COMPILER) \
		--includedir=$(srcdir)/lib \
		--includedir=$(builddir)/lib \
		--includedir=`$(PKG_CONFIG) --variable=girdir gstreamer-@GST_TARGET@` \
		--includedir=`$(PKG_CONFIG) --variable=girdir insanity-1.0` \
		$(INTROSPECTION_COMPILER_OPTS) $< -o $(@F)

CLEANFILES = $(BUILT_GIRSOURCES) $(typelibs_DATA)
endif

