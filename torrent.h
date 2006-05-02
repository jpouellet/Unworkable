/* $Id: torrent.h,v 1.4 2006-05-02 00:27:06 niallo Exp $ */
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

#ifndef TORRENT_H
#define TORRENT_H

#include <sys/queue.h>

enum type { MULTIFILE, SINGLEFILE };

struct torrent_path {
	char					*path;
	SLIST_ENTRY(torrent_path)		paths;
};

struct torrent_multi_file {
	SLIST_HEAD(multi_paths, torrent_path)	multi_paths;
	SLIST_ENTRY(torrent_multi_file)		files;
	long					length;
	char					*md5sum;
};

struct torrent {
	union {
		struct {
			long			length;
			char			*name;
			long			piece_length;
			char			*pieces;
		} singlefile;

		struct {
			SLIST_HEAD(multi_files, torrent_multi_file) multi_files;
			char			*name;
			long			piece_length;
			char			*pieces;
		} multifile;
	} body;
	char					*announce;
	time_t					creation_date;
	char					*comment;
	char					*created_by;
	enum type				type;
};

struct torrent		*torrent_parse_file(const char *);
void 			torrent_print(struct torrent *);

/* TORRENT_H */
#endif
