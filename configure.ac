#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT(mod_ical, 1.0.0, minfrin@sharp.fm)
AM_INIT_AUTOMAKE([dist-bzip2])
AC_CONFIG_FILES([Makefile mod_ical.spec])
AC_CONFIG_SRCDIR([mod_ical.c])
AC_CONFIG_HEADER(config.h)

# Checks for programs.
AC_PROG_CC
AC_ARG_WITH(apxs,
    [  --with-apxs=PATH        path to Apache apxs],
    [
        if test "$withval" = "yes"; then
            AC_PATH_PROGS(APXS, apxs apxs2, reject, $PATH:/usr/sbin)
        else
            APXS=$withval
            AC_SUBST(APXS)
        fi
    ],
    [
        AC_PATH_PROGS(APXS, apxs apxs2, reject, $PATH:/usr/sbin)
    ])
if test "$APXS" = "reject"; then
  AC_MSG_ERROR([Could not find apxs on the path.])
fi

# Make sure the Apache include files are found
CPPFLAGS="$CPPFLAGS -I`$APXS -q INCLUDEDIR`"
CFLAGS="$CFLAGS -I`$APXS -q INCLUDEDIR`"

# Checks for libraries.
PKG_CHECK_MODULES(apr, apr-1 >= 1.2)
PKG_CHECK_MODULES(apu, apr-util-1 >= 1.2)
PKG_CHECK_MODULES(libical, libical >= 0.40)
PKG_CHECK_MODULES(libxml, libxml-2.0 > 2)
PKG_CHECK_MODULES(jsonc, json-c > 0.10)

CFLAGS="$CFLAGS $apr_CFLAGS $apu_CFLAGS $libical_CFLAGS $libxml_CFLAGS $jsonc_CFLAGS"
CPPFLAGS="$CPPFLAGS $apr_CPPFLAGS $apu_CPPFLAGS $libical_CPPFLAGS $libxml_CPPFLAGS $jsonc_CPPFLAGS"
LDFLAGS="$LDFLAGS $apr_LIBS $apu_LIBS $libical_LIBS $libxml_LIBS $jsonc_LIBS"

# Checks for header files.
AC_CHECK_HEADERS(libical/ical.h libxml/encoding.h json-c/json.h json.h)

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T

# Checks for library functions.
AC_CHECK_LIB(ical, icalparser_new)
AC_CHECK_LIB(xml2, htmlParseDocument)
AC_CHECK_LIB(json-c, json_object_new_object)
AC_CHECK_FUNCS([icalrecur_weekday_to_string icalrecurrencetype_month_is_leap icalrecurrencetype_month_month])

AC_SUBST(PACKAGE_VERSION)
AC_OUTPUT
