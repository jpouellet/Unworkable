/* $Id: scheduler.c,v 1.12 2008-10-03 19:09:46 niallo Exp $ */
/*
 * Copyright (c) 2006, 2007, 2008 Niall O'Higgins <niallo@p2presearch.com>
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/tree.h>

#include <string.h>
/* solaris 10 */
#if defined(__SVR4) && defined(__sun)
#include <utility.h>
#endif

#include "includes.h"

static int	 scheduler_piece_assigned(struct session *, struct torrent_piece *);
static u_int32_t scheduler_piece_find_rarest(struct peer *, int, int *);
static int	 scheduler_is_endgame(struct session *);
static int	 scheduler_threshold_kill(struct peer *);
static void	 scheduler_dequeue_uploads(struct peer *);
static int	 scheduler_reap_dead(struct session *, struct peer *);
static void	 scheduler_fill_requests(struct session *, struct peer *);
static void	 scheduler_choke_algorithm(struct session *, time_t *);
static void	 scheduler_endgame_algorithm(struct session *);

/*
 * scheduler_is_endgame()
 *
 * Are we in the end game?
 * Returns 1 if true, zero if false.
 */
static int
scheduler_is_endgame(struct session *sc)
{
	struct torrent_piece *tpp;
	u_int32_t i;
	int not_queued = 0;

	for (i = 0; i < sc->tp->num_pieces; i++) {
		if ((tpp = torrent_piece_find(sc->tp, i)) == NULL)
			errx(1, "scheduler_is_endgame(): torrent_piece_find");
		if (!(tpp->flags & TORRENT_PIECE_CKSUMOK)) {
			/* are all the blocks in the queue? */
			if (!scheduler_piece_assigned(sc, tpp)) {
				not_queued = 1;
				break;
			}
		}
	}

	return (!not_queued);
}

/*
 * scheduler_piece_assigned()
 *
 * Are all this piece's blocks in the download queue and assigned to a peer?
 * Returns 1 on success, 0 on failure
 */
static int
scheduler_piece_assigned(struct session *sc, struct torrent_piece *tpp)
{
	u_int32_t off;
	struct piece_dl *pd;

	/* if this piece and all its blocks are already in our download queue,
	 * skip it */
	for (off = 0; ; off += BLOCK_SIZE) {
		if (off >= tpp->len)
			return (1);
		pd = network_piece_dl_find(sc, NULL, tpp->index, off);

		/* if a piece doesn't exist, or has been orphaned, then its not
		 * done */
		if (pd == NULL || (pd->bytes != pd->len && pd->pc == NULL))
			return (0);
	}
}

/*
 * scheduler_piece_cmp()
 *
 * Used by qsort()
 */
static int
scheduler_piece_cmp(const void *a, const void *b)
{
	const struct piececounter *x, *y;

	x = a;
	y = b;

	return (x->count - y->count);

}

/*
 * scheduler_peer_cmp()
 *
 * Used by qsort()
 */
static int
scheduler_peer_cmp(const void *a, const void *b)
{
	const struct peercounter *x, *y;

	x = a;
	y = b;

	return (y->rate - x->rate);

}

/*
 * scheduler_peer_speedrank()
 *
 * For a given session return an array of peers sorted by their download speeds.
 */
static struct peercounter *
scheduler_peer_speedrank(struct session *sc)
{
	struct peer *p;
	struct peercounter *peers;
	u_int32_t i = 0;

	peers = xcalloc(sc->num_peers, sizeof(*peers));
	TAILQ_FOREACH(p, &sc->peers, peer_list) {
		peers[i].peer = p;
		if (p->state & PEER_STATE_INTERESTED) {
			peers[i].rate = network_peer_rate(p);
			/* kind of a hack so we don't unchoke
			 * un-interested peers */
			if (peers[i].rate == 0)
				peers[i].rate = 1;
		}
		i++;
	}
	if (i != sc->num_peers)
		errx(1, "scheduler_peer_speedrank: peer number mismatch (i: %u num_peers: %u)", i, sc->num_peers);

	qsort(peers, sc->num_peers, sizeof(*peers), scheduler_peer_cmp);

	return (peers);
}

/*
 * scheduler_piece_rarityarray()
 *
 * For a given session return sorted array of piece counts.
 */
