/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2005 Timo Hirvonen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "expr.h"
#include "glob.h"
#include "uchar.h"
#include "track_info.h"
#include "comment.h"
#include "xmalloc.h"
#include "utils.h"
#include "debug.h"
#include "list.h"
#include "ui_curses.h" /* using_utf8, charset */
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>

enum token_type
{
	/* special chars */
	TOK_NOT,
	TOK_LT,
	TOK_GT,

#define NR_COMBINATIONS TOK_EQ

	/* special chars */
	TOK_EQ,
	TOK_AND,
	TOK_OR,
	TOK_LPAREN,
	TOK_RPAREN,

#define NR_SPECIALS TOK_NE
#define COMB_BASE TOK_NE

	/* same as the first 3 + '=' */
	TOK_NE,
	TOK_LE,
	TOK_GE,

	TOK_KEY,
	TOK_INT_OR_KEY,
	TOK_STR
};
#define NR_TOKS (TOK_STR + 1)

struct token
{
	struct list_head node;
	enum token_type type;
	/* for TOK_KEY, TOK_INT_OR_KEY and TOK_STR */
	char str[];
};

/* same order as TOK_* */
static const char specials[NR_SPECIALS] = "!<>=&|()";

static const int tok_to_op[NR_TOKS] = {
	-1, OP_LT, OP_GT, OP_EQ, -1, -1, -1, -1, OP_NE, OP_LE, OP_GE, -1, -1, -1};

static const char *const op_names[NR_OPS] = {"<", "<=", "=", ">=", ">", "!="};
static const char *const expr_names[NR_EXPRS] = {
	"&", "|", "!", "a string", "an integer", "a boolean"};

static char error_buf[64] = {
	0,
};

// 调试文件处理函数
static FILE *expr_debug_fp = NULL;

static void expr_debug_init(void)
{
	if (!expr_debug_fp)
	{
		expr_debug_fp = fopen("/tmp/cmus_expr_debug.log", "w");
		if (expr_debug_fp)
		{
			fprintf(expr_debug_fp, "===== CMUS EXPR DEBUG LOG STARTED =====\n");
			fflush(expr_debug_fp);
		}
	}
}

static void expr_debug_log(const char *format, ...)
{
	if (!expr_debug_fp)
	{
		expr_debug_init();
		if (!expr_debug_fp)
			return;
	}

	va_list ap;
	va_start(ap, format);
	vfprintf(expr_debug_fp, format, ap);
	va_end(ap);
	fflush(expr_debug_fp);
}

static void expr_debug_close(void)
{
	if (expr_debug_fp)
	{
		fprintf(expr_debug_fp, "===== CMUS EXPR DEBUG LOG CLOSED =====\n");
		fclose(expr_debug_fp);
		expr_debug_fp = NULL;
	}
}

static void set_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(error_buf, sizeof(error_buf), format, ap);
	va_end(ap);
}

static struct token *get_str(const char *str, int *idxp)
{
	struct token *tok;
	int s = *idxp + 1;
	int e = s;

	expr_debug_log("DEBUG: get_str at position %d\n", *idxp);

	// 安全检查：确保字符串不为NULL且索引有效
	if (!str || str[*idxp] == '\0' || str[*idxp] != '"')
	{
		expr_debug_log("DEBUG: invalid string or not starting with quote\n");
		set_error("invalid string or not starting with quote");
		return NULL;
	}

	/* can't remove all backslashes here => don't remove any */
	while (str[e] != '\0' && str[e] != '"')
	{
		int c = str[e];

		if (c == 0)
		{
			expr_debug_log("DEBUG: get_str error - unexpected end of string\n");
			goto err;
		}
		if (c == '\\')
		{
			expr_debug_log("DEBUG: get_str found escape character at position %d\n", e);
			if (str[e + 1] == 0)
			{
				expr_debug_log("DEBUG: get_str error - unexpected end after escape\n");
				goto err;
			}
			e += 2;
			continue;
		}
		e++;
	}

	if (str[e] == '\0')
	{
		expr_debug_log("DEBUG: get_str error - string not terminated\n");
		goto err;
	}

	tok = xmalloc(sizeof(struct token) + e - s + 1);
	memcpy(tok->str, str + s, e - s);
	tok->str[e - s] = 0;
	tok->type = TOK_STR;
	*idxp = e + 1;
	expr_debug_log("DEBUG: get_str successful, string is '%s'\n", tok->str);
	return tok;
err:
	set_error("end of expression at middle of string");
	return NULL;
}

static struct token *get_int_or_key(const char *str, int *idxp)
{
	int idx = *idxp;
	int start = idx;
	unsigned int type = TOK_KEY;
	struct token *tok;

	expr_debug_log("DEBUG: get_int_or_key at position %d\n", idx);

	// 安全检查：确保字符串不为NULL且idx在有效范围内
	if (!str || str[idx] == '\0')
	{
		expr_debug_log("DEBUG: invalid string or position\n");
		set_error("invalid string or position");
		return NULL;
	}

