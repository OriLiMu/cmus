/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2004-2005 Timo Hirvonen
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

#include "uchar.h"
#include "compiler.h"
#include "gbuf.h"
#include "utils.h"	   /* N_ELEMENTS */
#include "ui_curses.h" /* using_utf8, charset */
#include "convert.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>

#include "unidecomp.h"
#include "wcwidth_uchar.h"

const char hex_tab[16] = "0123456789abcdef";

/*
 * Byte Sequence                                             Min       Min        Max
 * ----------------------------------------------------------------------------------
 * 0xxxxxxx                                              0000000   0x00000   0x00007f
 * 110xxxxx 10xxxxxx                                000 10000000   0x00080   0x0007ff
 * 1110xxxx 10xxxxxx 10xxxxxx                  00001000 00000000   0x00800   0x00ffff
 * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   00001 00000000 00000000   0x10000   0x10ffff (not 0x1fffff)
 *
 * max: 100   001111   111111   111111  (0x10ffff)
 */

/* Length of UTF-8 byte sequence.
 * Table index is the first byte of UTF-8 sequence.
 */
static const signed char len_tab[256] = {
	/*   0-127  0xxxxxxx */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,

	/* 128-191  10xxxxxx (invalid first byte) */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

	/* 192-223  110xxxxx */
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,

	/* 224-239  1110xxxx */
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,

	/* 240-244  11110xxx (000 - 100) */
	4, 4, 4, 4, 4,

	/* 11110xxx (101 - 111) (always invalid) */
	-1, -1, -1,

	/* 11111xxx (always invalid) */
	-1, -1, -1, -1, -1, -1, -1, -1};

/* fault-tolerant equivalent to len_tab, from glib */
static const char utf8_skip_data[256] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 1, 1};
const char *const utf8_skip = utf8_skip_data;

/* index is length of the UTF-8 sequence - 1 */
static int min_val[4] = {0x000000, 0x000080, 0x000800, 0x010000};
static int max_val[4] = {0x00007f, 0x0007ff, 0x00ffff, 0x10ffff};

/* get value bits from the first UTF-8 sequence byte */
static unsigned int first_byte_mask[4] = {0x7f, 0x1f, 0x0f, 0x07};

int u_is_valid(const char *str)
{
	const unsigned char *s = (const unsigned char *)str;
	int i = 0;

	while (s[i])
	{
		unsigned char ch = s[i++];
		int len = len_tab[ch];

		if (len <= 0)
			return 0;

		if (len > 1)
		{
			/* len - 1 10xxxxxx bytes */
			uchar u;
			int c;

			len--;
			u = ch & first_byte_mask[len];
			c = len;
			do
			{
				ch = s[i++];
				if (len_tab[ch] != 0)
					return 0;
				u = (u << 6) | (ch & 0x3f);
			} while (--c);

			if (u < min_val[len] || u > max_val[len])
				return 0;
		}
	}
	return 1;
}

size_t u_strlen(const char *str)
{
	size_t len;
	for (len = 0; *str; len++)
		str = u_next_char(str);
	return len;
}

size_t u_strlen_safe(const char *str)
{
	const unsigned char *s = (const unsigned char *)str;
	size_t len = 0;

	while (*s)
	{
		int l = len_tab[*s];

		if (unlikely(l > 1))
		{
			/* next l - 1 bytes must be 0x10xxxxxx */
			int c = 1;
			do
			{
				if (len_tab[s[c]] != 0)
				{
					/* invalid sequence */
					goto single_char;
				}
				c++;
			} while (c < l);

			/* valid sequence */
			s += l;
			len++;
			continue;
		}
	single_char:
		/* l is -1, 0 or 1
		 * invalid chars counted as single characters */
		s++;
		len++;
	}
	return len;
}

int u_char_width(uchar u)
{
	int w;

	if (unlikely(u < 0x20))
		goto control;

	if (unlikely(!using_utf8))
		return 1;

	/* invalid bytes in unicode stream are rendered "<xx>" */
	if (u & U_INVALID_MASK)
		goto invalid;

	w = wcwidth_uchar(u);
	if (w >= 0)
		return w;
	else
		return 1;

control:
	/* special case */
	if (u == 0)
		return 1;

	/* print control chars as <xx> */
invalid:
	return 4;
}

int u_str_width(const char *str)
{
	int idx = 0, w = 0;

	while (str[idx])
	{
		uchar u = u_get_char(str, &idx);
		w += u_char_width(u);
	}
	return w;
}

int u_str_nwidth(const char *str, int len)
{
	int idx = 0;
	int w = 0;

	while (len > 0)
	{
		uchar u = u_get_char(str, &idx);
		if (u == 0)
			break;
		w += u_char_width(u);
		len--;
	}
	return w;
}

char *u_strchr(const char *str, uchar uch)
{
	int idx = 0;

	while (str[idx])
	{
		uchar u = u_get_char(str, &idx);
		if (uch == u)
			return (char *)(str + idx);
	}
	return NULL;
}

