AC_INIT([insanity-gst],[0.0.1])

AC_CONFIG_SRCDIR([lib/insanity-gst/insanitygsttest.h])

AC_PREREQ(2.53)

AC_CANONICAL_TARGET

AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AM_MAINTAINER_MODE

AC_CONFIG_HEADERS([lib/insanity-gst/config.h])

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])],
  [AM_DEFAULT_VERBOSITY=1
   AC_SUBST(AM_DEFAULT_VERBOSITY)])

dnl  3. If the library source code has changed at all since the last
dnl     update, then increment REVISION (`C:R:A' becomes `C:r+1:A').
dnl
dnl  4. If any interfaces have been added, removed, or changed since the
dnl     last update, increment CURRENT, and set REVISION to 0.
dnl
dnl  5. If any interfaces have been added since the last public release,
dnl     then increment AGE.
dnl
dnl  6. If any interfaces have been removed since the last public release,
dnl     then set AGE to 0.
LIBINSANITY_GST_SHARED_VERSION="0:0:0"
AC_SUBST(LIBINSANITY_GST_SHARED_VERSION)
LIBINSANITY_GST_API_VERSION=0.10
AC_SUBST(LIBINSANITY_GST_API_VERSION)
GST_TARGET=0.10
AC_SUBST(GST_TARGET)

AM_PROG_AR

AC_PROG_CC
AC_PROG_CC_STDC
AC_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_PROG_GREP

AC_C_CONST

# Check compiler warning flags
WARNING_CFLAGS=""
for i in -Wall -Wdeclaration-after-statement -Wvla -Wpointer-arith -Wmissing-declarations -Wmissing-prototypes -Wredundant-decls -Wundef -Wwrite-strings -Wformat-nonliteral -Wformat-security -Wold-style-definition -Winit-self -Wmissing-include-dirs -Waddress -Waggregate-return -Wno-multichar -Wnested-externs; do
    AS_COMPILER_FLAG($i, WARNING_CFLAGS="$WARNING_CFLAGS $i")
done

AC_SUBST(WARNING_CFLAGS)

AM_PATH_PYTHON(,HAVE_PYTHON=yes, HAVE_PYTHON=no)
AM_CONDITIONAL(HAVE_PYTHON,test "${HAVE_PYTHON}" = "yes")

AC_SUBST(ACLOCAL_AMFLAGS, "-I m4")

# headers

AC_HEADER_STDC
AC_CHECK_HEADERS([ifaddrs.h])

AC_CHECK_PROG(HAVE_PKG_CONFIG,pkg-config,yes)

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.30)
PKG_CHECK_MODULES(GOBJECT, gobject-2.0 >= 2.30)
PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.30)
PKG_CHECK_MODULES(GIO, gio-2.0 >= 2.30)
PKG_CHECK_MODULES(GST, gstreamer-${GST_TARGET})
PKG_CHECK_MODULES(GST_BASE, gstreamer-base-${GST_TARGET})
PKG_CHECK_MODULES(GST_PBUTILS, gstreamer-pbutils-${GST_TARGET})
PKG_CHECK_MODULES(GST_INTERFACES, gstreamer-interfaces-${GST_TARGET})
PKG_CHECK_MODULES(GST_VIDEO, gstreamer-video-${GST_TARGET})
PKG_CHECK_MODULES(INSANITY, insanity-1.0)

dnl optional packages
PKG_CHECK_MODULES(SOUP, libsoup-2.4, HAVE_SOUP=yes, HAVE_SOUP=no)
AM_CONDITIONAL(HAVE_SOUP,test "$HAVE_SOUP" = "yes")
PKG_CHECK_MODULES(GST_RTSP_SERVER, gst-rtsp-server-0.10, HAVE_GST_RTSP_SERVER=yes, HAVE_GST_RTSP_SERVER=no)
AM_CONDITIONAL(HAVE_GST_RTSP_SERVER,test "$HAVE_GST_RTSP_SERVER" = "yes")

dnl check for gobject-introspection
GOBJECT_INTROSPECTION_CHECK([1.31.1])

dnl check for gtk-doc
GTK_DOC_CHECK([1.3])
AC_CHECK_PROGS(GTKDOC_REBASE,[gtkdoc-rebase])

dnl Check for documentation xrefs
GLIB_PREFIX="`$PKG_CONFIG --variable=prefix glib-2.0`"
AC_SUBST(GLIB_PREFIX)

dnl Add parameters for aclocal
AC_SUBST(ACLOCAL_AMFLAGS, "-I m4 -I common/m4")

AC_CONFIG_FILES(
  [Makefile]
  [common/Makefile]
  [common/m4/Makefile]
  [lib/Makefile]
  [lib/insanity-gst/Makefile]
  [tests/Makefile]
  [generators/Makefile]
  [docs/Makefile]
  [docs/insanity-gst/Makefile]
  [docs/insanity-gst/version.xml]
  [tools/Makefile]
  [lib/insanity-gst.pc]
  [lib/insanity-gst-uninstalled.pc]
)

AC_OUTPUT

