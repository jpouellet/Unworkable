/* $Id: network.c,v 1.49 2007-05-05 01:30:06 niallo Exp $ */
/*
 * Copyright (c) 2006, 2007 Niall O'Higgins <niallo@unworkable.org>
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
#include <sys/queue.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <netdb.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bencode.h"
#include "buf.h"
#include "network.h"
#include "parse.h"
#include "xmalloc.h"

#define PEER_MSG_ID_CHOKE		0
#define PEER_MSG_ID_UNCHOKE		1
#define PEER_MSG_ID_INTERESTED		2
#define PEER_MSG_ID_NOTINTERESTED	3
#define PEER_MSG_ID_HAVE		4
#define PEER_MSG_ID_BITFIELD		5
#define PEER_MSG_ID_REQUEST		6
#define PEER_MSG_ID_PIECE		7
#define PEER_MSG_ID_CANCEL		8

/* bittorrent peer */
struct peer {
	TAILQ_ENTRY(peer) peer_list;
	struct sockaddr_in sa;
	int connfd;
	int handshook;
	size_t rxpending;
	size_t txpending;
	struct bufferevent *bufev;
	u_int32_t txmsglen, rxmsglen;
	u_int8_t *txmsg, *rxmsg;
	/* from peer's handshake message */
	u_int8_t pstrlen;
	u_int8_t id[20];
	u_int8_t info_hash[20];
};

/* data associated with a bittorrent session */
struct session {
	/* don't expect to have huge numbers of peers, or be searching very often, so linked list
	 * should be fine for storage */
	TAILQ_HEAD(peers, peer) peers;
	int connfd;
	char *key;
	char *ip;
	char *numwant;
	char *peerid;
	char *port;
	char *trackerid;
	char *request;
	struct event announce_event;

	struct torrent *tp;
};

static int	network_announce(struct session *, const char *);
static void	network_announce_update(int, short, void *);
static void	network_handle_announce_response(struct bufferevent *, void *);
static void	network_handle_announce_error(struct bufferevent *, short, void *);
static void	network_handle_write(struct bufferevent *, void *);
static int	network_connect(int, int, int, const struct sockaddr *,
		    socklen_t);
static int	network_connect_tracker(const char *, const char *);
static int	network_connect_peer(struct peer *);
static void	network_peerlist_update(struct session *, struct benc_node *);
static void	network_peer_handshake(struct session *, struct peer *);
static void	network_handle_peer_response(struct bufferevent *, void *);
static void	network_handle_peer_write(struct bufferevent *, void *);
static void	network_handle_peer_error(struct bufferevent *, short, void *);

