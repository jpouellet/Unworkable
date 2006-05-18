/* $Id: main.c,v 1.12 2006-05-18 15:50:18 niallo Exp $ */
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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bencode.h"
#include "torrent.h"

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
	int ch, fd, i, j, k, iflag, badflag, hint;
	struct torrent *torrent;
	struct torrent_piece *tpp;
	char *p;

	badflag = 0;

	root = benc_node_create();
	root->flags = BLIST;

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
	torrent_data_open(torrent);

	for (i = 0; i < torrent->num_pieces; i++) {
		torrent_piece_map(torrent, i);
		tpp = torrent_piece_find(torrent, i);
		if (tpp == NULL)
			printf("could not find piece: %d\n", i);
		j = torrent_piece_checkhash(torrent, tpp);
		if (j != 0) {
			errx(1, "hash mismatch for piece: %d\n", i);
			badflag = 1;
		}
        /* lazy unmapping */
        if (i % 8 == 0 && i > 0) {
            for (k = 0; k < 8; k++) {
		        torrent_piece_unmap(torrent, i - k);
            }
        }
	}
	if (badflag == 0)
		printf("torrent matches hash\n");

	exit(0);

}
