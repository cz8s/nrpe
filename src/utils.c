/*
 * utils.c - NRPE Utility Functions
 *
 * Copyright (c) 1999-2006 Ethan Galstad (nagios@nagios.org)
 * Copyright (c) 2011 Kristian Lyngstol <kristian@bohemians.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

 /*
  * This file contains common network functions used in nrpe and check_nrpe.
  *
  * XXX: Much legacy or redundant. Fixing it as I go. -K
  *
  */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#include "common.h"
#include "utils.h"

static unsigned long crc32_table[256];

/* build the crc table - must be called before calculating the crc value */
void generate_crc32_table(void)
{
	unsigned long crc, poly;
	int i, j;

	poly = 0xEDB88320L;
	for (i = 0; i < 256; i++) {
		crc = i;
		for (j = 8; j > 0; j--) {
			if (crc & 1)
				crc = (crc >> 1) ^ poly;
			else
				crc >>= 1;
		}
		crc32_table[i] = crc;
	}

	return;
}

/* calculates the CRC 32 value for a buffer */
unsigned long calculate_crc32(char *buffer, int buffer_size)
{
	register unsigned long crc;
	int this_char;
	int current_index;

	crc = 0xFFFFFFFF;

	for (current_index = 0; current_index < buffer_size; current_index++) {
		this_char = (int)buffer[current_index];
		crc =
		    ((crc >> 8) & 0x00FFFFFF) ^ crc32_table[(crc ^ this_char) &
							    0xFF];
	}

	return (crc ^ 0xFFFFFFFF);
}

/* fill a buffer with semi-random data */
void randomize_buffer(char *buffer, int buffer_size)
{
	FILE *fp;
	int x;
	int seed;

	/**** FILL BUFFER WITH RANDOM ALPHA-NUMERIC CHARACTERS ****/

	/***************************************************************
	   Only use alpha-numeric characters becase plugins usually
	   only generate numbers and letters in their output.  We
	   want the buffer to contain the same set of characters as
	   plugins, so its harder to distinguish where the real output
	   ends and the rest of the buffer (padded randomly) starts.
	***************************************************************/

	/* try to get seed value from /dev/urandom, as its a better source of entropy */
	fp = fopen("/dev/urandom", "r");
	if (fp != NULL) {
		seed = fgetc(fp);
		fclose(fp);
	}

	/* else fallback to using the current time as the seed */
	else
		seed = (int)time(NULL);

	srand(seed);
	for (x = 0; x < buffer_size; x++)
		buffer[x] = (int)'0' + (int)(72.0 * rand() / (RAND_MAX + 1.0));

	return;
}


/*
 * Convenience functions for exiting with a warning and error respectively.
 * Nagios ignores stderr and interprets return-code 2 as critical and 1 as
 * warning.
 */
void exit_crit(const char *fmt, ...)
{
	va_list ap;
	fprintf(stdout,"CRITICAL: ");
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fprintf(stdout, "\n");
	exit(2);
}

void exit_warn(const char *fmt, ...)
{
	va_list ap;
	fprintf(stdout,"WARNING: ");
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fprintf(stdout, "\n");
	exit(1);
}

void exit_unknown(const char *fmt, ...)
{
	va_list ap;
	fprintf(stdout,"UNKNOWN: ");
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fprintf(stdout, "\n");
	exit(3);
}

/*
 * Open a connection to the hostname and port and store the FD on *sd.
 * Returns 0 on success. Iterates sockets until one works, thus
 * IPv4/IPv6-agnostic.
 */
int my_tcp_connect(char *host_name, int port, int *sd)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	char strport[10];

	s = snprintf(strport,10,"%d",port);
	if (s>10 || s<0)
		exit_crit("Invalid port-number?");
	
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	s = getaddrinfo(host_name, strport, &hints, &result);
	if (s != 0)
		exit_crit("getaddrinfo: %s\n", gai_strerror(s));

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;

		close(sfd);
	}

	if (rp == NULL)
		exit_crit("Could not connect\n");

	freeaddrinfo(result);

	*sd = sfd;

	return 0;
}

/* sends all data - thanks to Beej's Guide to Network Programming */
int sendall(int s, char *buf, int *len)
{
	int total = 0;
	int bytesleft = *len;
	int n = 0;

	/* send all the data */
	while (total < *len) {

		/* send some data */
		n = send(s, buf + total, bytesleft, 0);

		/* break on error */
		if (n == -1)
			break;

		/* apply bytes we sent */
		total += n;
		bytesleft -= n;
	}

	/* return number of bytes actually send here */
	*len = total;

	/* return -1 on failure, 0 on success */
	return n == -1 ? -1 : 0;
}

/*
 * receives all data - modelled after sendall()
 *
 * FIXME: Should use SO_RCVTIMEOUT if applicable.
 * The sleep(1)-way is inherently slow and wasteful. If we're going to
 * wait, we might as well block.
 */
int recvall(int s, char *buf, int *len, int timeout)
{
	int total = 0;
	int bytesleft = *len;
	int n = 0;
	time_t start_time;
	time_t current_time;

	/* clear the receive buffer */
	bzero(buf, *len);

	time(&start_time);

	/* receive all data */
	while (total < *len) {

		/* receive some data */
		n = recv(s, buf + total, bytesleft, 0);

		/* no data has arrived yet (non-blocking socket) */
		if (n == -1 && errno == EAGAIN) {
			time(&current_time);
			if (current_time - start_time > timeout)
				break;
			sleep(1);
			continue;
		}

		/* receive error or client disconnect */
		else if (n <= 0)
			break;

		/* apply bytes we received */
		total += n;
		bytesleft -= n;
	}

	/* return number of bytes actually received here */
	*len = total;

	/* return <=0 on failure, bytes received on success */
	return (n <= 0) ? n : total;
}

/* fixes compiler problems under Solaris, since strsep() isn't included */
/* this code is taken from the glibc source */
char *my_strsep(char **stringp, const char *delim)
{
	char *begin, *end;

	begin = *stringp;
	if (begin == NULL)
		return NULL;

	/* A frequent case is when the delimiter string contains only one
	   character.  Here we don't need to call the expensive `strpbrk'
	   function and instead work using `strchr'.  */
	if (delim[0] == '\0' || delim[1] == '\0') {
		char ch = delim[0];

		if (ch == '\0')
			end = NULL;
		else {
			if (*begin == ch)
				end = begin;
			else
				end = strchr(begin + 1, ch);
		}
	}

	else
		/* Find the end of the token.  */
		end = strpbrk(begin, delim);

	if (end) {

		/* Terminate the token and set *STRINGP past NUL character.  */
		*end++ = '\0';
		*stringp = end;
	} else
		/* No more delimiters; this is the last token.  */
		*stringp = NULL;

	return begin;
}

/* show license */
void display_license(void)
{

	printf
	    ("This program is released under the GPL (see below) with the additional\n");
	printf
	    ("exemption that compiling, linking, and/or using OpenSSL is allowed.\n\n");

	printf
	    ("This program is free software; you can redistribute it and/or modify\n");
	printf
	    ("it under the terms of the GNU General Public License as published by\n");
	printf
	    ("the Free Software Foundation; either version 2 of the License, or\n");
	printf("(at your option) any later version.\n\n");
	printf
	    ("This program is distributed in the hope that it will be useful,\n");
	printf
	    ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
	printf
	    ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n");
	printf("GNU General Public License for more details.\n\n");
	printf
	    ("You should have received a copy of the GNU General Public License\n");
	printf("along with this program; if not, write to the Free Software\n");
	printf("Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.\n\n");

	return;
}