static void
scheduler_piece_rarityarray(struct session *sc)
{
	struct piececounter *pieces;
	struct peer *p;
	u_int32_t i, count, pos, len;

	pos = 0;
	len = sc->tp->num_pieces;
	pieces = xcalloc(len, sizeof(*pieces));

	/* counts for each piece */
	for (i = 0; i < len; i++) {
		count = 0;
		/* otherwise count it */
		TAILQ_FOREACH(p, &sc->peers, peer_list) {
			if (!(p->state & PEER_STATE_ESTABLISHED))
				continue;
			if (p->bitfield != NULL
			    && util_getbit(p->bitfield, i))
				count++;
		}
		if (pos > len)
			errx(1, "scheduler_piece_rarityarray: pos is %u should be %u\n", pos,
			    (sc->tp->num_pieces - sc->tp->good_pieces - 1));

		pieces[pos].count = count;
		pieces[pos].idx = i;
		pos++;
	}
	/* sort the rarity array */
	qsort(pieces, len, sizeof(*pieces), scheduler_piece_cmp);

	/* set the session timestamp */
	sc->last_rarity = time(NULL);
	if (sc->rarity_array != NULL)
		xfree(sc->rarity_array);
	sc->rarity_array = pieces;
}

#define FIND_RAREST_IGNORE_ASSIGNED	0
#define FIND_RAREST_ABSOLUTE		1
/*
 * scheduler_piece_find_rarest()
 *
 * Find the rarest piece, allowing for certain conditions.
 */
static u_int32_t
scheduler_piece_find_rarest(struct peer *p, int flag, int *res)
{
	struct torrent_piece *tpp;
	struct piececounter *pieces;
	u_int32_t i;
	int found = 0;

	tpp = NULL;
	*res = 1;

#define RARITY_AGE			5
	if (time(NULL) - p->sc->last_rarity > RARITY_AGE)
		scheduler_piece_rarityarray(p->sc);
	pieces = p->sc->rarity_array;
	/* find the rarest piece amongst our peers */
	for (i = 0; i < p->sc->tp->num_pieces; i++) {
		/* if this peer doesn't have this piece, skip it */
		if (p->bitfield != NULL
		    && !util_getbit(p->bitfield, i))
			continue;
		tpp = torrent_piece_find(p->sc->tp, pieces[i].idx);
		/* if we have this piece, skip it */
		if (tpp->flags & TORRENT_PIECE_CKSUMOK) {
			continue;
		}
		if (flag == FIND_RAREST_IGNORE_ASSIGNED) {
			/* if this piece and all its blocks are already
			 * assigned to a peer and worked on skip it */
			if (scheduler_piece_assigned(p->sc, tpp)) {
				continue;
			} else {
				found = 1;
				break;
			}
		} else {
			found = 1;
			break;
		}
	}

	*res = found;
	if (tpp == NULL)
		return (0);
	return (tpp->index);
}

/*
 * scheduler_threshold_kill()
 *
 * If we have not received data in PEER_COMMS_THRESHOLD,
 * remove the block requests from our list and kill the peer.
 */
static int
scheduler_threshold_kill(struct peer *p)
{
	if ((p->state & PEER_STATE_BITFIELD || p->state & PEER_STATE_ESTABLISHED)
	    && network_peer_lastcomms(p) >= PEER_COMMS_THRESHOLD) {
		trace("comms threshold exceeded for peer %s:%d",
		    inet_ntoa(p->sa.sin_addr), ntohs(p->sa.sin_port));
		p->state = 0;
		p->state |= PEER_STATE_DEAD;
		return (0);
	}

	return (-1);
}

/*
 * scheduler_dequeue_uploads()
 *
 * Dequeue a piece to peer, if there is one waiting.
 */
static void
scheduler_dequeue_uploads(struct peer *p)
{
	struct piece_ul *pu;
	/* honour one upload */
	if ((pu = network_piece_ul_dequeue(p)) != NULL) {
		trace("dequeuing piece to peer %s:%d",
		    inet_ntoa(p->sa.sin_addr), ntohs(p->sa.sin_port));
		network_peer_write_piece(p, pu->idx, pu->off, pu->len);
		xfree(pu);
		pu = NULL;
	}
}

/*
 * scheduler_reap_dead()
 *
 * Reap peer if it is marked dead.
 */
static int
scheduler_reap_dead(struct session *sc, struct peer *p)
{
	/* if peer is marked dead, free it */
	if (p->state & PEER_STATE_DEAD) {
		TAILQ_REMOVE(&sc->peers, p, peer_list);
		network_peer_free(p);
		sc->num_peers--;
		return (0);
	}
	return (-1);
}

/*
 * scheduler_fill_requests()
 *
 * If peer is not choked, make sure it has enough requests in its queue.
 */
