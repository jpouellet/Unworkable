#
# Copyright (c) 2006, 2007, 2008 Niall O'Higgins <niallo@p2presearch.com>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# $Id: BSDmakefile,v 1.7 2008-09-08 17:46:30 niallo Exp $

CC?= cc
CFLAGS+= -Wall
CFLAGS+= -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+= -Wmissing-declarations
CFLAGS+= -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+= -Wsign-compare -g -ggdb

#
# Uncomment if you like to use Boehm's garbage collector (/usr/ports/devel/boehm-gc).
#LDFLAGS+=                -L/usr/local/lib -lgc -lpthread
#DPADD+=                /usr/local/lib/libgc.a
#CFLAGS+=               -DUSE_BOEHM_GC -DGC_DEBUG -DFIND_LEAK -I/usr/local/include/gc
# You can also use Boehm's garbage collector as a means to find leaks.
#  # export GC_FIND_LEAK=1

PROG= unworkable

SRCS= announce.c bencode.c buf.c ctl_server.c main.c network.c parse.y progressmeter.c scheduler.c torrent.c trace.c util.c xmalloc.c
OBJS= ${SRCS:N*.h:N*.sh:R:S/$/.o/g}
MAN= unworkable.1

all: ${PROG} unworkable.cat1

${PROG}: ${OBJS}
	${CC} -o ${.TARGET} ${LDFLAGS} -levent -lcrypto ${OBJS}

unworkable.cat1: ${MAN}
	nroff -Tascii -mandoc $(MAN) > unworkable.cat1

clean:
	rm -rf *.o ${PROG} y.tab.h unworkable.cat1