static int
network_announce(struct session *sc, const char *event)
{
	int i, l;
	size_t n;
	char host[MAXHOSTNAMELEN], port[6], path[MAXPATHLEN], *c;
#define GETSTRINGLEN 2048
	char *params, *request;
	char tbuf[3*SHA1_DIGEST_LENGTH+1];
	struct bufferevent *bufev;

	params = xmalloc(GETSTRINGLEN);
	request = xmalloc(GETSTRINGLEN);
	memset(params, '\0', GETSTRINGLEN);
	memset(request, '\0', GETSTRINGLEN);

	/* convert binary info hash to url encoded format */
	for (i = 0; i < SHA1_DIGEST_LENGTH; i++) {
		l = snprintf(&tbuf[3*i], sizeof(tbuf), "%%%02x", sc->tp->info_hash[i]);
		if (l == -1 || l >= (int)sizeof(tbuf))
			goto trunc;
	}
#define HTTPLEN 7
	/* separate out hostname, port and path */
	c = strstr(sc->tp->announce, "http://");
	c += HTTPLEN;
	n = strcspn(c, ":/") + 1;
	if (n > sizeof(host) - 1)
		goto err;

	memcpy(host, c, n - 1);
	host[n - 1] = '\0';

	c += n;
	if (*c != '/') {
		n = strcspn(c, "/") + 1;
		if (n > sizeof(port) - 1)
			goto err;

		memcpy(port, c, n - 1);
		port[n - 1] = '\0';
	} else {
		if (strlcpy(port, "80", sizeof(port)) >= sizeof(port))
			goto trunc;
	}
	c += n - 1;

	if (strlcpy(path, c, sizeof(path)) >= sizeof(path))
		goto trunc;
	/* strip trailing slash */
	if (path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';

	/* build params string */
	l = snprintf(params, GETSTRINGLEN,
	    "?info_hash=%s"
	    "&peer_id=%s"
	    "&port=%s"
	    "&uploaded=%llu"
	    "&downloaded=%llu"
	    "&left=%llu"
	    "&compact=1",
	    tbuf,
	    sc->peerid,
	    sc->port,
	    sc->tp->uploaded,
	    sc->tp->downloaded,
	    sc->tp->left);
	if (l == -1 || l >= GETSTRINGLEN)
		goto trunc;
	/* these parts are optional */
	if (event != NULL) {
		l = snprintf(params, GETSTRINGLEN, "%s&event=%s", params,
		    event);
		if (l == -1 || l >= GETSTRINGLEN)
			goto trunc;
	}
	if (sc->ip != NULL) {
		l = snprintf(params, GETSTRINGLEN, "%s&ip=%s", params,
		    sc->ip);
		if (l == -1 || l >= GETSTRINGLEN)
			goto trunc;
	}
	if (sc->numwant != NULL) {
		l = snprintf(params, GETSTRINGLEN, "%s&numwant=%s", params,
		    sc->numwant);
		if (l == -1 || l >= GETSTRINGLEN)
			goto trunc;
	}
	if (sc->key != NULL) {
		l = snprintf(params, GETSTRINGLEN, "%s&key=%s", params,
		    sc->key);
		if (l == -1 || l >= GETSTRINGLEN)
			goto trunc;
	}
	if (sc->trackerid != NULL) {
		l = snprintf(params, GETSTRINGLEN, "%s&trackerid=%s",
		    params, sc->trackerid);
		if (l == -1 || l >= GETSTRINGLEN)
			goto trunc;
	}

	l = snprintf(request, GETSTRINGLEN,
	    "GET %s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path,
	    params, host);
	if (l == -1 || l >= GETSTRINGLEN)
		goto trunc;

	/* non blocking connect ? */
	if ((sc->connfd = network_connect_tracker(host, port)) == -1)
		exit(1);
	
	sc->request = request;
	bufev = bufferevent_new(sc->connfd, network_handle_announce_response,
	    network_handle_write, network_handle_announce_error, sc);
	bufferevent_enable(bufev, EV_READ);
	bufferevent_write(bufev, request, strlen(request) + 1);
	xfree(params);
	return (0);

trunc:
	warnx("network_announce: string truncation detected");
err:
	xfree(params);
	xfree(request);
	return (-1);
}

static void
network_handle_announce_response(struct bufferevent *bufev, void *arg)
{
#define RESBUFLEN 1024
	BUF *buf;
	u_char *c, *res;
	size_t len;
	struct benc_node *node, *troot;
	struct session *sc;
	struct torrent *tp;
	struct timeval tv;

	printf("network_handle_announce_response\n");
	buf = NULL;
	troot = node = NULL;
	/* XXX need to handle case where full response is not yet buffered */
	res = xmalloc(RESBUFLEN);
	memset(res, '\0', RESBUFLEN);
	len = bufferevent_read(bufev, res, RESBUFLEN);

	sc = arg;
	tp = sc->tp;

	c = res;
	if (strncmp(c, "HTTP/1.0", 8) != 0 && strncmp(c, "HTTP/1.1", 8)) {
		warnx("network_handle_announce_response: not a valid HTTP response");
		goto err;
	}
	c += 9;
	if (strncmp(c, "200", 3) != 0) {
		warnx("network_handle_announce_response: HTTP response indicates error");
		goto err;
	}
	c = strstr(c, "\r\n\r\n");
	if (c == NULL) {
		warnx("network_handle_announce_response: HTTP response had no content");
		goto err;
	}
	c += 4;

	if ((buf = buf_alloc(128, BUF_AUTOEXT)) == NULL) {
		warnx("network_handle_announce_response: could not allocate buffer");
		xfree(res);
		return;
	}
	buf_set(buf, c, len - (c - res), 0);

	troot = benc_root_create();
	if ((troot = benc_parse_buf(buf, troot)) == NULL)
		errx(1,"network_handle_announce_response: HTTP response parsing failed");

	benc_node_print(troot, 0);
	if ((node = benc_node_find(troot, "interval")) == NULL)
		errx(1, "no interval field");

	if (!(node->flags & BINT))
		errx(1, "interval is not a number");

	tp->interval = node->body.number;

	if ((node = benc_node_find(troot, "peers")) == NULL)
		errx(1, "no peers field");
	network_peerlist_update(sc, node);
	benc_node_freeall(troot);

	printf("tracker announce completed, sending next one in %d seconds\n",
	    tp->interval);
	timerclear(&tv);
	tv.tv_sec = tp->interval;
	evtimer_del(&sc->announce_event);
	evtimer_set(&sc->announce_event, network_announce_update, sc);
	evtimer_add(&sc->announce_event, &tv);
err:
	bufferevent_free(bufev);
	buf_free(buf);
	xfree(res);
	(void) close(sc->connfd);
}

static int
network_connect(int domain, int type, int protocol, const struct sockaddr *name, socklen_t namelen)
{
	int sockfd;

	sockfd = socket(domain, type, protocol);
	if (sockfd == -1) {
		warn("network_connect: socket");
		return (-1);
	}
	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1)
		err(1, "network_connect");
	if (connect(sockfd, name, namelen) == -1) {
		warn("network_connect: connect");
		return (-1);
	}

	return (sockfd);

}

