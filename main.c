/* $Id: main.c,v 1.25 2007-05-08 20:36:04 niallo Exp $ */
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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(USE_BOEHM_GC)
#include <gc.h>
#endif

#include "includes.h"

void usage(void);

extern char *optarg;
extern int  optind;

void
usage(void)
{
	fprintf(stderr, "unworkable: [-i] file\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int ch, j, iflag, badflag;
	size_t i;
	struct torrent *torrent;
	struct torrent_piece *tpp;

	#if defined(USE_BOEHM_GC)
	GC_INIT();
	#endif
	badflag = 0;

	while ((ch = getopt(argc, argv, "i")) != -1) {
		switch (ch) {
		case 'i':
			iflag = 1;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc == 0)
		usage();

	torrent = torrent_parse_file(argv[0]);
	torrent_print(torrent);
	for (i = 0; i < torrent->num_pieces; i++) {
		torrent_piece_map(torrent, i);
		tpp = torrent_piece_find(torrent, i);
		j = torrent_piece_checkhash(torrent, tpp);
		if (j != 0) {
			warnx("hash mismatch for piece: %zd\n", i);
			badflag = 1;
			tpp->flags |= TORRENT_PIECE_CKSUMOK;
		}
	}
	if (badflag == 0)
		printf("torrent matches hash\n");


	network_init();
	network_start_torrent(torrent);

	exit(0);
}
