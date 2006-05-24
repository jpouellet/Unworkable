/* $Id: network.c,v 1.4 2006-05-24 00:58:09 niallo Exp $ */
/*
 * Copyright (c) 2006 Niall O'Higgins <niallo@unworkable.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <err.h>
#include <event.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network.h"

int
network_announce(const char *url, const char *infohash, const char *peerid,
    const char *myport, const char *uploaded, const char *downloaded,
    const char *left, const char *compact, const char *event, const char *ip,
    const char *numwant, const char *key, const char *trackerid)
{
	int connfd;
	size_t n;
	char host[MAXHOSTNAMELEN], port[6], path[MAXPATHLEN], *c;
	char request[1024];
	FILE *conn;

#define HTTPLEN 7
	c = strstr(url, "http://");
	c += HTTPLEN;
	n = strcspn(c, ":/") + 1;
	if (n > sizeof(host) - 1)
		return (-1);

	strlcpy(host, c, n);
	printf("hostname: %s\n", host);

	c += n;
	if (*c != '/') {
		n = strcspn(c, "/") + 1;
		if (n > sizeof(port) - 1)
			return (-1);

		strlcpy(port, c, n);
	} else {
		strlcpy(port, "80", sizeof(port));
	}
	printf("port: %s\n", port);
	c += n - 1;

	strlcpy(path, c, sizeof(path));
	/* strip trailing slash */
	if (path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';

	printf("path: %s\n", path);

	if ((connfd = network_connect(host, port)) == -1)
		exit(1);
	
	if ((conn = fdopen(connfd, "r+")) == NULL)
		err(1, "network_announce");

	/* build request string */
	if (strlcpy(request, "?info_hash=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, infohash, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?peer_id=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, peerid, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?port=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, myport, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?uploaded=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, uploaded, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?downloaded=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, downloaded, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?left=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, left, sizeof(request)) >= sizeof(request)
	    || strlcat(request, "?compact=", sizeof(request)) >= sizeof(request)
	    || strlcat(request, compact, sizeof(request)) >= sizeof(request))
		errx(1, "network_announce: string truncation detected");
	/* these parts are optional */
	if (event != NULL) {
		if (strlcat(request, "?event=", sizeof(request)) >= sizeof(request)
		    || strlcat(request, event, sizeof(request)) >= sizeof(request))
			errx(1, "network_announce: string truncation detected");
	}
	if (ip != NULL) {
		if (strlcat(request, "?ip=", sizeof(request)) >= sizeof(request)
		    || strlcat(request, ip, sizeof(request)) >= sizeof(request))
			errx(1, "network_announce: string truncation detected");
	}
	if (numwant != NULL) {
		if (strlcat(request, "?numwant=", sizeof(request)) >= sizeof(request)
		    || strlcat(request, numwant, sizeof(request)) >= sizeof(request))
			errx(1, "network_announce: string truncation detected");
	}
	if (numwant != NULL) {
		if (strlcat(request, "?key=", sizeof(request)) >= sizeof(request)
		    || strlcat(request, key, sizeof(request)) >= sizeof(request))
			errx(1, "network_announce: string truncation detected");
	}
	if (trackerid != NULL) {
		if (strlcat(request, "?trackerid=", sizeof(request)) >= sizeof(request)
		    || strlcat(request, trackerid, sizeof(request)) >= sizeof(request))
			errx(1, "network_announce: string truncation detected");
	}
	fprintf(conn, "GET %s%s HTTP/1.1\r\nHost: %s\r\n\r\n", path, request,
	    host);

	return (0);
}

int
network_connect(const char *host, const char *port)
{
	struct addrinfo hints, *res, *res0;
	int error, sockfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error) {
		warnx("%s", gai_strerror(error));
		return (-1);
	}
	/* assume first address is ok */
	res = res0;
	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd == -1) {
		warn("network_tracker_announce: socket");
		return (-1);
	}
	
	if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
		warn("network_tracker_announce: connect");
		return (-1);
	}

	freeaddrinfo(res0);

	return (sockfd);
}

void
network_loop()
{

	event_init();


}