static int
network_connect_peer(struct peer *p)
{
	printf("network_connect_peer\n");
	return (network_connect(PF_INET, SOCK_STREAM, 0,
	    (const struct sockaddr *) &p->sa, sizeof(p->sa)));
}

static int
network_connect_tracker(const char *host, const char *port)
{
	struct addrinfo hints, *res, *res0;
	int error, sockfd;

	printf("network_connect_tracker\n");
	memset(&hints, 0, sizeof(hints));
	/* IPv4-only for now */
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error) {
		warnx("network_connect: %s", gai_strerror(error));
		return (-1);
	}
	/* assume first address is ok */
	res = res0;
	sockfd = network_connect(res->ai_family, res->ai_socktype,
	    res->ai_protocol, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res0);

	return (sockfd);
}

static void
network_handle_announce_error(struct bufferevent *bufev, short error, void *data)
{
	struct session *sc = data;

	printf("network error\n");
	if (error & EVBUFFER_TIMEOUT) {
		printf("buffer event timeout");
		bufferevent_free(bufev);
	}
	if (error & EVBUFFER_EOF) {
		printf("EOF on fd %d\n", sc->connfd);
		bufferevent_disable(bufev, EV_READ|EV_WRITE);
		bufferevent_free(bufev);
		(void)close(sc->connfd);
	}
}

static void
network_handle_write(struct bufferevent *bufev, void *data)
{
	struct session *sc = data;

	printf("network_handle_write\n");
	xfree(sc->request);
}

static void
network_announce_update(int fd, short type, void *arg)
{
	struct session *sc = arg;
	struct timeval tv;

	printf("network_announce_update\n");
	network_announce(sc, NULL);
	timerclear(&tv);
	tv.tv_sec = sc->tp->interval;
	evtimer_set(&sc->announce_event, network_announce_update, sc);
	evtimer_add(&sc->announce_event, &tv);
}

/* Yes, this is slow.  But peer lists should not be too long, and we shouldn't be running it
   often at all (once per announce, interval is often thousands of seconds).
   So O(n2) should be acceptable worst case. */