	/* integer */
	if (isdigit((unsigned char)str[idx]))
	{
		expr_debug_log("DEBUG: first character is digit, might be INT_OR_KEY\n");
		type = TOK_INT_OR_KEY;
		while (str[idx] != '\0' && isdigit((unsigned char)str[idx]))
			idx++;
	}

	/* key (can start with a digit, but can't be all digits) */
	if (str[idx] != '\0' && (isalpha((unsigned char)str[idx]) || str[idx] == '_' || str[idx] == ':' || str[idx] == '.' || str[idx] == '/'))
	{
		expr_debug_log("DEBUG: found key character: '%c'\n", str[idx]);
		do
		{
			idx++;
		} while (str[idx] != '\0' && (isalnum((unsigned char)str[idx]) || str[idx] == '_' || str[idx] == ':' || str[idx] == '.' || str[idx] == '/'));

		if (str[idx] != '\0' && str[idx] == '-')
		{
			/* '-' is allowed only in the middle of the key */
			idx++;
			if (str[idx] != '\0' && (isalnum((unsigned char)str[idx]) || str[idx] == '_' || str[idx] == ':' || str[idx] == '.' || str[idx] == '/' || str[idx] == '-'))
			{
				expr_debug_log("DEBUG: found hyphen followed by key character\n");
				do
				{
					idx++;
				} while (str[idx] != '\0' && (isalnum((unsigned char)str[idx]) || str[idx] == '_' || str[idx] == ':' || str[idx] == '.' || str[idx] == '/' || str[idx] == '-'));
			}
			else
			{
				expr_debug_log("DEBUG: found trailing hyphen (invalid)\n");
				idx--;
			}
		}
	}
	else if (type == TOK_INT_OR_KEY)
	{
		expr_debug_log("DEBUG: number only, will be returned as INT_OR_KEY\n");
		*idxp = idx;
		tok = xmalloc(sizeof(struct token) + idx - start + 1);
		tok->type = type;
		memcpy(tok->str, str + start, idx - start);
		tok->str[idx - start] = 0;
		return tok;
	}
	else if (str[idx] != '\0' && str[idx] == '-')
	{
		expr_debug_log("DEBUG: found starting hyphen (invalid as key start)\n");
		// Not a valid key or int
		set_error("unexpected character '-'");
		return NULL;
	}
	else
	{
		expr_debug_log("DEBUG: not a valid key or int starting with: '%c'\n", str[idx] != '\0' ? str[idx] : ' ');
		// Not a valid key or int
		if (str[idx] != '\0')
		{
			set_error("unexpected character '%c'", str[idx]);
		}
		else
		{
			set_error("unexpected end of string");
		}
		return NULL;
	}

	*idxp = idx;
	tok = xmalloc(sizeof(struct token) + idx - start + 1);
	tok->type = type;
	memcpy(tok->str, str + start, idx - start);
	tok->str[idx - start] = 0;

	expr_debug_log("DEBUG: get_int_or_key returning: '%s'\n", tok->str);
	return tok;
}

static struct token *get_token(const char *str, int *idxp)
{
	int idx = *idxp;
	int c, i;

	expr_debug_log("DEBUG: get_token at position %d\n", idx);

	// 安全检查：确保字符串不为NULL且索引有效
	if (!str || str[idx] == '\0')
	{
		expr_debug_log("DEBUG: get_token - invalid string or position\n");
		set_error("invalid string or position");
		return NULL;
	}

	c = str[idx];
	expr_debug_log("DEBUG: character at position %d is '%c'\n", idx, c);

	for (i = 0; i < NR_SPECIALS; i++)
	{
		struct token *tok;

		if (c != specials[i])
			continue;

		expr_debug_log("DEBUG: found special character '%c'\n", c);
		idx++;
		tok = xnew(struct token, 1);
		if (!tok)
		{
			expr_debug_log("DEBUG: memory allocation failed\n");
			set_error("memory allocation failed");
			return NULL;
		}
		tok->type = i;
		if (i < NR_COMBINATIONS && str[idx] != '\0' && str[idx] == '=')
		{
			expr_debug_log("DEBUG: found combination token with '='\n");
			tok->type = COMB_BASE + i;
			idx++;
		}
		*idxp = idx;
		expr_debug_log("DEBUG: returning token of type %d\n", tok->type);
		return tok;
	}

	if (c == '"')
	{
		expr_debug_log("DEBUG: found string start '\"'\n");
		struct token *tok = get_str(str, idxp);
		if (tok)
			expr_debug_log("DEBUG: returning string token: %s\n", tok->str);
		else
			expr_debug_log("DEBUG: get_str failed\n");
		return tok;
	}

	expr_debug_log("DEBUG: attempting to get int or key\n");
	struct token *tok = get_int_or_key(str, idxp);
	if (tok)
		expr_debug_log("DEBUG: returning int_or_key token of type %d: %s\n", tok->type, tok->str);
	else
		expr_debug_log("DEBUG: get_int_or_key failed\n");
	return tok;
}