static void
scheduler_fill_requests(struct session *sc, struct peer *p)
{
	struct piece_dl *pd;
	u_int64_t peer_rate;
	u_int32_t pieces_left, queue_len, i;
	int hint = 0;

	pieces_left = sc->tp->num_pieces - sc->tp->good_pieces;

	if (!(p->state & PEER_STATE_CHOKED)
	    && pieces_left > 0) {
		peer_rate = network_peer_rate(p);
		/* for each 10k/sec on this peer, add a request. */
		/* minimum queue length is 2, max is MAX_REQUESTS */
		queue_len = (u_int32_t) peer_rate / 10240;
		if (queue_len < 2) {
			queue_len = 2;
		} else if (queue_len > MAX_REQUESTS) {
			queue_len = MAX_REQUESTS;
		}
		/* test for overflow */
		if (queue_len < p->dl_queue_len) {
			queue_len = 0;
		} else {
			/* queue_len is what the peer's queue length should be */
			queue_len -= p->dl_queue_len;
		}

		for (i = 0; i < queue_len; i++) {
			pd = scheduler_piece_gimme(p, 0, &hint);
			/* probably means no bitfield from this peer yet, or all requests are in transit. give it some time. */
			if (pd == NULL)
				continue;
			network_peer_request_block(pd->pc, pd->idx, pd->off, pd->len);
			p->dl_queue_len++;
		}
	}
}

/*
 * scheduler_choke_algorithm()
 *
 * Every 10 seconds, sort peers by speed and unchoke the 3 fastest.
 */
static void
scheduler_choke_algorithm(struct session *sc, time_t *now)
{
	struct peer *p, *p2;
	struct peercounter *pc;
	u_int32_t i;
	int j, k, num_interested;

	p = p2 = NULL;

	if ((*now % 10) != 0)
		return;
	pc = scheduler_peer_speedrank(sc);
	for (i = 0; i < MIN(3, sc->num_peers); i++) {
		/* if this peer is already unchoked, leave it */
		if (!(pc[i].peer->state & PEER_STATE_AMCHOKING))
			continue;
		/* don't unchoke peers who have not expressed interest */
		if (!(pc[i].peer->state & PEER_STATE_INTERESTED))
			continue;
		/* now we can unchoke this one */
		trace("fastest unchoke");
		network_peer_write_unchoke(pc[i].peer);
	}
	if ((*now % 30) == 0 ) {
		num_interested = 0;
		TAILQ_FOREACH(p2, &sc->peers, peer_list) {
			if (p2->state & PEER_STATE_INTERESTED)
				num_interested++;
		}
		if (num_interested > 0) {
			j = random() % num_interested;
			p2 = TAILQ_FIRST(&sc->peers);
			for (k = 0; k < j; k++) {
				if (p2 == NULL)
					errx(1, "NULL peer");
				if (!(pc->peer->state & PEER_STATE_INTERESTED)) {
					p2 = TAILQ_NEXT(p2, peer_list);
					continue;
				}
			}
			trace("opportunistic unchoke");
			network_peer_write_unchoke(p2);
		}
	}
	/* choke any peers except for three fastest, and the one randomly selected */
	TAILQ_FOREACH(p, &sc->peers, peer_list) {
		int c = 0;
		/* don't try to choke any of the peers
		 * we just unchoked above */
		for (i = 0; i < MIN(3, sc->num_peers); i++) {
			if (p == pc[i].peer || p == p2) {
				c = 1;
				break;
			}
		}
		if (c)
			continue;
		if (!(p->state & PEER_STATE_AMCHOKING))
			network_peer_write_choke(p);
	}
	xfree(pc);
}

/*
 * scheduler_endgame_algorithm()
 *
 * Endgame handling.
 */
