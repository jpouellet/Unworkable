/* $Id: parse.y,v 1.58 2008-09-09 03:12:20 niallo Exp $ */
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


/*
 * Parser for BitTorrent `bencode' format.
 * See http://wiki.theory.org/BitTorrentSpecification
 */

%{

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "includes.h"

#ifndef LLONG_MAX
 #define LLONG_MAX 9223372036854775807LL
#endif
#ifndef LLONG_MIN
 #define LLONG_MIN (-LLONG_MAX - 1LL)
#endif

/* Assume no more than 16 nested dictionaries/lists. */
#define  BENC_STACK_SIZE	16

/* Initial size of lexer buffer. */
#define  BENC_BUFFER_SIZE	128
/* How much to grow lexer buffer by. */
#define  BENC_BUFFER_INCREMENT	20480

/* Internal node-stack functions */
static struct benc_node		*benc_stack_pop(void);
static struct benc_node		*benc_stack_peek(void);
static void			benc_stack_push(struct benc_node *);

static long			bstrlen  = 0;
static int			bstrflag = 0;
static int			bdone    = 0;
static int			bcdone   = 0;

static struct benc_node		*bstack[BENC_STACK_SIZE];
static int			bstackidx = 0;
BUF				*in     = NULL;

%}

%union {
	long long		number;
	char			*string;
	struct benc_node	*benc_node;
	size_t			len;
}

%token				COLON
%token <len>			END
%token				INT_START
%token				DICT_START
%token <benc_node>		LIST_START
%token <string>			STRING
%type  <benc_node>		bstring
%type  <benc_node>		bint
%type  <number>			number
%type  <benc_node>		bdict_entries
%type  <benc_node>		bdict_entry
%type  <benc_node>		blist_entries
%type  <benc_node>		bdict
%type  <benc_node>		blist

%start bencode

%%


bencode		: /* empty */
		| bencode bstring				{
			benc_node_add(root, $2);
		}
		| bencode bint					{
			benc_node_add(root, $2);
		}
		| bencode bdict					{
			benc_node_add(root, $2);
		}
		| bencode blist					{
			benc_node_add(root, $2);
		}
		;

number		: STRING					{
			long long lval;
			const char *errstr;
			lval = strtonum($1, LLONG_MIN, LLONG_MAX, &errstr);
			if (errstr) {
				yyerror("%s is %s", $1, errstr);
				xfree($1);
				YYERROR;
			} else {
				$$ = lval;
				if (bstrflag == 1)
					bstrlen = lval;
			}
			xfree($1);
		}
		;

/*
 * Special hack for bstrings.
 */
bstrflag	:						{
			bstrflag = 1;
		}
		;

bstring		: bstrflag number COLON STRING			{
			struct benc_node *node;

			node = benc_node_create();
			node->body.string.len = $2;
			node->body.string.value = $4;
			node->flags = BSTRING;
			$$ = node;
		}
		;

bint		: INT_START number END				{
			struct benc_node *node;

			node = benc_node_create();
			node->body.number = $2;
			node->flags = BINT;

			$$ = node;
		}
		;

blist		: LIST_START					{
			/*
			 * Push the list node onto the stack before continuing
			 * so that sub-elements can add themselves to it.
			 */
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BLIST;
			benc_stack_push(node);
		}
		blist_entries END				{
			/*
			 * Pop list node and link the remaining sub-element.
			 */
			struct benc_node *node;

			node = benc_stack_pop();
			benc_node_add_head(node, $3);
			$$ = node;
		}
		;

blist_entries	: bint						{
			$$ = $1;
		}
		| bstring					{
			$$ = $1;
		}
		| blist						{
			$$ = $1;
		}
		| bdict						{
			$$ = $1;
		}
		| blist_entries bint				{
			benc_node_add(benc_stack_peek(), $2);
		}
		| blist_entries bstring				{
			benc_node_add(benc_stack_peek(), $2);
		}
		| blist_entries blist				{
			benc_node_add(benc_stack_peek(), $2);
		}
		| blist_entries bdict				{
			benc_node_add(benc_stack_peek(), $2);
		}
		;