void u_prev_char_pos(const char *str, int *idx)
{
	const unsigned char *s = (const unsigned char *)str;
	int c, len, i = *idx;
	uchar ch;

	ch = s[--i];
	len = len_tab[ch];
	if (len != 0)
	{
		/* start of byte sequence or invalid uchar */
		goto one;
	}

	c = 1;
	while (1)
	{
		if (i == 0)
		{
			/* first byte of the sequence is missing */
			break;
		}

		ch = s[--i];
		len = len_tab[ch];
		c++;

		if (len == 0)
		{
			if (c < 4)
				continue;

			/* too long sequence */
			break;
		}
		if (len != c)
		{
			/* incorrect length */
			break;
		}

		/* ok */
		*idx = i;
		return;
	}
one:
	*idx = *idx - 1;
	return;
}

uchar u_get_char(const char *str, int *idx)
{
	const unsigned char *s = (const unsigned char *)str;
	int len, i, x = 0;
	uchar ch, u;

	if (idx)
		s += *idx;
	else
		idx = &x;
	ch = s[0];

	/* ASCII optimization */
	if (ch < 128)
	{
		*idx += 1;
		return ch;
	}

	len = len_tab[ch];
	if (unlikely(len < 1))
		goto invalid;

	u = ch & first_byte_mask[len - 1];
	for (i = 1; i < len; i++)
	{
		ch = s[i];
		if (unlikely(len_tab[ch] != 0))
			goto invalid;
		u = (u << 6) | (ch & 0x3f);
	}
	*idx += len;
	return u;
invalid:
	*idx += 1;
	u = s[0];
	return u | U_INVALID_MASK;
}

void u_set_char_raw(char *str, int *idx, uchar uch)
{
	int i = *idx;

	if (uch <= 0x0000007fU)
	{
		str[i++] = uch;
		*idx = i;
	}
	else if (uch <= 0x000007ffU)
	{
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000c0U;
		i += 2;
		*idx = i;
	}
	else if (uch <= 0x0000ffffU)
	{
		str[i + 2] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000e0U;
		i += 3;
		*idx = i;
	}
	else if (uch <= 0x0010ffffU)
	{
		str[i + 3] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 2] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000f0U;
		i += 4;
		*idx = i;
	}
	else
	{
		/* must be an invalid uchar */
		str[i++] = uch & 0xff;
		*idx = i;
	}
}

/*
 * Printing functions, these lose information
 */

void u_set_char(char *str, size_t *idx, uchar uch)
{
	int i = *idx;

	if (unlikely(uch <= 0x0000001fU))
		goto invalid;

	if (uch <= 0x0000007fU)
	{
		str[i++] = uch;
		*idx = i;
		return;
	}
	else if (uch <= 0x000007ffU)
	{
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000c0U;
		i += 2;
		*idx = i;
		return;
	}
	else if (uch <= 0x0000ffffU)
	{
		str[i + 2] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000e0U;
		i += 3;
		*idx = i;
		return;
	}
	else if (uch <= 0x0010ffffU)
	{
		str[i + 3] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 2] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 1] = (uch & 63) | 0x80;
		uch >>= 6;
		str[i + 0] = uch | 0x000000f0U;
		i += 4;
		*idx = i;
		return;
	}
invalid:
	/* control character or invalid unicode */
	if (uch == 0)
	{
		/* handle this special case here to make the common case fast */
		str[i++] = 0;
		*idx = i;
	}
	else
	{
		str[i++] = '<';
		str[i++] = hex_tab[(uch >> 4) & 0xf];
		str[i++] = hex_tab[uch & 0xf];
		str[i++] = '>';
		*idx = i;
	}
}

size_t u_copy_chars(char *dst, const char *src, int *width)
{
	int w = *width;
	int si = 0;
	size_t di = 0;
	int cw;
	uchar u;

	while (w >= 0)
	{
		u = u_get_char(src, &si);
		if (u == 0)
			break;

		cw = u_char_width(u);
		w -= cw;

		if (unlikely(w < 0))
		{
			if (cw == 4 && w >= -3)
			{
				dst[di++] = '<';
				if (w >= -2)
					dst[di++] = hex_tab[(u >> 4) & 0xf];
				if (w >= -1)
					dst[di++] = hex_tab[u & 0xf];
				w = 0;
			}
			else
				w += cw;
			break;
		}
		u_set_char(dst, &di, u);
	}
	*width = w;
	return di;
}

int u_to_ascii(char *dst, const char *src, int len)
{
	int i, idx = 0;
	for (i = 0; i < len && src[idx]; i++)
	{
		uchar u = u_get_char(src, &idx);
		dst[i] = (u < 128) ? u : '?';
	}
	return i;
}

void u_to_utf8(char *dst, const char *src)
{
	int s = 0;
	size_t d = 0;
	uchar u;
	do
	{
		u = u_get_char(src, &s);
		u_set_char(dst, &d, u);
	} while (u != 0);
}

int u_print_size(uchar uch)
{
	int s = u_char_size(uch);
	/* control characters and invalid unicode set as <XX> */
	if (uch < 0x0000001fU && uch != 0)
	{
		return 4;
	}
	return s;
}

