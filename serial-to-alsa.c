/*
 *  serial-to-alsa.c - copies MIDI messages from serial to ALSA with a
 *                     ring buffer.
 *
 *  Copyright (c) 2015 ROLI Ltd.
 *  Felipe F. Tonello <felipe.tonello@roli.com>
 *
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>
#include <sys/select.h>

#include <alsa/asoundlib.h>

#include "config.h"

#define COLOR_RED	"\033[31m"
#define COLOR_GREEN	"\033[32m"
#define COLOR_YELLOW	"\033[33m"
#define COLOR_RESET	"\033[0m"

#define eprint(format, ...)						\
	fprintf(stderr, COLOR_RED format COLOR_RESET, ##__VA_ARGS__);	\
	putc('\n', stderr)

struct sta_option {
	char *midi_port_name;
	char *serial_port_name;
};

static struct sta_option options = {
	.midi_port_name = "hw:1,0",
	.serial_port_name = "/dev/ttymxc1",
};

enum {
	T_ALSA,
	T_SERIAL,
	T_COUNT,
};

#define BUF_COUNT 16
#define BUF_SIZE 256

struct sta_userdata {
	snd_rawmidi_t *output;
	int fd;
	pthread_t t[T_COUNT];
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	uint8_t buf[BUF_COUNT][BUF_SIZE];
	size_t buf_count;
};

static bool stop = false;

static void usage()
{
	printf("Usage: serial-to-alsa options\n"
	       "\n"
	       "-h, --help              this help\n"
	       "-V, --version           print current version\n"
	       "-m, --midi-port=name    select port by name (default: hw:1,0)\n"
	       "-s, --serial-port=name  select port by name (default: /dev/ttymxc1)\n"
	       "\n");
}

static void version()
{
	puts("serial-to-alsa version " PACKAGE_VERSION);
}

static void * sta_malloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		eprint("out of memory");
		exit(EXIT_FAILURE);
	}
	return p;
}

static void sig_handler(int dummy)
{
	stop = true;
}

static int alsa_setup(snd_rawmidi_t **output /* OUT */)
{
	int err;
	if ((err = snd_rawmidi_open(NULL,
	                            output,
	                            options.midi_port_name,
	                            SND_RAWMIDI_NONBLOCK)) < 0) {
		eprint("ALSA: cannot open port \"%s\": %s",
		        options.midi_port_name, snd_strerror(err));
		goto end;
	}

	if ((err = snd_rawmidi_nonblock(*output, 0)) < 0) {
		eprint("ALSA: cannot set blocking mode: %s", snd_strerror(err));
		goto end;
	}

end:
	return err;
}

static int serial_setup(const char *port)
{
	int fd, err;
	struct termios tio;

	if ((fd = open(port, O_RDONLY | O_NOCTTY)) < 0) {
		eprint("SERIAL: cannot open port \"%s\": %s",
		        port, strerror(errno));
		err = fd;
		goto err;
	}

	if ((err = tcgetattr(fd, &tio)) < 0) {
		eprint("SERIAL: cannot get attributes from \"%s\": %s",
		        port, strerror(errno));
		goto err;
	}

	tio.c_cflag = CLOCAL | CREAD | CS8;
	tio.c_iflag = IGNCR | IGNPAR | IGNBRK; /* ignore everything we can */
	tio.c_lflag = ICANON; /* canonical mode */
	/* use 0xFF as our end-of-line character */
	tio.c_cc[VEOL]     = 0xFF;
	tio.c_cc[VEOL2]    = 0xFF;
	/* map everything else to a value we'll never see */
	tio.c_cc[VEOF]     = 0xFE;
	tio.c_cc[VERASE]   = 0xFE;
	tio.c_cc[VKILL]    = 0xFE;
	tio.c_cc[VLNEXT]   = 0xFE;
	tio.c_cc[VREPRINT] = 0xFE;
	tio.c_cc[VWERASE]  = 0xFE;

	if ((err = cfsetspeed(&tio, B230400)) < 0) {
		eprint("SERIAL: cannot set speed attribute for \"%s\": %s",
		        port, strerror(errno));
		goto err;
	}

	/* try: TCSANOW */
	if ((err = tcsetattr (fd, TCSAFLUSH, &tio)) < 0) {
		eprint("SERIAL: cannot set attributes for \"%s\": %s",
		        port, strerror(errno));
		goto err;
	}

	fsync(fd);
	tcflush(fd, TCIFLUSH);

	return fd;

err:
	if (fd > 0)
		close(fd);

	return err;
}