bdict_entry	: bstring bint					{
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BINT|BDICT_ENTRY;
			node->body.dict_entry.key = $1->body.string.value;
			node->body.dict_entry.value = $2;

			xfree($1);
			$$ = node;
		}
		| bstring bstring				{
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BSTRING|BDICT_ENTRY;
			node->body.dict_entry.key = $1->body.string.value;
			node->body.dict_entry.value = $2;

			xfree($1);
			$$ = node;
		}
		| bstring blist					{
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BLIST|BDICT_ENTRY;
			node->body.dict_entry.key = $1->body.string.value;
			node->body.dict_entry.value = $2;

			xfree($1);
			$$ = node;
		}
		| bstring bdict					{
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BDICT|BDICT_ENTRY;
			node->body.dict_entry.key = $1->body.string.value;
			node->body.dict_entry.value = $2;

			xfree($1);
			$$ = node;
		}


bdict_entries	: bdict_entry					{
			$$ = $1;
		}
		| bdict_entries bdict_entry			{
			benc_node_add(benc_stack_peek(), $2);
		}
		;

bdict		: DICT_START					{
			/*
			 * Push the dict node onto the stack before continuing
			 * so that sub-elements can add themselves to it.
			 */
			struct benc_node *node;

			node = benc_node_create();
			node->flags = BDICT;

			benc_stack_push(node);
		}
		bdict_entries END				{
			/*
			 * Pop dict node and link the remaining sub-element.
			 */
			struct benc_node *node;

			node = benc_stack_pop();
			node->end = $4;
			benc_node_add_head(node, $3);

			$$ = node;
		}
		;
%%

int
yylex(void)
{
	char	*buf, *p;
	int	c;
	long	buflen = BENC_BUFFER_SIZE, i = 0;

	buf = xmalloc(buflen);

	p = buf;
	memset(buf, '\0', buflen);

	for (;;) {
		if (i == buflen) {
			ptrdiff_t p_offset = p - buf;
			buflen += BENC_BUFFER_INCREMENT;
			trace("yylex() realloc");
			buf = xrealloc(buf, buflen);
			trace("yylex() realloc done");
			/* ensure pointers are not invalidated after realloc */
			p = buf + p_offset;
			/* NUL-fill the new memory */
			memset(p, '\0', BENC_BUFFER_INCREMENT);
		}

		if (bstrlen == 0 && bstrflag == 1 && bcdone == 1) {
			yylval.string = buf;
			bstrlen = bstrflag = bcdone = 0;
			return (STRING);
		}

		if ((c = buf_getc(in)) == EOF) {
			xfree(buf);
			return (0);
		}

		/* if we are in string context, ignore special chars */
		if ((c == ':' && bdone == 0 && bcdone == 1)
		    || (c != ':' && bstrflag == 1))
			goto skip;

		switch (c) {
		case ':':
			if (bdone == 0 && i > 0) {
				yylval.string = buf;
				bdone = 1;
				bcdone = 0;
				(void)buf_ungetc(in);
				return (STRING);
			} else {
				bdone = 0;
				bcdone = 1;
				xfree(buf);
				return (COLON);
			}
		case 'e':
			/* in other contexts, e is END */
			if (bdone == 0 && i > 0) {
				yylval.string = buf;
				bdone = 1;
				(void)buf_ungetc(in);
				return (STRING);
			} else {
				bdone = 0;
				yylval.len = buf_pos(in);
				xfree(buf);
				return (END);
			}
		case 'i':
			xfree(buf);
			return (INT_START);
		case 'd':
			xfree(buf);
			return (DICT_START);
		case 'l':
			xfree(buf);
			return (LIST_START);
		}
skip:
		/* add this character to the buffer */
		*p = c;
		i++;

		if (i == bstrlen && bstrflag == 1) {
			yylval.string = buf;
			bstrlen = bstrflag = bcdone = 0;
			return (STRING);
		}

		p++;
	}
}

int
yyerror(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);

	return (0);
}

struct benc_node*
benc_parse_buf(BUF *b, struct benc_node *node)
{
	root = node;
	in = b;
	if (yyparse() != 0)
		return (NULL);

	return (root);
}

static struct benc_node*
benc_stack_pop(void)
{
	struct benc_node *node;

	bstackidx--;
	node = bstack[bstackidx];

	return (node);
}

static struct benc_node*
benc_stack_peek(void)
{
	struct benc_node *node;

	node = bstack[bstackidx - 1];

	return (node);
}

static void
benc_stack_push(struct benc_node *node)
{
	if (bstackidx == BENC_STACK_SIZE - 1)
		errx(1, "benc_stack_push: stack overflow");
	bstack[bstackidx] = node;
	bstackidx++;
}
