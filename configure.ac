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

LIBINSANITYGST_SHARED_VERSION="0:0:0"

GST_TARGET=0.10
AC_SUBST(GST_TARGET)

AC_PROG_CC
AC_PROG_CC_STDC
AC_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_PROG_GREP

AC_C_CONST

AM_PATH_PYTHON(,HAVE_PYTHON=yes, HAVE_PYTHON=no)
AM_CONDITIONAL(HAVE_PYTHON,test "${HAVE_PYTHON}" = "yes")

# headers

AC_HEADER_STDC

AC_CHECK_PROG(HAVE_PKG_CONFIG,pkg-config,yes)

PKG_CHECK_MODULES(GLIB, glib-2.0)
PKG_CHECK_MODULES(GOBJECT, gobject-2.0)
PKG_CHECK_MODULES(GTHREAD, gthread-2.0)
PKG_CHECK_MODULES(GST, gstreamer-${GST_TARGET})
PKG_CHECK_MODULES(INSANITY, insanity-1.0)

dnl check for gobject-introspection
GOBJECT_INTROSPECTION_CHECK([1.31.1])

dnl check for gtk-doc
GTK_DOC_CHECK([1.3])

dnl Check for documentation xrefs
GLIB_PREFIX="`$PKG_CONFIG --variable=prefix glib-2.0`"
AC_SUBST(GLIB_PREFIX)

AC_CONFIG_FILES(
  [Makefile]
  [lib/Makefile]
  [lib/insanity-gst/Makefile]
  [tests/Makefile]
  [generators/Makefile]
  [docs/Makefile]
  [lib/insanity-gst.pc]
  [lib/insanity-gst-uninstalled.pc]
)

ln -sf insanity-gst.pc lib/insanity-gst-$GST_TARGET.pc
ln -sf insanity-gst-uninstalled.pc lib/insanity-gst-$GST_TARGET-uninstalled.pc

AC_OUTPUT

# gtk-doc 
#  [docs/insanity-gst/Makefile]
#  [docs/insanity-gst/version.xml]
#  [docs/random/Makefile]

