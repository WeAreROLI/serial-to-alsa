ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}

AM_CFLAGS = $(ALSA_CFLAGS) $(PTHREAD_CFLAGS)
AM_LDFLAGS = $(ALSA_LIBS) $(PTHREAD_LIBS)

bin_PROGRAMS = serial-to-alsa
serial_to_alsa_SOURCES = serial-to-alsa.c
serial_to_alsa_LDFLAGS = $(AM_LDFLAGS)
serial_to_alsa_CFLAGS = $(AM_CFLAGS)

dist_noinst_SCRIPTS = autogen.sh