static void
scheduler_endgame_algorithm(struct session *sc)
{
	struct torrent_piece *tpp;
	struct peer *p;
	struct piece_dl *pd;
	u_int32_t i, off, len;
	int hint = 0;

	/* find incomplete pieces */
	for (i = 0; i < sc->tp->num_pieces; i++) {
		if ((tpp = torrent_piece_find(sc->tp, i)) == NULL)
			errx(1, "scheduler(): torrent_piece_find");
		if (tpp->flags & TORRENT_PIECE_CKSUMOK)
			continue;
		/* which peers have it? */
		trace("we still need piece idx %u", i);
		TAILQ_FOREACH(p, &sc->peers, peer_list) {
			if (p->bitfield == NULL
			    || util_getbit(p->bitfield, i))
				continue;
			if (p->state & PEER_STATE_CHOKED) {
				trace("    (choked) peer %s:%d has it",
				    inet_ntoa(p->sa.sin_addr), ntohs(p->sa.sin_port));
				continue;
			}
			trace("    (unchoked) peer %s:%d has it",
			    inet_ntoa(p->sa.sin_addr), ntohs(p->sa.sin_port));
			hint = 0;
			/* find the un-queued pieces */
			for (off = 0; off < tpp->len; off += BLOCK_SIZE) {
				int found = 0;
				/* is this block offset already queued on this peer? */
				TAILQ_FOREACH(pd, &p->peer_piece_dls, peer_piece_dl_list) {
					if (pd->idx == i && pd->off == off) {
						found = 1;
						break;
					}
				}
				if (found)
					continue;
				if (BLOCK_SIZE > tpp->len - off) {
					len = tpp->len - off;
				} else {
					len = BLOCK_SIZE;
				}
				pd = network_piece_dl_create(p, i, off, len);
				trace("choosing endgame dl (tpp->len %u) len %u idx %u off %u", tpp->len, len, i, off);
				network_peer_request_block(pd->pc, pd->idx, pd->off, pd->len);
				p->dl_queue_len++;
			}
		}
	}
}

/*
 * scheduler_piece_gimme()
 *
 * According to various selection strategies, hand me something to download.
 */
struct piece_dl *
scheduler_piece_gimme(struct peer *peer, int flags, int *hint)
{
	struct torrent_piece *tpp;
	struct piece_dl *pd;
	struct piece_dl_idxnode *pdin;
	u_int32_t i, j, idx, len, off, *pieces, peerpieces;
	int res;

	res = 0;
	idx = off = 0;
	tpp = NULL;

	/* if we have some blocks in a piece, try to complete that same piece */
	RB_FOREACH(pdin, piece_dl_by_idxoff, &peer->sc->piece_dl_by_idxoff) {
		tpp = torrent_piece_find(peer->sc->tp, pdin->idx);
		if (!(tpp->flags & TORRENT_PIECE_CKSUMOK)) {
			/* if not all this piece's blocks are in the download
			 * queue and this peer actually has this piece */
			if (!scheduler_piece_assigned(peer->sc, tpp)
			    && peer->bitfield != NULL
			    && util_getbit(peer->bitfield, pdin->idx)) {
				idx = pdin->idx;
				goto get_block;
			}
		}
	}
	/* first 4 pieces should be chosen randomly */
	if (peer->sc->tp->good_pieces < 4 && peer->sc->tp->num_pieces > 4) {
		/* check how many useful pieces this peer has */
		peerpieces = 0;
		for (i = 0; i < peer->sc->tp->num_pieces; i++) {
			if (peer->bitfield != NULL
			    && util_getbit(peer->bitfield, i)) {
				tpp = torrent_piece_find(peer->sc->tp, i);
				/* do we already have this piece? */
				if (tpp->flags & TORRENT_PIECE_CKSUMOK)
					continue;
				/* is it already assigned? */
				if (scheduler_piece_assigned(peer->sc, tpp))
					continue;
				peerpieces++;
			}
		}
		/* peer has no pieces */
		if (peerpieces == 0)
			return (NULL);
		/* build array of pieces this peer has */
		pieces = xcalloc(peerpieces, sizeof(*pieces));
		j = 0;
		for (i = 0; i < peer->sc->tp->num_pieces; i++) {
			if (peer->bitfield != NULL
			    && util_getbit(peer->bitfield, i)) {
				tpp = torrent_piece_find(peer->sc->tp, i);
				/* do we already have this piece? */
				if (tpp->flags & TORRENT_PIECE_CKSUMOK)
					continue;
				/* is it already assigned? */
				if (scheduler_piece_assigned(peer->sc, tpp))
					continue;
				pieces[j] = i;
				j++;
			}
		}
		/* select piece randomly */
		idx = pieces[random() % peerpieces];
		xfree(pieces);
		tpp = torrent_piece_find(peer->sc->tp, idx);
	} else {
		/* find the rarest piece that does not have all its blocks
		 * already in the download queue */
		idx = scheduler_piece_find_rarest(peer, FIND_RAREST_IGNORE_ASSIGNED, &res);
		/* there are no more pieces right now */
		if (!res)
			return (NULL);
		tpp = torrent_piece_find(peer->sc->tp, idx);
	}
get_block:
	if (flags & PIECE_GIMME_NOCREATE) {
		*hint = 1;
		return (NULL);
	}
	/* find the next block (by offset) in the piece, which is not already
	 * assigned to a peer */
	for (off = 0; ; off += BLOCK_SIZE) {
		if (off >= tpp->len)
			errx(1, "gone to a bad offset %u in idx %u, len %u", off, idx, tpp->len);
		pd = network_piece_dl_find(peer->sc, NULL, idx, off);
		/* no piece dl at all */
		if (pd == NULL) {
			break;
		} else if (pd->pc == NULL && pd->bytes != pd->len) {
			/* piece dl exists, but it has been orphaned -> recycle
			 * it */
			trace("recycling dl (tpp->len %u) len %u idx %u off %u", tpp->len, pd->len, pd->idx, pd->off);
			pd->pc = peer;
			/* put it in this peer's list */
			TAILQ_INSERT_TAIL(&peer->peer_piece_dls, pd, peer_piece_dl_list);
			return (pd);
		}
	}
	if (BLOCK_SIZE > tpp->len - off) {
		len = tpp->len - off;
	} else {
		len = BLOCK_SIZE;
	}
	pd = network_piece_dl_create(peer, idx, off, len);

	trace("choosing next dl (tpp->len %u) len %u (tpp->idx %u) idx %u off %u", tpp->len, len, tpp->index, idx, off);
	return (pd);
}