static void * alsa_worker(void *data)
{
	struct sta_userdata *u = data;

	assert(u);

	pthread_setname_np(pthread_self(), "ALSA Thread");

	while (!stop) {
		size_t i;

		if (pthread_mutex_lock(&u->mutex) != 0) {
			eprint("THREAD: cannot lock mutex in ALSA thread: %s",
			        strerror(errno));
			stop = true;
		}

		if (pthread_cond_wait(&u->condition, &u->mutex) != 0) {
			eprint("THREAD: cannot wait for condition variable in "
			        "ALSA thread: %s", strerror(errno));
			stop = true;
			goto mutex;
		}

		if (stop)
			goto mutex;

		if (u->buf_count == 0) {
			eprint("SERIAL: Buffer underflow...");
			fflush(stderr);
			goto mutex;
		}

		for (i = 0; i < u->buf_count; i++) {
			size_t j;
			int err;

			printf(COLOR_GREEN "MIDI --> ");

			j = 0;
			/* Don't send the 0xFF at then end */
			while (u->buf[i][j] != 0xFF) {
				printf("%02x ", u->buf[i][j]);
				j++;
			}

			if (j == 0) {
				printf("nothing to send");
			}

			printf("\n" COLOR_RESET);
			fflush(stdout);

			if ((j > 0) &&
			    ((err = snd_rawmidi_write(u->output, u->buf[i], j)) < 0)) {
				eprint("ALSA: cannot send data: %s",
				        snd_strerror(err));
				continue;
			}
		}

		u->buf_count = 0;

	mutex:
		if (pthread_mutex_unlock(&u->mutex) != 0) {
			eprint("THREAD: cannot unlock mutex in ALSA thread: %s",
			        strerror(errno));
			pthread_kill(u->t[T_SERIAL], 9);
			stop = true;
		}
	}

	return NULL;
}