static void free_tokens(struct list_head *head)
{
	struct list_head *item = head->next;

	while (item != head)
	{
		struct list_head *next = item->next;
		struct token *tok = container_of(item, struct token, node);

		free(tok);
		item = next;
	}
}

static int tokenize(struct list_head *head, const char *str)
{
	struct token *tok;
	int idx = 0;

	expr_debug_log("DEBUG: tokenize begin with str = %s\n", str ? str : "NULL");

	/* head is already allocated, just initialize it */
	head->next = head;
	head->prev = head;

	while (1)
	{
		while (isspace((unsigned char)str[idx]))
			++idx;
		if (str[idx] == 0)
		{
			expr_debug_log("DEBUG: tokenize end (end of string)\n");
			return 0;
		}

		tok = get_token(str, &idx);
		if (tok == NULL)
		{
			expr_debug_log("DEBUG: tokenize failed (get_token returned NULL)\n");
			free_tokens(head);
			return -1;
		}
		list_add_tail(&tok->node, head);
		expr_debug_log("DEBUG: tokenize added token of type %d\n", tok->type);
	}
}

static struct expr *expr_new(int type)
{
	struct expr *new = xnew0(struct expr, 1);

	new->type = type;

	return new;
}

static int parse(struct expr **rootp, struct list_head *head, struct list_head **itemp, int level);

static int parse_one(struct expr **exprp, struct list_head *head, struct list_head **itemp)
{
	struct list_head *item = *itemp;
	struct token *tok;
	enum token_type type;
	int rc;

	*exprp = NULL;
	if (item == head)
	{
		set_error("expression expected");
		return -1;
	}

	tok = container_of(item, struct token, node);
	type = tok->type;
	if (type == TOK_NOT)
	{
		struct expr *new, *tmp;

		*itemp = item->next;
		rc = parse_one(&tmp, head, itemp);
		if (rc)
			return rc;
		new = expr_new(EXPR_NOT);
		new->left = tmp;
		*exprp = new;
		return 0;
	}
	else if (type == TOK_LPAREN)
	{
		*itemp = item->next;
		*exprp = NULL;
		return parse(exprp, head, itemp, 1);
		/* ')' already eaten */
	}
	else if (type == TOK_KEY || type == TOK_INT_OR_KEY)
	{
		const char *key = tok->str;
		struct expr *new;
		int op = -1;

		item = item->next;
		if (item != head)
		{
			tok = container_of(item, struct token, node);
			op = tok_to_op[tok->type];
		}
		if (item == head || op == -1)
		{
			/* must be a bool */
			new = expr_new(EXPR_BOOL);
			new->key = xstrdup(key);
			*itemp = item;
			*exprp = new;
			return 0;
		}
		item = item->next;
		if (item == head)
		{
			set_error("right side of expression expected");
			return -1;
		}
		tok = container_of(item, struct token, node);
		type = tok->type;
		*itemp = item->next;
		if (type == TOK_STR)
		{
			if (op != OP_EQ && op != OP_NE)
			{
				set_error("invalid string operator '%s'", op_names[op]);
				return -1;
			}
			new = expr_new(EXPR_STR);
			new->key = xstrdup(key);
			glob_compile(&new->estr.glob_head, tok->str);
			new->estr.op = op;
			*exprp = new;
			return 0;
		}
		else if (type == TOK_INT_OR_KEY)
		{
			long int val = 0;

			if (str_to_int(tok->str, &val))
			{
			}
			new = expr_new(EXPR_INT);
			new->key = xstrdup(key);
			new->eint.val = val;
			new->eint.op = op;
			*exprp = new;
			return 0;
		}
		else if (type == TOK_KEY)
		{
			new = expr_new(EXPR_ID);
			new->key = xstrdup(key);
			new->eid.key = xstrdup(tok->str);
			new->eid.op = op;
			*exprp = new;
			return 0;
		}
		if (op == OP_EQ || op == OP_NE)
		{
			set_error("integer or string expected");
		}
		else
		{
			set_error("integer expected");
		}
		return -1;
	}
	set_error("key expected");
	return -1;
}

static void add(struct expr **rootp, struct expr *expr)
{
	struct expr *tmp, *root = *rootp;

	if (root == NULL)
	{
		*rootp = expr;
		return;
	}

	tmp = root;
	while (tmp->right)
		tmp = tmp->right;
	if (tmp->type <= EXPR_OR)
	{
		/* tmp is binary, tree is incomplete */
		tmp->right = expr;
		expr->parent = tmp;
		return;
	}

	/* tmp is unary, tree is complete
	 * expr must be a binary operator */
	BUG_ON(expr->type > EXPR_OR);

	expr->left = root;
	root->parent = expr;
	*rootp = expr;
}