static void
network_peerlist_update(struct session *sc, struct benc_node *peers)
{
	char *peerlist;
	size_t len, i;
	struct peer *p, *ep, *nxt;
	int found = 0;

	/* XXX */
	if (!(peers->flags & BSTRING))
		errx(1, "long peer lists not supported yet");

	len = peers->body.string.len;
	peerlist = peers->body.string.value;
	p = NULL;

	printf("network_peerlist_update\n");
	/* check for peers to add */
	for (i = 0; i < len; i++) {
		if (i % 6 == 0) {
			p = xmalloc(sizeof(*p));
			memset(p, 0, sizeof(*p));
			p->sa.sin_family = AF_INET;
			memcpy(&p->sa.sin_addr, peerlist + i, 4);
			memcpy(&p->sa.sin_port, peerlist + i + 4, 2);
			/* Is this peer already in the list? */
			found = 0;
			TAILQ_FOREACH(ep, &sc->peers, peer_list) {
				/* XXX check for ourselves */
				if (memcmp(&ep->sa.sin_addr, &p->sa.sin_addr, sizeof(ep->sa.sin_addr)) == 0
				    && memcmp(&ep->sa.sin_port, &p->sa.sin_port, sizeof(ep->sa.sin_port)) == 0) {
					found = 1;
					break;
				}
			}
			if (!found)
				TAILQ_INSERT_TAIL(&sc->peers, p, peer_list);
			continue;
		}
	}

	/* check for peers to remove */
	peerlist = peers->body.string.value;
	for (ep = TAILQ_FIRST(&sc->peers); ep != TAILQ_END(&sc->peers); ep = nxt) {
		nxt = TAILQ_NEXT(ep, peer_list);
		for (i = 0; i < len; i++ ) {
			if (i % 6 == 0) {
				p = xmalloc(sizeof(*p));
				memset(p, 0, sizeof(*p));
				memcpy(&p->sa.sin_addr, peerlist + i, 4);
				memcpy(&p->sa.sin_port, peerlist + i + 4, 2);
				/* Is this peer in the new list? */
				found = 0;
				if (memcmp(&ep->sa.sin_addr, &p->sa.sin_addr, sizeof(p->sa.sin_addr)) == 0
				    && memcmp(&ep->sa.sin_port, &p->sa.sin_port, sizeof(p->sa.sin_addr)) == 0) {
					found = 1;
					xfree(p);
					break;
				}
				xfree(p);
			}
		}
		/* if not, remove from list and free memory */
		if (!found) {
			TAILQ_REMOVE(&sc->peers, ep, peer_list);
			if (ep->connfd != 0)
				(void) close(ep->connfd);
			xfree(ep);
		}
	}
	printf("peer list for url %s: \n", sc->tp->announce);
	TAILQ_FOREACH(ep, &sc->peers, peer_list) {
		printf("host=%s, port=%d - ", inet_ntoa(ep->sa.sin_addr),
		    ntohs(ep->sa.sin_port));
		if (ep->connfd != 0) {
			/* XXX */
		} else {
			/* XXX non-blocking connect? */
			ep->connfd = network_connect_peer(ep);
			ep->bufev = bufferevent_new(ep->connfd, network_handle_peer_response,
			    network_handle_peer_write, network_handle_peer_error, ep);
			bufferevent_enable(ep->bufev, EV_READ|EV_WRITE);
			network_peer_handshake(sc, ep);
		}
	}
}
static void
network_peer_handshake(struct session *sc, struct peer *p)
{
	printf("network_peer_handshake for peer on fd %d\n", p->connfd);
	/*
	* handshake: <pstrlen><pstr><reserved><info_hash><peer_id>
	* pstrlen: string length of <pstr>, as a single raw byte
	* pstr: string identifier of the protocol
	* reserved: eight (8) reserved bytes. All current implementations use all zeroes. Each bit in
	* these bytes can be used to change the behavior of the protocol.
	* An email from Bram suggests that trailing bits should be used first, so that leading bits
	* may be used to change the meaning of trailing bits.
	* info_hash: 20-byte SHA1 hash of the info key in the metainfo file. This is the same
	* info_hash that is transmitted in tracker requests.
	* peer_id: 20-byte string used as a unique ID for the client. This is the same peer_id that is
	* transmitted in tracker requests.
	*
	* In version 1.0 of the BitTorrent protocol, pstrlen = 19, and pstr = "BitTorrent protocol".
	*/
	#define HANDSHAKELEN (1 + 19 + 8 + 20 + 20)
	p->txmsg = xmalloc(HANDSHAKELEN);
	memset(p->txmsg, 0, HANDSHAKELEN);
	p->txmsg[0] = 19;
	memcpy(p->txmsg + 1, "BitTorrent protocol", 19);
	memcpy(p->txmsg + 28, sc->tp->info_hash, 20);
	memcpy(p->txmsg + 48, sc->peerid, 20);

	if (bufferevent_write(p->bufev, p->txmsg, HANDSHAKELEN) != 0)
		errx(1, "network_peer_handshake() failure");
}


static void
network_handle_peer_error(struct bufferevent *bufev, short what, void *data)
{
	printf("peer error\n");
}

static void
network_handle_peer_write(struct bufferevent *bufev, void *data)
{
	struct peer *p = data;

	printf("network_peer_write for peer on fd %d\n", p->connfd);
	xfree(p->txmsg);
}

