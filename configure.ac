AC_PREREQ([2.64])
AC_INIT([serial-to-alsa],
	[0.1],
	[https://github.com/WeAreROLI/serial-to-alsa/issues],
	[serial-to-alsa],
	[https://github.com/WeAreROLI/serial-to-alsa])
AM_INIT_AUTOMAKE([1.14 foreign -Wall -Wno-portability no-define])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])

PKG_CHECK_MODULES([ALSA], [alsa], [], [AC_MSG_ERROR([*** ALSA lib required.])])

AC_SUBST([PTHREAD_CFLAGS], [-pthread])
AC_SUBST([PTHREAD_LIBS], [-pthread])

AC_CONFIG_SRCDIR([serial-to-alsa.c])
AC_CONFIG_HEADERS([config.h])
AC_PREFIX_DEFAULT(/usr)
AC_PROG_CC
AC_OUTPUT(Makefile)