static int parse(struct expr **rootp, struct list_head *head, struct list_head **itemp, int level)
{
	struct list_head *item = *itemp;

	while (1)
	{
		struct token *tok;
		struct expr *expr;
		int rc, type;

		rc = parse_one(&expr, head, &item);
		if (rc)
			return rc;
		add(rootp, expr);
		if (item == head)
		{
			if (level > 0)
			{
				set_error("')' expected");
				return -1;
			}
			*itemp = item;
			return 0;
		}
		tok = container_of(item, struct token, node);
		if (tok->type == TOK_RPAREN)
		{
			if (level == 0)
			{
				set_error("unexpected ')'");
				return -1;
			}
			*itemp = item->next;
			return 0;
		}

		if (tok->type == TOK_AND)
		{
			type = EXPR_AND;
		}
		else if (tok->type == TOK_OR)
		{
			type = EXPR_OR;
		}
		else
		{
			set_error("'&' or '|' expected");
			return -1;
		}
		expr = expr_new(type);
		add(rootp, expr);
		item = item->next;
	}
}

static const struct
{
	char short_key;
	const char *long_key;
} map_short2long[] = {
	{'A', "albumartist"},
	{'D', "discnumber"},
	{
		'T',
		"tag",
	},
	{'a', "artist"},
	{'c', "comment"},
	{'d', "duration"},
	{'f', "filename"},
	{'g', "genre"},
	{'l', "album"},
	{'n', "tracknumber"},
	{'X', "play_count"},
	{'s', "stream"},
	{'t', "title"},
	{'y', "date"},
	{'\0', NULL},
};

static const struct
{
	const char *key;
	enum expr_type type;
} builtin[] = {
	{"album", EXPR_STR},
	{"albumartist", EXPR_STR},
	{"artist", EXPR_STR},
	{"bitrate", EXPR_INT},
	{"bpm", EXPR_INT},
	{"codec", EXPR_STR},
	{"codec_profile", EXPR_STR},
	{"comment", EXPR_STR},
	{"date", EXPR_INT},
	{"discnumber", EXPR_INT},
	{"duration", EXPR_INT},
	{"filename", EXPR_STR},
	{"genre", EXPR_STR},
	{"media", EXPR_STR},
	{"originaldate", EXPR_INT},
	{"play_count", EXPR_INT},
	{"stream", EXPR_BOOL},
	{"tag", EXPR_BOOL},
	{"title", EXPR_STR},
	{"tracknumber", EXPR_INT},
	{NULL, -1},
};

static const char *lookup_long_key(char c)
{
	int i;
	for (i = 0; map_short2long[i].short_key; i++)
	{
		if (map_short2long[i].short_key == c)
			return map_short2long[i].long_key;
	}
	return NULL;
}

static enum expr_type lookup_key_type(const char *key)
{
	int i;
	for (i = 0; builtin[i].key; i++)
	{
		int cmp = strcmp(key, builtin[i].key);
		if (cmp == 0)
			return builtin[i].type;
		if (cmp < 0)
			break;
	}
	return -1;
}

static unsigned long stack4_new(void)
{
	return 0;
}
static void stack4_push(unsigned long *s, unsigned long e)
{
	*s = (*s << 4) | e;
}
static void stack4_pop(unsigned long *s)
{
	*s = *s >> 4;
}
static unsigned long stack4_top(unsigned long s)
{
	return s & 0xf;
}
static void stack4_replace_top(unsigned long *s, unsigned long e)
{
	*s = (*s & ~0xf) | e;
}