static void
network_handle_peer_response(struct bufferevent *bufev, void *data)
{
	struct peer *p = data;
	/* should always be 19, but just in case... */
	size_t len;
	int i;
	u_int32_t msglen;
	u_int8_t *base, id = 0;

	if (!p->handshook) {
		printf("handshake response\n");
		if (p->rxpending == 0) {
			/* this should be a handshake response, minimum of 1 byte read, which is length
			 * field, so we always know how much data to expect */
			p->rxmsg = xmalloc(1);
			len = bufferevent_read(bufev, p->rxmsg, 1);
			if (len != 1)
				errx(1, "len should be 1 here!");
			memcpy(&p->pstrlen, p->rxmsg, 1);
			if (p->pstrlen != 19)
				errx(1, "pstrlen is %d not 19!", p->pstrlen);
			xfree(p->rxmsg);
			/* now we can allocate full data buffer, and know when we're done reading... */
			p->rxmsglen = p->pstrlen + 8 + 20 + 20;
			p->rxmsg = xmalloc(p->rxmsglen);
			p->rxpending = p->rxmsglen;
			goto read;
		} else {
		read:
			base = p->rxmsg + (p->rxmsglen - p->rxpending);
			len = bufferevent_read(bufev, p->rxmsg, p->rxmsglen);
			if (len < p->rxmsglen) {
				p->rxpending = p->rxmsglen - len;
				return;
			}
			/* if we get this far, means we have got the full handshake */
			memcpy(&p->info_hash, base + 1 + p->pstrlen + 8, 20);
			memcpy(&p->id, base + 1 + p->pstrlen + 8 + 20, 20);

			xfree(p->rxmsg);
			p->rxmsg = NULL;
			printf("parsed incoming handshake\n");
			printf("peer hash:\t0x");
			for (i = 0; i < SHA1_DIGEST_LENGTH; i++)
				printf("%02x", p->info_hash[i]);
			printf("\n");
			p->handshook = 1;
			return;
		}
	} else {
		printf("handshake done, other data\n");
		len = bufferevent_read(bufev, &msglen, 4);
		if (len != 4)
			errx(1, "len should be 4 here!");
		p->rxmsglen = ntohl(msglen);
		printf("message length: %d\n", p->rxmsglen);
		/* keep-alive: do nothing */
		if (p->rxmsglen == 0)
			return;
		p->rxmsg = xmalloc(p->rxmsglen);
		memset(p->rxmsg, 0, p->rxmsglen);
		p->rxpending = p->rxmsglen;
		base = p->rxmsg + (p->rxmsglen - p->rxpending);
		len = bufferevent_read(bufev, p->rxmsg, p->rxmsglen);
		if (len < p->rxmsglen) {
			p->rxpending = p->rxmsglen - len;
			return;
		}
		/* if we get this far, means we have the entire message */
		memcpy(&id, base, 1);
		switch (id) {
			case PEER_MSG_ID_CHOKE:
				printf("peer sez choke\n");
				break;
			case PEER_MSG_ID_UNCHOKE:
				printf("peer sez unchoke\n");
				break;
			case PEER_MSG_ID_INTERESTED:
				printf("peer sez interested\n");
				break;
			case PEER_MSG_ID_NOTINTERESTED:
				printf("peer sez notinterested\n");
				break;
			case PEER_MSG_ID_HAVE:
				printf("peer sez have\n");
				break;
			case PEER_MSG_ID_BITFIELD:
				printf("peer sez bitfield\n");
				break;
			case PEER_MSG_ID_REQUEST:
				printf("peer sez request\n");
				break;
			case PEER_MSG_ID_PIECE:
				printf("peer sez piece\n");
				break;
			case PEER_MSG_ID_CANCEL:
				printf("peer sez cancel\n");
				break;
		}
	}
}

/* network subsystem init, needs to be called before doing anything */
void
network_init()
{
	event_init();
}

/* start handling network stuff for a new torrent */
int
network_start_torrent(struct torrent *tp)
{
	int ret;
	struct session *sc;

	sc = xmalloc(sizeof(*sc));
	memset(sc, 0, sizeof(*sc));

	TAILQ_INIT(&sc->peers);
	sc->tp = tp;
	sc->port = xstrdup("6668");
	sc->peerid = xstrdup("U1234567891234567890");

	ret = network_announce(sc, "started");

	event_dispatch();

	return (ret);
}

