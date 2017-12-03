## Process this file with autoconf to produce configure.
## In general, the safest way to proceed is to run ./autogen.sh

# make sure we're interpreted by some minimal autoconf
AC_PREREQ([2.59])

AC_INIT([malloc-trace-replay],[0.0.1],[gperftools@googlegroups.com])

AC_CONFIG_SRCDIR(README)
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])

AC_PROG_RANLIB
AC_PROG_CXX
AX_CXX_COMPILE_STDCXX_11([ext], [mandatory])

PKG_CHECK_MODULES([capnp], [capnp kj])

AC_ARG_VAR([CAPNP], [capnp tool to invoke])
AS_IF([test "x$CAPNP" = "x"], [AC_SUBST([CAPNP], [capnp])])

AX_GENERATE_CHANGELOG

# Write generated configuration file
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