static char *expand_short_expr(const char *expr_short)
{
	/* state space, can contain maximal 15 states */
	enum state_type
	{
		ST_SKIP_SPACE = 1,
		ST_TOP,
		ST_EXPECT_KEY,
		ST_EXPECT_OP,
		ST_EXPECT_INT,
		ST_IN_INT,
		ST_MEM_INT,
		ST_IN_2ND_INT,
		ST_EXPECT_STR,
		ST_IN_QUOTE_STR,
		ST_IN_STR,
	};

	size_t len_expr_short = strlen(expr_short);
	/* worst case blowup of expr_short is 31/5 (e.g. ~n1-2), so take x7:
	 * strlen("~n1-2") == 5
	 * strlen("(tracknumber>=1&tracknumber<=2)") == 31
	 */
	char *out = xnew(char, len_expr_short * 7);
	char *num = NULL;
	size_t i, i_num = 0, k = 0;
	const char *key = NULL;
	int level = 0;
	enum expr_type etype;
	/* used as state-stack, can contain at least 32/4 = 8 states */
	unsigned long state_stack = stack4_new();
	stack4_push(&state_stack, ST_TOP);
	stack4_push(&state_stack, ST_SKIP_SPACE);

	/* include terminal '\0' to recognize end of string */
	for (i = 0; i <= len_expr_short; i++)
	{
		unsigned char c = expr_short[i];
		switch (stack4_top(state_stack))
		{
		case ST_SKIP_SPACE:
			if (c != ' ')
			{
				stack4_pop(&state_stack);
				i--;
			}
			break;
		case ST_TOP:
			switch (c)
			{
			case '~':
				stack4_push(&state_stack, ST_EXPECT_OP);
				stack4_push(&state_stack, ST_SKIP_SPACE);
				stack4_push(&state_stack, ST_EXPECT_KEY);
				break;
			case '(':
				level++;
			/* Fall through */
			case '!':
			case '|':
				out[k++] = c;
				stack4_push(&state_stack, ST_SKIP_SPACE);
				break;
			case ')':
				level--;
				out[k++] = c;
				stack4_push(&state_stack, ST_EXPECT_OP);
				stack4_push(&state_stack, ST_SKIP_SPACE);
				break;
			case '\0':
				if (level > 0)
				{
					set_error("')' expected");
					goto error_exit;
				}
				out[k++] = c;
				break;
			default:
				set_error("unexpected '%c'", c);
				goto error_exit;
			}
			break;
		case ST_EXPECT_KEY:
			stack4_pop(&state_stack);
			key = lookup_long_key(c);
			if (!key)
			{
				set_error("unknown short key %c", c);
				goto error_exit;
			}
			etype = lookup_key_type(key);
			if (etype == EXPR_INT)
			{
				stack4_push(&state_stack, ST_EXPECT_INT);
				out[k++] = '(';
			}
			else if (etype == EXPR_STR)
			{
				stack4_push(&state_stack, ST_EXPECT_STR);
			}
			else if (etype != EXPR_BOOL)
			{
				BUG("wrong etype: %d\n", etype);
			}
			strcpy(out + k, key);
			k += strlen(key);
			stack4_push(&state_stack, ST_SKIP_SPACE);
			break;
		case ST_EXPECT_OP:
			if (c == '~' || c == '(' || c == '!')
				out[k++] = '&';
			i--;
			stack4_replace_top(&state_stack, ST_SKIP_SPACE);
			break;
		case ST_EXPECT_INT:
			if (c == '<' || c == '>')
			{
				out[k++] = c;
				stack4_replace_top(&state_stack, ST_IN_INT);
			}
			else if (c == '-')
			{
				out[k++] = '<';
				out[k++] = '=';
				stack4_replace_top(&state_stack, ST_IN_INT);
			}
			else if (isdigit(c))
			{
				if (!num)
					num = xnew(char, len_expr_short);
				num[i_num++] = c;
				stack4_replace_top(&state_stack, ST_MEM_INT);
			}
			else
			{
				set_error("integer expected", expr_short);
				goto error_exit;
			}
			break;
		case ST_IN_INT:
			if (isdigit(c))
			{
				out[k++] = c;
			}
			else
			{
				i -= 1;
				stack4_pop(&state_stack);
				out[k++] = ')';
			}
			break;
		case ST_MEM_INT:
			if (isdigit(c))
			{
				num[i_num++] = c;
			}
			else
			{
				if (c == '-')
				{
					out[k++] = '>';
					out[k++] = '=';
					stack4_replace_top(&state_stack, ST_IN_2ND_INT);
				}
				else
				{
					out[k++] = '=';
					i--;
					stack4_pop(&state_stack);
				}
				strncpy(out + k, num, i_num);
				k += i_num;
				i_num = 0;
				if (c != '-')
					out[k++] = ')';
			}
			break;
		case ST_IN_2ND_INT:
			if (isdigit(c))
			{
				num[i_num++] = c;
			}
			else
			{
				i--;
				stack4_pop(&state_stack);
				if (i_num > 0)
				{
					out[k++] = '&';
					strcpy(out + k, key);
					k += strlen(key);
					out[k++] = '<';
					out[k++] = '=';
					strncpy(out + k, num, i_num);
					k += i_num;
				}
				out[k++] = ')';
			}
			break;
		case ST_EXPECT_STR:
			out[k++] = '=';
			if (c == '"')
			{
				stack4_replace_top(&state_stack, ST_IN_QUOTE_STR);
				out[k++] = c;
			}
			else
			{
				stack4_replace_top(&state_stack, ST_IN_STR);
				out[k++] = '"';
				out[k++] = '*';
				out[k++] = c;
			}
			break;
		case ST_IN_QUOTE_STR:
			if (c == '"' && expr_short[i - 1] != '\\')
			{
				stack4_pop(&state_stack);
			}
			out[k++] = c;
			break;
		case ST_IN_STR:
			/* isalnum() doesn't work for multi-byte characters */
			if (c != '~' && c != '!' && c != '|' &&
				c != '(' && c != ')' && c != '\0')
			{
				out[k++] = c;
			}
			else
			{
				while (k > 0 && out[k - 1] == ' ')
					k--;
				out[k++] = '*';
				out[k++] = '"';
				i--;
				stack4_pop(&state_stack);
			}
			break;
		default:
			BUG("state %ld not covered", stack4_top(state_stack));
			break;
		}
	}

	if (num)
		free(num);

	d_print("expanded \"%s\" to \"%s\"\n", expr_short, out);

	return out;

error_exit:
	if (num)
		free(num);
	free(out);
	return NULL;
}

int expr_is_short(const char *str)
{
	int i;
	for (i = 0; str[i]; i++)
	{
		if (str[i] == '~')
			return 1;
		if (str[i] != '!' && str[i] != '(' && str[i] != ' ')
			return 0;
	}
	return 0;
}