/*
 * scheduler()
 *
 * Bulk of decision making happens here.  Runs every second, once announce is complete.
 */
void
scheduler(int fd, short type, void *arg)
{
	struct peer *p, *p2, *nxt;
	struct session *sc = arg;
	struct timeval tv;
	/* piece rarity array */
	struct piece_dl *pd;
	struct piece_dl_idxnode *pdin;
	struct peercounter *pc;
	u_int32_t pieces_left, reqs_outstanding, reqs_completed, reqs_orphaned;
	u_int32_t choked, unchoked;
	char tbuf[64];
	time_t now;

	reqs_outstanding = reqs_completed = reqs_orphaned = choked = unchoked = 0;
	p = p2 = NULL;
	pc = NULL;
	pd = NULL;
	timerclear(&tv);
	tv.tv_sec = 1;
	evtimer_set(&sc->scheduler_event, scheduler, sc);
	evtimer_add(&sc->scheduler_event, &tv);

	pieces_left = sc->tp->num_pieces - sc->tp->good_pieces;
	if (!TAILQ_EMPTY(&sc->peers)) {
		for (p = TAILQ_FIRST(&sc->peers); p; p = nxt) {
			nxt = TAILQ_NEXT(p, peer_list);
			if (p->state & PEER_STATE_CHOKED) {
				choked++;
			} else {
				unchoked++;
			}
			if (scheduler_reap_dead(sc, p) == 0)
				continue;
			if (scheduler_threshold_kill(p) == 0)
				continue;
			scheduler_dequeue_uploads(p);
			scheduler_fill_requests(sc, p);
		}
	}
	now = time(NULL);

	scheduler_choke_algorithm(sc, &now);

	if (scheduler_is_endgame(sc))
		scheduler_endgame_algorithm(sc);

	/* try to get some more peers */
	if (sc->num_peers < PEERS_WANTED
	    && pieces_left > 0
	    && !sc->announce_underway
	    && (now - sc->last_announce) > MIN_ANNOUNCE_INTERVAL) {
		/* XXX: But what if the tracker really only has a small number of peers?
		 * We will keep asking over and over, wasting resources.
		 * This should be fixed */
		announce(sc, NULL);
	}
	/* print some trace info, if trace is enabled */
	if (unworkable_trace == NULL)
		return;
	RB_FOREACH(pdin, piece_dl_by_idxoff, &sc->piece_dl_by_idxoff) {
		pd = TAILQ_FIRST(&pdin->idxnode_piece_dls);
		if (pd->pc == NULL) {
			reqs_orphaned++;
			strlcpy(tbuf, " [orphaned] ", sizeof(tbuf));
		} else if (pd->pc->connfd != 0) {
			snprintf(tbuf, sizeof(tbuf), "assigned to: %s:%d",
			    inet_ntoa(pd->pc->sa.sin_addr), ntohs(pd->pc->sa.sin_port));
		}
		if (pd->bytes != pd->len) {
			reqs_outstanding++;
		} else {
			reqs_completed++;
			strlcat(tbuf, " [done] ", sizeof(tbuf));
		}
		if ((now % 60) == 0) {
			trace("piece_dl: idx %u off: %u len: %u %s", pd->idx, pd->off, pd->len, tbuf);
		}
	}
	trace("Peers: %u (c %u/u %u) Good pieces: %u/%u Reqs outstanding/orphaned/completed: %u/%u/%u",
	      sc->num_peers, choked, unchoked, sc->tp->good_pieces, sc->tp->num_pieces,
	      reqs_outstanding, reqs_orphaned, reqs_completed);
}