static void * serial_worker(void *data)
{
	struct sta_userdata *u = data;

	assert(u);

	pthread_setname_np(pthread_self(), "SERIAL Thread");

	while (!stop) {
		size_t len, err;
		fd_set rfds;
		struct timeval tv;

		FD_ZERO(&rfds);
		FD_SET(u->fd, &rfds);

		tv.tv_sec = 0;
		tv.tv_usec = 5000;

		while ((err = select(FD_SETSIZE, &rfds, NULL, NULL, &tv)) <= 0) {
			if (err == -1) {
				eprint("THREAD: cannot wait for terminal in SERIAL "
				        "thread: %s", strerror(errno));
				stop = true;
				goto cond;
			} else {
				/* check if SIGINT was triggered */
				if (stop)
					goto cond;

				/* reset flags because select updates them */
				FD_ZERO(&rfds);
				FD_SET(u->fd, &rfds);

				tv.tv_sec = 0;
				tv.tv_usec = 5000;
			}
		}

		if (pthread_mutex_lock(&u->mutex) != 0) {
			eprint("THREAD: cannot lock mutex in SERIAL thread: %s",
			        strerror(errno));
			stop = true;
		}

		if (u->buf_count == BUF_COUNT) {
			eprint("SERIAL: Buffer overflow... ignore MIDI messages");
			fflush(stderr);
			/* discards the data in the terminal input queue */
			tcflush(u->fd, TCIFLUSH);
			goto mutex;
		}

		if ((len = read(u->fd, u->buf[u->buf_count], sizeof(*u->buf))) > 0) {
			size_t i;
			printf(COLOR_YELLOW "MIDI <-- ");

			for (i = 0; i < (len - 1); i++) {
				/* STM32 internal protocol */
				if (u->buf[u->buf_count][i] == 0xFA)
					u->buf[u->buf_count][i] = 0x0A;
				printf("%02x ", u->buf[u->buf_count][i]);
			}
			printf("\n" COLOR_RESET);
			fflush(stdout);
			u->buf_count++;

		} else {
			eprint("SERIAL: cannot read from \"%s\": %s",
			        options.serial_port_name, strerror(errno));
			stop = true;
		}

	mutex:
		if (pthread_mutex_unlock(&u->mutex) != 0) {
			eprint("THREAD: cannot unlock mutex in SERIAL thread: %s",
			        strerror(errno));
			pthread_kill(u->t[T_ALSA], 9);
			stop = true;
		}

	cond:
		if (pthread_cond_signal(&u->condition) != 0) {
			eprint("THREAD: cannot signal condition variable in "
			        "SERIAL thread: %s", strerror(errno));
			pthread_kill(u->t[T_ALSA], 9);
			stop = true;
		}
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	static const char short_options[] = "hVm:s:";
	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{"midi-port", required_argument, NULL, 'm'},
		{"serial-port", required_argument, NULL, 's'},
		{ }
	};
	int c, err;
	struct sta_userdata u;
	pthread_mutexattr_t atts;

	u.buf_count = 0;

	while ((c = getopt_long(argc, argv, short_options,
	                        long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage();
			return 0;
		case 'V':
			version();
			return 0;
		case 'm':
			options.midi_port_name = optarg;
			break;
		case 's':
			options.serial_port_name = optarg;
			break;
		default:
			eprint("Try `serial-to-alsa --help' for more information.");
			return 1;
		}
	}

	signal(SIGINT, sig_handler);

	if ((err = alsa_setup(&u.output)) < 0)
		goto end;

	if ((u.fd = serial_setup(options.serial_port_name)) < 0) {
		err = u.fd;
		goto end;
	}

	/* Mutex */
	if ((err = pthread_mutexattr_init(&atts)) != 0) {
		eprint("THREAD: cannot create mutex attribute object: %s",
		       strerror(errno));
		goto end;
	}

	if ((err = pthread_mutexattr_settype(&atts, PTHREAD_MUTEX_RECURSIVE)) != 0) {
		eprint("THREAD: cannot set mutex attribute: %s", strerror(errno));
		pthread_mutexattr_destroy(&atts);
		goto end;
	}

	/* if ((err = pthread_mutexattr_setprotocol(&atts, PTHREAD_PRIO_INHERIT)) != 0) { */
	/* 	eprint("THREAD: cannot set mutex protocol: %s", strerror(errno)); */
	/* 	pthread_mutexattr_destroy(&atts); */
	/* 	goto end; */
	/* } */

	if ((err = pthread_mutex_init(&u.mutex, &atts)) != 0) {
		eprint("THREAD: cannot create mutex: %s", strerror(errno));
		pthread_mutexattr_destroy(&atts);
		goto end;
	}

	if ((err = pthread_mutexattr_destroy(&atts)) != 0) {
		eprint("THREAD: cannot destroy mutex attribute object: %s",
		        strerror(errno));
		goto mutex;
	}

	/* Condition Variable */
	if ((err = pthread_cond_init(&u.condition, NULL)) != 0) {
		eprint("THREAD: cannot create condition variable: %s",
		        strerror(errno));
		goto mutex;
	}

	/* Thread Execution */
	if ((err = pthread_create(&u.t[T_ALSA], NULL, alsa_worker, &u)) != 0) {
		eprint("THREAD: cannot create ALSA thread: %s", strerror(errno));
		goto cond;
	}

	if ((err = pthread_create(&u.t[T_SERIAL], NULL, serial_worker, &u)) != 0) {
		eprint("THREAD: cannot create SERIAL thread: %s", strerror(errno));
		pthread_kill(u.t[T_ALSA], 9);
		goto cond;
	}

	/* Wait for threads */
	if ((err = pthread_join(u.t[T_ALSA], NULL)) != 0) {
		eprint("THREAD: error while waiting for ALSA thread: %s",
		        strerror(errno));
	}

	if ((err = pthread_join(u.t[T_SERIAL], NULL)) != 0) {
		eprint("THREAD: error while waiting for SERIAL thread: %s",
		        strerror(errno));
	}

cond:
	pthread_cond_destroy(&u.condition);

mutex:
	pthread_mutex_destroy(&u.mutex);

end:

	if (u.output)
		snd_rawmidi_close(u.output);

	if (u.fd > 0)
		close(u.fd);

	return err;
}