struct expr *expr_parse(const char *str)
{
	expr_debug_log("DEBUG: expr_parse called with str = %s\n", str ? str : "NULL");
	struct expr *result = expr_parse_i(str, "filter contains control characters", 1);
	expr_debug_log("DEBUG: expr_parse returning %p\n", result);
	return result;
}

struct expr *expr_parse_i(const char *str, const char *err_msg, int check_short)
{
	LIST_HEAD(head);
	struct expr *root = NULL;
	struct list_head *item;
	char *long_str = NULL, *u_str = NULL;
	int i;

	expr_debug_log("DEBUG: expr_parse_i begin with str = %s\n", str ? str : "NULL");

	for (i = 0; str[i]; i++)
	{
		unsigned char c = str[i];
		if (c < 0x20)
		{
			expr_debug_log("DEBUG: control character found at position %d\n", i);
			set_error(err_msg);
			goto out;
		}
	}

	expr_debug_log("DEBUG: passed control character check\n");

	if (!using_utf8 && utf8_encode(str, charset, &u_str) == 0)
	{
		expr_debug_log("DEBUG: utf8_encode successful\n");
		str = u_str;
	}

	if (!u_is_valid(str))
	{
		expr_debug_log("DEBUG: invalid UTF-8\n");
		set_error("invalid UTF-8");
		goto out;
	}

	expr_debug_log("DEBUG: passed UTF-8 validity check\n");

	if (check_short && expr_is_short(str))
	{
		expr_debug_log("DEBUG: expanding short expression\n");
		str = long_str = expand_short_expr(str);
		if (!str)
		{
			expr_debug_log("DEBUG: expand_short_expr failed\n");
			goto out;
		}
		expr_debug_log("DEBUG: expanded to: %s\n", str);
	}

	expr_debug_log("DEBUG: tokenizing\n");
	if (tokenize(&head, str))
	{
		expr_debug_log("DEBUG: tokenize failed\n");
		goto out;
	}
	expr_debug_log("DEBUG: tokenize successful\n");

	item = head.next;
	expr_debug_log("DEBUG: parsing\n");
	if (parse(&root, &head, &item, 0))
	{
		expr_debug_log("DEBUG: parse failed\n");
		root = NULL;
	}
	else
	{
		expr_debug_log("DEBUG: parse successful\n");
	}
	free_tokens(&head);

out:
	free(u_str);
	free(long_str);
	expr_debug_log("DEBUG: expr_parse_i returning %p\n", root);
	return root;
}

int expr_check_leaves(struct expr **exprp, const char *(*get_filter)(const char *name))
{
	struct expr *expr = *exprp;
	struct expr *e;
	const char *filter;
	int i, rc;

	if (expr->left)
	{
		if (expr_check_leaves(&expr->left, get_filter))
			return -1;
		if (expr->right)
			return expr_check_leaves(&expr->right, get_filter);
		return 0;
	}

	for (i = 0; builtin[i].key; i++)
	{
		int cmp = strcmp(expr->key, builtin[i].key);

		if (cmp > 0)
			continue;
		if (cmp < 0)
			break;

		if (builtin[i].type != expr->type)
		{
			/* type mismatch */
			set_error("%s is %s", builtin[i].key, expr_names[builtin[i].type]);
			return -1;
		}
		return 0;
	}

	if (expr->type != EXPR_BOOL)
	{
		/* unknown key */
		set_error("unknown key %s", expr->key);
		return -1;
	}

	/* user defined filter */
	filter = get_filter(expr->key);
	if (filter == NULL)
	{
		set_error("unknown filter or boolean %s", expr->key);
		return -1;
	}
	e = expr_parse(filter);
	if (e == NULL)
	{
		return -1;
	}
	rc = expr_check_leaves(&e, get_filter);
	if (rc)
	{
		expr_free(e);
		return rc;
	}

	/* replace */
	e->parent = expr->parent;
	expr_free(expr);

	/* this sets parents left pointer */
	*exprp = e;
	return 0;
}

unsigned int expr_get_match_type(struct expr *expr)
{
	const char *key;

	if (expr->left)
	{
		unsigned int left = expr_get_match_type(expr->left);
		if (expr->type == EXPR_AND || expr->type == EXPR_OR)
			return left | expr_get_match_type(expr->right);
		return left;
	}

	key = expr->key;
	if (strcmp(key, "artist") == 0 || strcmp(key, "albumartist") == 0)
		return TI_MATCH_ARTIST;
	if (strcmp(key, "album") == 0 || strcmp(key, "discnumber") == 0)
		return TI_MATCH_ALBUM;
	if (strcmp(key, "title") == 0 || strcmp(key, "tracknumber") == 0)
		return TI_MATCH_TITLE;

	return 0;
}

int expr_is_harmless(const struct expr *expr)
{
	switch (expr->type)
	{
	case EXPR_OR:
	case EXPR_NOT:
		return 0;
	case EXPR_AND:
		expr = expr->right;
	default:
		break;
	}
	if (expr->type == EXPR_INT)
	{
		switch (expr->eint.op)
		{
		case IOP_LT:
		case IOP_EQ:
		case IOP_LE:
			return 0;
		default:
			return 1;
		}
	}
	if (expr->type == EXPR_ID)
		return 0;
	return 1;
}