int u_str_print_size(const char *str)
{
	int l = 0;
	int idx = 0;
	uchar u;
	do
	{
		u = u_get_char(str, &idx);
		l += u_print_size(u);
	} while (u != 0);
	return l;
}

int u_skip_chars(const char *str, int *width, bool overskip)
{
	int w = *width;
	int last_idx = 0, idx = 0;
	uchar u = 0;

	while (w > 0)
	{
		last_idx = idx;
		u = u_get_char(str, &idx);
		w -= u_char_width(u);
	}
	/* undo last get if skipped 'too much' (the last char was double width or invalid (<xx>)) */
	if (w < 0 && !overskip)
	{
		w += u_char_width(u);
		idx = last_idx;
	}
	else
		while (1)
		{
			/* consume any zero-width characters (e.g. combining marks) */
			last_idx = idx;
			u = u_get_char(str, &idx);
			if (u_char_width(u) != 0)
			{
				idx = last_idx;
				break;
			}
		}
	*width = w;
	return idx;
}

/*
 * Case-folding functions
 */

static inline uchar u_casefold_char(uchar ch)
{
	/* faster lookup for for A-Z, rest of ASCII unaffected */
	if (ch < 0x0041)
		return ch;
	if (ch <= 0x005A)
		return ch + 0x20;
#if defined(_WIN32) || defined(__STDC_ISO_10646__) || defined(__APPLE__)
	if (ch < 128)
		return ch;
	ch = towlower(ch);
#endif
	return ch;
}

char *u_casefold(const char *str)
{
	GBUF(out);
	int i = 0;

	while (str[i])
	{
		char buf[4];
		int buflen = 0;
		uchar ch = u_get_char(str, &i);

		ch = u_casefold_char(ch);
		u_set_char_raw(buf, &buflen, ch);
		gbuf_add_bytes(&out, buf, buflen);
	}

	return gbuf_steal(&out);
}

/*
 * Comparison functions
 */

int u_strcase_equal(const char *a, const char *b)
{
	int ai = 0, bi = 0;

	while (a[ai])
	{
		uchar au, bu;

		au = u_get_char(a, &ai);
		bu = u_get_char(b, &bi);

		if (u_casefold_char(au) != u_casefold_char(bu))
			return 0;
	}

	return b[bi] ? 0 : 1;
}

static uchar get_base_from_composed(uchar ch)
{
	int begin = 0;
	int end = N_ELEMENTS(unidecomp_map);

	if (ch < unidecomp_map[begin].composed || ch > unidecomp_map[end - 1].composed)
		return ch;

	/* binary search */
	while (1)
	{
		int half = (begin + end) / 2;
		if (ch == unidecomp_map[half].composed)
			return unidecomp_map[half].base;
		else if (half == begin)
			break;
		else if (ch > unidecomp_map[half].composed)
			begin = half;
		else
			end = half;
	}
	return ch;
}

static inline int do_u_strncase_equal(const char *a, const char *b, size_t len, int only_base_chars)
{
	int ai = 0, bi = 0;
	size_t i;

	for (i = 0; i < len; i++)
	{
		uchar au, bu;

		au = u_get_char(a, &ai);
		bu = u_get_char(b, &bi);

		if (only_base_chars)
		{
			au = get_base_from_composed(au);
			bu = get_base_from_composed(bu);
		}

		if (u_casefold_char(au) != u_casefold_char(bu))
			return 0;
	}

	return 1;
}

int u_strncase_equal(const char *a, const char *b, size_t len)
{
	return do_u_strncase_equal(a, b, len, 0);
}

int u_strncase_equal_base(const char *a, const char *b, size_t len)
{
	return do_u_strncase_equal(a, b, len, 1);
}

static inline char *do_u_strcasestr(const char *haystack, const char *needle, int only_base_chars)
{
	if (!haystack || !needle || !*needle)
		return (char *)haystack;

	/* strlen is faster and works here */
	int haystack_len = strlen(haystack);
	int needle_len = u_strlen(needle);

	do
	{
		int idx;

		if (haystack_len < needle_len)
			return NULL;
		if (do_u_strncase_equal(needle, haystack, needle_len, only_base_chars))
			return (char *)haystack;

		/* skip one char */
		idx = 0;
		u_get_char(haystack, &idx);
		haystack += idx;
		haystack_len -= idx;
	} while (1);
}

char *u_strcasestr(const char *haystack, const char *needle)
{
	return do_u_strcasestr(haystack, needle, 0);
}

char *u_strcasestr_base(const char *haystack, const char *needle)
{
	return do_u_strcasestr(haystack, needle, 1);
}

char *u_strcasestr_filename(const char *haystack, const char *needle)
{
	char *r = NULL, *ustr = NULL;
	if (!using_utf8 && utf8_encode(haystack, charset, &ustr) == 0)
		haystack = ustr;
	r = u_strcasestr_base(haystack, needle);
	free(ustr);
	return r;
}