static const char *str_val(const char *key, struct track_info *ti, char **need_free)
{
	const char *val;
	*need_free = NULL;
	if (strcmp(key, "filename") == 0)
	{
		val = ti->filename;
		if (!using_utf8 && utf8_encode(val, charset, need_free) == 0)
		{
			val = *need_free;
		}
	}
	else if (strcmp(key, "codec") == 0)
	{
		val = ti->codec;
	}
	else if (strcmp(key, "codec_profile") == 0)
	{
		val = ti->codec_profile;
	}
	else
	{
		val = keyvals_get_val(ti->comments, key);
	}
	return val;
}

static int int_val(const char *key, struct track_info *ti)
{
	int val;
	if (strcmp(key, "duration") == 0)
	{
		val = ti->duration;
		/* duration of a stream is infinite (well, almost) */
		if (is_http_url(ti->filename))
			val = INT_MAX;
	}
	else if (strcmp(key, "date") == 0)
	{
		val = (ti->date >= 0) ? (ti->date / 10000) : -1;
	}
	else if (strcmp(key, "originaldate") == 0)
	{
		val = (ti->originaldate >= 0) ? (ti->originaldate / 10000) : -1;
	}
	else if (strcmp(key, "bitrate") == 0)
	{
		val = (ti->bitrate >= 0) ? (int)(ti->bitrate / 1000. + 0.5) : -1;
	}
	else if (strcmp(key, "play_count") == 0)
	{
		val = ti->play_count;
	}
	else if (strcmp(key, "bpm") == 0)
	{
		val = ti->bpm;
	}
	else
	{
		val = comments_get_int(ti->comments, key);
	}
	return val;
}

int expr_op_to_bool(int res, int op)
{
	switch (op)
	{
	case OP_LT:
		return res < 0;
	case OP_LE:
		return res <= 0;
	case OP_EQ:
		return res == 0;
	case OP_GE:
		return res >= 0;
	case OP_GT:
		return res > 0;
	case OP_NE:
		return res != 0;
	default:
		return 0;
	}
}

int expr_eval(struct expr *expr, struct track_info *ti)
{
	enum expr_type type;
	const char *key;

	expr_debug_log("DEBUG: Entering expr_eval, expr=%p, ti=%p\n", expr, ti);

	if (!expr)
	{
		expr_debug_log("DEBUG: expr_eval - expr is NULL, returning 0\n");
		return 0;
	}

	if (!ti)
	{
		expr_debug_log("DEBUG: expr_eval - ti is NULL, returning 0\n");
		return 0;
	}

	type = expr->type;
	expr_debug_log("DEBUG: expr_eval - expr type=%d\n", type);

	if (expr->left)
	{
		expr_debug_log("DEBUG: expr_eval - evaluating left expression\n");
		int left = expr_eval(expr->left, ti);
		expr_debug_log("DEBUG: expr_eval - left result=%d\n", left);

		if (type == EXPR_AND)
		{
			expr_debug_log("DEBUG: expr_eval - AND operator, evaluating right expression\n");
			int result = left && expr_eval(expr->right, ti);
			expr_debug_log("DEBUG: expr_eval - returning AND result=%d\n", result);
			return result;
		}
		if (type == EXPR_OR)
		{
			expr_debug_log("DEBUG: expr_eval - OR operator, evaluating right expression\n");
			int result = left || expr_eval(expr->right, ti);
			expr_debug_log("DEBUG: expr_eval - returning OR result=%d\n", result);
			return result;
		}
		/* EXPR_NOT */
		expr_debug_log("DEBUG: expr_eval - NOT operator, returning !%d=%d\n", left, !left);
		return !left;
	}

	key = expr->key;
	expr_debug_log("DEBUG: expr_eval - key=%s\n", key ? key : "NULL");

	if (type == EXPR_STR)
	{
		int res;
		char *need_free;

		expr_debug_log("DEBUG: expr_eval - EXPR_STR, calling str_val\n");
		const char *val = str_val(key, ti, &need_free);
		if (!val)
		{
			expr_debug_log("DEBUG: expr_eval - str_val returned NULL, using empty string\n");
			val = "";
		}
		else
		{
			expr_debug_log("DEBUG: expr_eval - str_val returned '%s'\n", val);
		}

		expr_debug_log("DEBUG: expr_eval - calling glob_match\n");
		res = glob_match(&expr->estr.glob_head, val);
		expr_debug_log("DEBUG: expr_eval - glob_match returned %d\n", res);

		free(need_free);

		if (expr->estr.op == SOP_EQ)
		{
			expr_debug_log("DEBUG: expr_eval - SOP_EQ, returning %d\n", res);
			return res;
		}
		expr_debug_log("DEBUG: expr_eval - SOP_NE, returning %d\n", !res);
		return !res;
	}
	else if (type == EXPR_INT)
	{
		expr_debug_log("DEBUG: expr_eval - EXPR_INT, calling int_val\n");
		int val = int_val(key, ti);
		expr_debug_log("DEBUG: expr_eval - int_val returned %d\n", val);

		int res;
		if (expr->eint.val == -1)
		{
			/* -1 is "not set"
			 * doesn't make sense to do 123 < "not set"
			 * but it makes sense to do date=-1 (date is not set)
			 */
			expr_debug_log("DEBUG: expr_eval - eint.val is -1 ('not set')\n");
			if (expr->eint.op == IOP_EQ)
			{
				expr_debug_log("DEBUG: expr_eval - IOP_EQ, returning %d\n", val == -1);
				return val == -1;
			}
			if (expr->eint.op == IOP_NE)
			{
				expr_debug_log("DEBUG: expr_eval - IOP_NE, returning %d\n", val != -1);
				return val != -1;
			}
		}
		if (val == -1)
		{
			/* tag not set, can't compare */
			expr_debug_log("DEBUG: expr_eval - val is -1 (tag not set), returning 0\n");
			return 0;
		}

		res = val - expr->eint.val;
		expr_debug_log("DEBUG: expr_eval - comparing %d with %d, result=%d, op=%d\n",
					   val, expr->eint.val, res, expr->eint.op);

		int bool_result = expr_op_to_bool(res, expr->eint.op);
		expr_debug_log("DEBUG: expr_eval - expr_op_to_bool returned %d\n", bool_result);
		return bool_result;
	}
	else if (type == EXPR_ID)
	{
		expr_debug_log("DEBUG: expr_eval - EXPR_ID\n");

		int a = 0, b = 0;
		const char *sa, *sb;
		char *fa, *fb;
		int res = 0;

		expr_debug_log("DEBUG: expr_eval - calling str_val on key '%s'\n", key);
		if ((sa = str_val(key, ti, &fa)))
		{
			expr_debug_log("DEBUG: expr_eval - str_val returned '%s'\n", sa);

			expr_debug_log("DEBUG: expr_eval - calling str_val on eid.key '%s'\n", expr->eid.key);
			if ((sb = str_val(expr->eid.key, ti, &fb)))
			{
				expr_debug_log("DEBUG: expr_eval - second str_val returned '%s'\n", sb);

				res = strcmp(sa, sb);
				expr_debug_log("DEBUG: expr_eval - strcmp result=%d\n", res);

				free(fa);
				free(fb);

				int bool_result = expr_op_to_bool(res, expr->eid.op);
				expr_debug_log("DEBUG: expr_eval - expr_op_to_bool returned %d\n", bool_result);
				return bool_result;
			}
			expr_debug_log("DEBUG: expr_eval - second str_val returned NULL\n");
			free(fa);
		}
		else
		{
			expr_debug_log("DEBUG: expr_eval - str_val returned NULL, using int_val\n");

			a = int_val(key, ti);
			expr_debug_log("DEBUG: expr_eval - first int_val returned %d\n", a);

			b = int_val(expr->eid.key, ti);
			expr_debug_log("DEBUG: expr_eval - second int_val returned %d\n", b);

			res = a - b;
			expr_debug_log("DEBUG: expr_eval - res = %d - %d = %d\n", a, b, res);

			if (a == -1 || b == -1)
			{
				expr_debug_log("DEBUG: expr_eval - a or b is -1\n");

				switch (expr->eid.op)
				{
				case KOP_EQ:
					expr_debug_log("DEBUG: expr_eval - KOP_EQ, returning %d\n", res == 0);
					return res == 0;
				case KOP_NE:
					expr_debug_log("DEBUG: expr_eval - KOP_NE, returning %d\n", res != 0);
					return res != 0;
				default:
					expr_debug_log("DEBUG: expr_eval - unsupported op, returning 0\n");
					return 0;
				}
			}

			int bool_result = expr_op_to_bool(res, expr->eid.op);
			expr_debug_log("DEBUG: expr_eval - expr_op_to_bool returned %d\n", bool_result);
			return bool_result;
		}
		expr_debug_log("DEBUG: expr_eval - returning res=%d\n", res);
		return res;
	}

	if (strcmp(key, "stream") == 0)
	{
		int result = is_http_url(ti->filename);
		expr_debug_log("DEBUG: expr_eval - key is 'stream', is_http_url returned %d\n", result);
		return result;
	}

	int result = track_info_has_tag(ti);
	expr_debug_log("DEBUG: expr_eval - calling track_info_has_tag, returned %d\n", result);
	return result;
}

void expr_free(struct expr *expr)
{
	if (expr->left)
	{
		expr_free(expr->left);
		if (expr->right)
			expr_free(expr->right);
	}
	free(expr->key);
	if (expr->type == EXPR_STR)
		glob_free(&expr->estr.glob_head);
	else if (expr->type == EXPR_ID)
		free(expr->eid.key);
	free(expr);
}

const char *expr_error(void)
{
	return error_buf;
}

// 公开的函数用于在程序退出时关闭调试文件
void expr_debug_exit(void)
{
	expr_debug_close();
}
