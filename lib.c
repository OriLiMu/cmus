/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2004-2006 Timo Hirvonen
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

#include "lib.h"
#include "editable.h"
#include "track_info.h"
#include "options.h"
#include "xmalloc.h"
#include "rbtree.h"
#include "debug.h"
#include "utils.h"
#include "u_collate.h"
#include "ui_curses.h" /* cur_view */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

struct editable lib_editable;
static struct editable_shared lib_editable_shared;

struct tree_track *lib_cur_track = NULL;
unsigned int play_sorted = 0;
enum aaa_mode aaa_mode = AAA_MODE_ALL;
/* used in ui_curses.c for status display */
char *lib_live_filter = NULL;

struct rb_root lib_shuffle_root;
struct rb_root lib_album_shuffle_root;
static struct expr *filter = NULL;
static struct expr *add_filter = NULL;
static int remove_from_hash = 1;

static struct expr *live_filter_expr = NULL;
static struct track_info *cur_track_ti = NULL;
static struct track_info *sel_track_ti = NULL;

// 调试文件处理函数
static FILE *lib_debug_fp = NULL;

static void lib_debug_init(void)
{
	if (!lib_debug_fp)
	{
		lib_debug_fp = fopen("/tmp/cmus_lib_debug.log", "w");
		if (lib_debug_fp)
		{
			fprintf(lib_debug_fp, "===== CMUS LIB DEBUG LOG STARTED =====\n");
			fflush(lib_debug_fp);
		}
	}
}

static void lib_debug_log(const char *format, ...)
{
	if (!lib_debug_fp)
	{
		lib_debug_init();
		if (!lib_debug_fp)
			return;
	}

	va_list ap;
	va_start(ap, format);
	vfprintf(lib_debug_fp, format, ap);
	va_end(ap);
	fflush(lib_debug_fp);
}

static void lib_debug_close(void)
{
	if (lib_debug_fp)
	{
		fprintf(lib_debug_fp, "===== CMUS LIB DEBUG LOG CLOSED =====\n");
		fclose(lib_debug_fp);
		lib_debug_fp = NULL;
	}
}

const char *artist_sort_name(const struct artist *a)
{
	if (a->sort_name)
		return a->sort_name;

	if (smart_artist_sort && a->auto_sort_name)
		return a->auto_sort_name;

	return a->name;
}

static inline void sorted_track_to_iter(struct tree_track *track, struct iter *iter)
{
	iter->data0 = &lib_editable.head;
	iter->data1 = track;
	iter->data2 = NULL;
}

static void all_wins_changed(void)
{
	lib_tree_win->changed = 1;
	lib_track_win->changed = 1;
	lib_editable.shared->win->changed = 1;
}

static void shuffle_add(struct tree_track *track)
{
	shuffle_list_add(&track->simple_track.shuffle_info, &lib_shuffle_root, track->album);
}

static void album_shuffle_list_add(struct album *album)
{
	shuffle_list_add(&album->shuffle_info, &lib_album_shuffle_root, album);
}

static void album_shuffle_list_remove(struct album *album)
{
	rb_erase(&album->shuffle_info.tree_node, &lib_album_shuffle_root);
}

static void views_add_track(struct track_info *ti)
{
	struct tree_track *track = xnew(struct tree_track, 1);

	/* NOTE: does not ref ti */
	simple_track_init((struct simple_track *)track, ti);

	/* both the hash table and views have refs */
	track_info_ref(ti);

	tree_add_track(track, album_shuffle_list_add);
	shuffle_add(track);
	editable_add(&lib_editable, (struct simple_track *)track);
}

struct fh_entry
{
	struct fh_entry *next;

	/* ref count is increased when added to this hash */
	struct track_info *ti;
};

#define FH_SIZE (1024)
static struct fh_entry *ti_hash[FH_SIZE] = {
	NULL,
};

static int hash_insert(struct track_info *ti)
{
	const char *filename = ti->filename;
	unsigned int pos = hash_str(filename) % FH_SIZE;
	struct fh_entry **entryp;
	struct fh_entry *e;

	entryp = &ti_hash[pos];
	e = *entryp;
	while (e)
	{
		if (strcmp(e->ti->filename, filename) == 0)
		{
			/* found, don't insert */
			return 0;
		}
		e = e->next;
	}

	e = xnew(struct fh_entry, 1);
	track_info_ref(ti);
	e->ti = ti;
	e->next = *entryp;
	*entryp = e;
	return 1;
}

static void hash_remove(struct track_info *ti)
{
	const char *filename = ti->filename;
	unsigned int pos = hash_str(filename) % FH_SIZE;
	struct fh_entry **entryp;

	entryp = &ti_hash[pos];
	while (1)
	{
		struct fh_entry *e = *entryp;

		BUG_ON(e == NULL);
		if (strcmp(e->ti->filename, filename) == 0)
		{
			*entryp = e->next;
			track_info_unref(e->ti);
			free(e);
			break;
		}
		entryp = &e->next;
	}
}

static int is_filtered(struct track_info *ti)
{
	lib_debug_log("DEBUG: Entering is_filtered for track %s\n", ti ? ti->filename : "NULL");

	if (!ti)
	{
		lib_debug_log("DEBUG: is_filtered - ti is NULL, returning 1\n");
		return 1;
	}

	if (live_filter_expr)
	{
		lib_debug_log("DEBUG: is_filtered - checking live_filter_expr (type: %d)\n", live_filter_expr->type);

		lib_debug_log("DEBUG: is_filtered - calling expr_eval on live_filter_expr\n");
		int result = expr_eval(live_filter_expr, ti);
		lib_debug_log("DEBUG: is_filtered - expr_eval returned %d\n", result);

		if (!result)
		{
			lib_debug_log("DEBUG: is_filtered - track filtered by live_filter_expr, returning 1\n");
			return 1;
		}
	}
	else
	{
		lib_debug_log("DEBUG: is_filtered - live_filter_expr is NULL\n");
	}

	if (!live_filter_expr && lib_live_filter)
	{
		lib_debug_log("DEBUG: is_filtered - checking lib_live_filter: '%s'\n", lib_live_filter);

		lib_debug_log("DEBUG: is_filtered - calling track_info_matches\n");
		int result = track_info_matches(ti, lib_live_filter, TI_MATCH_ALL);
		lib_debug_log("DEBUG: is_filtered - track_info_matches returned %d\n", result);

		if (!result)
		{
			lib_debug_log("DEBUG: is_filtered - track filtered by lib_live_filter, returning 1\n");
			return 1;
		}
	}
	else if (!live_filter_expr)
	{
		lib_debug_log("DEBUG: is_filtered - lib_live_filter is NULL\n");
	}

	if (filter)
	{
		lib_debug_log("DEBUG: is_filtered - checking filter (type: %d)\n", filter->type);

		lib_debug_log("DEBUG: is_filtered - calling expr_eval on filter\n");
		int result = expr_eval(filter, ti);
		lib_debug_log("DEBUG: is_filtered - expr_eval returned %d\n", result);

		if (!result)
		{
			lib_debug_log("DEBUG: is_filtered - track filtered by filter, returning 1\n");
			return 1;
		}
	}
	else
	{
		lib_debug_log("DEBUG: is_filtered - filter is NULL\n");
	}

	lib_debug_log("DEBUG: is_filtered - track not filtered, returning 0\n");
	return 0;
}

static bool track_exists(struct track_info *ti)
{
	struct rb_node *node;
	struct artist *artist;
	struct album *album;
	struct tree_track *track;

	if (!ti->collkey_title)
		return false;

	char *artist_collkey_name = u_strcasecoll_key(tree_artist_name(ti));
	rb_for_each_entry(artist, node, &lib_artist_root, tree_node)
	{
		if (strcmp(artist->collkey_name, artist_collkey_name) == 0)
			break;
	}
	free(artist_collkey_name);

	if (!artist)
		return false;

	char *album_collkey_name = u_strcasecoll_key(tree_album_name(ti));
	rb_for_each_entry(album, node, &artist->album_root, tree_node)
	{
		if (strcmp(album->collkey_name, album_collkey_name) == 0)
			break;
	}
	free(album_collkey_name);

	if (!album)
		return false;

	rb_for_each_entry(track, node, &album->track_root, tree_node)
	{
		struct track_info *iter_ti = tree_track_info(track);
		if (iter_ti->tracknumber == ti->tracknumber && iter_ti->discnumber == ti->discnumber && iter_ti->collkey_title && strcmp(iter_ti->collkey_title, ti->collkey_title) == 0)
			return true;
	}
	return false;
}

void lib_add_track(struct track_info *ti, void *opaque)
{
	if (!ti)
		return;

	if (add_filter && !expr_eval(add_filter, ti))
	{
		/* filter any files excluded by lib_add_filter */
		return;
	}

	if (ignore_duplicates && track_exists(ti))
		return;

	if (!hash_insert(ti))
	{
		/* duplicate files not allowed */
		return;
	}

	if (!is_filtered(ti))
		views_add_track(ti);
}

static struct tree_track *album_first_track(const struct album *album)
{
	return to_tree_track(rb_first(&album->track_root));
}

static struct tree_track *artist_first_track(const struct artist *artist)
{
	return album_first_track(to_album(rb_first(&artist->album_root)));
}

static struct tree_track *normal_get_first(void)
{
	return artist_first_track(to_artist(rb_first(&lib_artist_root)));
}

static struct tree_track *album_last_track(const struct album *album)
{
	return to_tree_track(rb_last(&album->track_root));
}

static struct tree_track *artist_last_track(const struct artist *artist)
{
	return album_last_track(to_album(rb_last(&artist->album_root)));
}

static struct tree_track *normal_get_last(void)
{
	return artist_last_track(to_artist(rb_last(&lib_artist_root)));
}

static int aaa_mode_filter(const struct album *album)
{
	if (aaa_mode == AAA_MODE_ALBUM)
		return CUR_ALBUM == album;

	if (aaa_mode == AAA_MODE_ARTIST)
		return CUR_ARTIST == album->artist;

	/* AAA_MODE_ALL */
	return 1;
}

static int cur_album_filter(const struct album *album)
{
	return CUR_ALBUM == album;
}

/* set next/prev (tree) {{{ */

static struct tree_track *normal_get_next(enum aaa_mode aaa, bool allow_repeat, bool skip_album)
{
	if (lib_cur_track == NULL)
	{
		if (!allow_repeat)
			return NULL;
		return normal_get_first();
	}

	/* not last track of the album? */
	if (!skip_album && rb_next(&lib_cur_track->tree_node))
	{
		/* next track of the current album */
		return to_tree_track(rb_next(&lib_cur_track->tree_node));
	}

	if (aaa == AAA_MODE_ALBUM)
	{
		if (!allow_repeat || !repeat)
			return NULL;
		/* first track of the current album */
		return album_first_track(CUR_ALBUM);
	}

	/* not last album of the artist? */
	if (rb_next(&CUR_ALBUM->tree_node) != NULL)
	{
		/* first track of the next album */
		return album_first_track(to_album(rb_next(&CUR_ALBUM->tree_node)));
	}

	if (aaa == AAA_MODE_ARTIST)
	{
		if (!allow_repeat || !repeat)
			return NULL;
		/* first track of the first album of the current artist */
		return artist_first_track(CUR_ARTIST);
	}

	/* not last artist of the library? */
	if (rb_next(&CUR_ARTIST->tree_node) != NULL)
	{
		/* first track of the next artist */
		return artist_first_track(to_artist(rb_next(&CUR_ARTIST->tree_node)));
	}

	if (!allow_repeat || !repeat)
		return NULL;

	/* first track */
	return normal_get_first();
}

static struct tree_track *normal_get_prev(enum aaa_mode aaa, bool allow_repeat, bool skip_album)
{
	if (lib_cur_track == NULL)
	{
		if (!allow_repeat)
			return NULL;
		return normal_get_last();
	}

	/* not first track of the album? */
	if (!skip_album && rb_prev(&lib_cur_track->tree_node))
	{
		/* prev track of the album */
		return to_tree_track(rb_prev(&lib_cur_track->tree_node));
	}

	if (aaa == AAA_MODE_ALBUM)
	{
		if (!allow_repeat || !repeat)
			return NULL;
		/* last track of the album */
		return album_last_track(CUR_ALBUM);
	}

	/* not first album of the artist? */
	if (rb_prev(&CUR_ALBUM->tree_node) != NULL)
	{
		/* last track of the prev album of the artist */
		return album_last_track(to_album(rb_prev(&CUR_ALBUM->tree_node)));
	}

	if (aaa == AAA_MODE_ARTIST)
	{
		if (!allow_repeat || !repeat)
			return NULL;
		/* last track of the last album of the artist */
		return album_last_track(to_album(rb_last(&CUR_ARTIST->album_root)));
	}

	/* not first artist of the library? */
	if (rb_prev(&CUR_ARTIST->tree_node) != NULL)
	{
		/* last track of the last album of the prev artist */
		return artist_last_track(to_artist(rb_prev(&CUR_ARTIST->tree_node)));
	}

	if (!allow_repeat || !repeat)
		return NULL;

	/* last track */
	return normal_get_last();
}

static struct tree_track *shuffle_album_get_next(void)
{
	struct shuffle_info *shuffle_info = NULL;
	struct album *album;

	if (lib_cur_track != NULL)
		shuffle_info = &lib_cur_track->album->shuffle_info;
	album = (struct album *)shuffle_list_get_next(&lib_album_shuffle_root,
												  shuffle_info, aaa_mode_filter);
	if (album != NULL)
		return album_first_track(album);
	return NULL;
}

static struct tree_track *shuffle_album_get_prev(void)
{
	struct shuffle_info *shuffle_info = NULL;
	struct album *album;

	if (lib_cur_track != NULL)
		shuffle_info = &lib_cur_track->album->shuffle_info;
	album = (struct album *)shuffle_list_get_prev(&lib_album_shuffle_root,
												  shuffle_info, aaa_mode_filter);
	if (album != NULL)
		return album_last_track(album);
	return NULL;
}

static struct tree_track *sorted_album_first_track(struct tree_track *track)
{
	struct tree_track *prev = track;

	while (true)
	{
		prev = (struct tree_track *)simple_list_get_prev(&lib_editable.head,
														 (struct simple_track *)prev, NULL, false);
		if (prev == NULL)
			return track;
		if (prev->album == track->album)
			track = prev;
	}
}

static struct tree_track *sorted_album_last_track(struct tree_track *track)
{
	struct tree_track *next = track;

	while (true)
	{
		next = (struct tree_track *)simple_list_get_next(&lib_editable.head,
														 (struct simple_track *)next, NULL, false);
		if (next == NULL)
			return track;
		if (next->album == track->album)
			track = next;
	}
}

/* set next/prev (tree) }}} */

void lib_reshuffle(void)
{
	shuffle_list_reshuffle(&lib_shuffle_root);
	shuffle_list_reshuffle(&lib_album_shuffle_root);
	if (lib_cur_track)
	{
		shuffle_insert(&lib_shuffle_root, NULL, &lib_cur_track->simple_track.shuffle_info);
		shuffle_insert(&lib_album_shuffle_root, NULL, &lib_cur_track->album->shuffle_info);
	}
}

void lib_sort_artists(void)
{
	tree_sort_artists(album_shuffle_list_add, album_shuffle_list_remove);
}

static void free_lib_track(struct editable *e, struct list_head *item)
{
	struct tree_track *track = (struct tree_track *)to_simple_track(item);
	struct track_info *ti = tree_track_info(track);

	if (track == lib_cur_track)
		lib_cur_track = NULL;

	if (remove_from_hash)
		hash_remove(ti);

	rb_erase(&track->simple_track.shuffle_info.tree_node, &lib_shuffle_root);
	tree_remove(track, album_shuffle_list_remove);

	track_info_unref(ti);
	free(track);
}

void lib_init(void)
{
	editable_shared_init(&lib_editable_shared, free_lib_track);
	editable_init(&lib_editable, &lib_editable_shared, 1);
	tree_init();
	srand(time(NULL));
}

struct track_info *lib_set_track(struct tree_track *track)
{
	struct track_info *ti = NULL;

	if (track)
	{
		lib_cur_track = track;
		ti = tree_track_info(track);
		track_info_ref(ti);
		if (follow)
		{
			tree_sel_current(auto_expand_albums_follow);
			sorted_sel_current();
		}
		all_wins_changed();
	}
	return ti;
}

struct track_info *lib_goto_next(void)
{
	struct tree_track *track;

	if (rb_root_empty(&lib_artist_root))
	{
		BUG_ON(lib_cur_track != NULL);
		return NULL;
	}
	if (shuffle == SHUFFLE_TRACKS)
	{
		track = (struct tree_track *)shuffle_list_get_next(&lib_shuffle_root,
														   (struct shuffle_info *)lib_cur_track, aaa_mode_filter);
	}
	else if (shuffle == SHUFFLE_ALBUMS)
	{
		if (play_sorted)
			track = (struct tree_track *)simple_list_get_next(&lib_editable.head,
															  (struct simple_track *)lib_cur_track, cur_album_filter, false);
		else
			track = normal_get_next(AAA_MODE_ALBUM, false, false);
		if (track == NULL)
		{
			track = shuffle_album_get_next();
			if (play_sorted)
				track = sorted_album_first_track(track);
		}
	}
	else if (play_sorted)
	{
		track = (struct tree_track *)simple_list_get_next(&lib_editable.head,
														  (struct simple_track *)lib_cur_track, aaa_mode_filter, true);
	}
	else
	{
		track = normal_get_next(aaa_mode, true, false);
	}
	return lib_set_track(track);
}

struct track_info *lib_goto_prev(void)
{
	struct tree_track *track;

	if (rb_root_empty(&lib_artist_root))
	{
		BUG_ON(lib_cur_track != NULL);
		return NULL;
	}
	if (shuffle == SHUFFLE_TRACKS)
	{
		track = (struct tree_track *)shuffle_list_get_prev(&lib_shuffle_root,
														   (struct shuffle_info *)lib_cur_track, aaa_mode_filter);
	}
	else if (shuffle == SHUFFLE_ALBUMS)
	{
		if (play_sorted)
			track = (struct tree_track *)simple_list_get_prev(&lib_editable.head,
															  (struct simple_track *)lib_cur_track, cur_album_filter, false);
		else
			track = normal_get_prev(AAA_MODE_ALBUM, false, false);
		if (track == NULL)
		{
			track = shuffle_album_get_prev();
			if (play_sorted)
				track = sorted_album_last_track(track);
		}
	}
	else if (play_sorted)
	{
		track = (struct tree_track *)simple_list_get_prev(&lib_editable.head,
														  (struct simple_track *)lib_cur_track, aaa_mode_filter, true);
	}
	else
	{
		track = normal_get_prev(aaa_mode, true, false);
	}
	return lib_set_track(track);
}

struct track_info *lib_goto_next_album(void)
{
	struct tree_track *track = NULL;

	if (rb_root_empty(&lib_artist_root))
	{
		BUG_ON(lib_cur_track != NULL);
		return NULL;
	}
	if (shuffle == SHUFFLE_TRACKS)
	{
		return lib_goto_next();
	}
	else if (shuffle == SHUFFLE_ALBUMS)
	{
		track = shuffle_album_get_next();
		if (play_sorted)
			track = sorted_album_first_track(track);
	}
	else if (play_sorted)
	{
		track = sorted_album_last_track(lib_cur_track);
		track = (struct tree_track *)simple_list_get_next(&lib_editable.head,
														  (struct simple_track *)track, aaa_mode_filter, true);
	}
	else
	{
		track = normal_get_next(aaa_mode, true, true);
	}

	return lib_set_track(track);
}

struct track_info *lib_goto_prev_album(void)
{
	struct tree_track *track = NULL;

	if (rb_root_empty(&lib_artist_root))
	{
		BUG_ON(lib_cur_track != NULL);
		return NULL;
	}
	if (shuffle == SHUFFLE_TRACKS)
	{
		return lib_goto_prev();
	}
	else if (shuffle == SHUFFLE_ALBUMS)
	{
		track = shuffle_album_get_prev();
		if (play_sorted)
			track = sorted_album_first_track(track);
		else if (track)
			track = album_first_track(track->album);
	}
	else if (play_sorted)
	{
		track = sorted_album_first_track(lib_cur_track);
		track = (struct tree_track *)simple_list_get_prev(&lib_editable.head,
														  (struct simple_track *)track, aaa_mode_filter, true);
		track = sorted_album_first_track(track);
	}
	else
	{
		track = normal_get_prev(aaa_mode, true, true);
		if (track)
			track = album_first_track(track->album);
	}

	return lib_set_track(track);
}

static struct tree_track *sorted_get_selected(void)
{
	struct iter sel;

	if (list_empty(&lib_editable.head))
		return NULL;

	window_get_sel(lib_editable.shared->win, &sel);
	return iter_to_sorted_track(&sel);
}

struct track_info *sorted_activate_selected(void)
{
	return lib_set_track(sorted_get_selected());
}

static void hash_add_to_views(void)
{
	int i;
	for (i = 0; i < FH_SIZE; i++)
	{
		struct fh_entry *e;

		e = ti_hash[i];
		while (e)
		{
			struct track_info *ti = e->ti;

			if (!is_filtered(ti) && !(ignore_duplicates && track_exists(ti)))
				views_add_track(ti);
			e = e->next;
		}
	}
}

struct tree_track *lib_find_track(struct track_info *ti)
{
	struct simple_track *track;

	list_for_each_entry(track, &lib_editable.head, node)
	{
		if (strcmp(track->info->filename, ti->filename) == 0)
		{
			struct tree_track *tt = (struct tree_track *)track;
			return tt;
		}
	}
	return NULL;
}

void lib_store_cur_track(struct track_info *ti)
{
	if (cur_track_ti)
		track_info_unref(cur_track_ti);
	cur_track_ti = ti;
	track_info_ref(cur_track_ti);
}

struct track_info *lib_get_cur_stored_track(void)
{
	if (cur_track_ti && lib_find_track(cur_track_ti))
		return cur_track_ti;
	return NULL;
}

static void restore_cur_track(struct track_info *ti)
{
	struct tree_track *tt = lib_find_track(ti);
	if (tt)
		lib_cur_track = tt;
}

static int is_filtered_cb(void *data, struct track_info *ti)
{
	return is_filtered(ti);
}

static void do_lib_filter(int clear_before)
{
	lib_debug_log("DEBUG: Entering do_lib_filter, clear_before=%d\n", clear_before);

	/* try to save cur_track */
	if (lib_cur_track)
	{
		lib_debug_log("DEBUG: lib_cur_track exists, calling lib_store_cur_track\n");
		lib_store_cur_track(tree_track_info(lib_cur_track));
		lib_debug_log("DEBUG: lib_store_cur_track completed\n");
	}
	else
	{
		lib_debug_log("DEBUG: lib_cur_track is NULL\n");
	}

	if (clear_before)
	{
		lib_debug_log("DEBUG: filter results could grow, clear tracks and re-add (slow)\n");
	}

	remove_from_hash = 0;
	lib_debug_log("DEBUG: remove_from_hash set to 0\n");

	if (clear_before)
	{
		lib_debug_log("DEBUG: clear_before is true, calling editable_clear\n");
		editable_clear(&lib_editable);
		lib_debug_log("DEBUG: editable_clear completed\n");

		lib_debug_log("DEBUG: calling hash_add_to_views\n");
		hash_add_to_views();
		lib_debug_log("DEBUG: hash_add_to_views completed\n");
	}
	else
	{
		lib_debug_log("DEBUG: clear_before is false, calling editable_remove_matching_tracks\n");
		editable_remove_matching_tracks(&lib_editable, is_filtered_cb, NULL);
		lib_debug_log("DEBUG: editable_remove_matching_tracks completed\n");
	}

	lib_debug_log("DEBUG: setting remove_from_hash to 1\n");
	remove_from_hash = 1;

	lib_debug_log("DEBUG: calling window_changed\n");
	window_changed(lib_editable.shared->win);
	lib_debug_log("DEBUG: window_changed completed\n");

	lib_debug_log("DEBUG: calling window_goto_top\n");
	window_goto_top(lib_editable.shared->win);
	lib_debug_log("DEBUG: window_goto_top completed\n");

	lib_debug_log("DEBUG: setting lib_cur_win to lib_tree_win\n");
	lib_cur_win = lib_tree_win;

	lib_debug_log("DEBUG: calling window_goto_top on lib_tree_win\n");
	window_goto_top(lib_tree_win);
	lib_debug_log("DEBUG: window_goto_top completed\n");

	/* restore cur_track */
	if (cur_track_ti && !lib_cur_track)
	{
		lib_debug_log("DEBUG: cur_track_ti exists and lib_cur_track is NULL, calling restore_cur_track\n");
		restore_cur_track(cur_track_ti);
		lib_debug_log("DEBUG: restore_cur_track completed\n");
	}
	else
	{
		lib_debug_log("DEBUG: not calling restore_cur_track (cur_track_ti=%p, lib_cur_track=%p)\n",
					  cur_track_ti, lib_cur_track);
	}

	lib_debug_log("DEBUG: Exiting do_lib_filter\n");
}

static void unset_live_filter(void)
{
	free(lib_live_filter);
	lib_live_filter = NULL;
	free(live_filter_expr);
	live_filter_expr = NULL;
}

void lib_set_filter(struct expr *expr)
{
	int clear_before = lib_live_filter || filter;
	unset_live_filter();
	if (filter)
		expr_free(filter);
	filter = expr;
	do_lib_filter(clear_before);
}

void lib_set_add_filter(struct expr *expr)
{
	if (add_filter)
		expr_free(add_filter);
	add_filter = expr;
}

static struct tree_track *get_sel_track(void)
{
	switch (cur_view)
	{
	case TREE_VIEW:
		return tree_get_selected();
	case SORTED_VIEW:
		return sorted_get_selected();
	}
	return NULL;
}

static void set_sel_track(struct tree_track *tt)
{
	struct iter iter;

	switch (cur_view)
	{
	case TREE_VIEW:
		tree_sel_track(tt, auto_expand_albums_selcur);
		break;
	case SORTED_VIEW:
		sorted_track_to_iter(tt, &iter);
		window_set_sel(lib_editable.shared->win, &iter);
		break;
	}
}

static void store_sel_track(void)
{
	struct tree_track *tt = get_sel_track();
	if (tt)
	{
		sel_track_ti = tree_track_info(tt);
		track_info_ref(sel_track_ti);
	}
}

static void restore_sel_track(void)
{
	if (sel_track_ti)
	{
		struct tree_track *tt = lib_find_track(sel_track_ti);
		if (tt)
		{
			set_sel_track(tt);
			track_info_unref(sel_track_ti);
			sel_track_ti = NULL;
		}
	}
}

/* determine if filter results could grow, in which case all tracks must be cleared and re-added */
static int do_clear_before(const char *str, struct expr *expr)
{
	if (!lib_live_filter)
		return 0;
	if (!str)
		return 1;
	if ((!expr && live_filter_expr) || (expr && !live_filter_expr))
		return 1;
	if (!expr || expr_is_harmless(expr))
		return !strstr(str, lib_live_filter);
	return 1;
}

void lib_set_live_filter(const char *str)
{
	int clear_before;
	struct expr *expr = NULL;

	lib_debug_log("DEBUG: lib_set_live_filter called with str = %s\n", str ? str : "NULL");

	if (strcmp0(str, lib_live_filter) == 0)
		return;

	lib_debug_log("DEBUG: past strcmp0 check\n");

	if (str && expr_is_short(str))
	{
		lib_debug_log("DEBUG: str is short format, calling expr_parse\n");
		expr = expr_parse(str);
		if (!expr)
		{
			lib_debug_log("DEBUG: expr_parse returned NULL, error: %s\n", expr_error());
			ui_curses_display_error_msg(expr_error());
			return;
		}
		lib_debug_log("DEBUG: expr_parse successful\n");
	}

	clear_before = do_clear_before(str, expr);

	if (!str)
		store_sel_track();

	unset_live_filter();
	lib_live_filter = str ? xstrdup(str) : NULL;
	live_filter_expr = expr;
	lib_debug_log("DEBUG: calling do_lib_filter\n");
	do_lib_filter(clear_before);
	lib_debug_log("DEBUG: do_lib_filter completed\n");

	if (expr)
	{
		unsigned int match_type = expr_get_match_type(expr);
		if (match_type & TI_MATCH_ALBUM)
			tree_expand_all();
		if (match_type & TI_MATCH_TITLE)
			tree_sel_first();
	}
	else if (str)
		tree_expand_matching(str);

	if (!str)
		restore_sel_track();

	lib_debug_log("DEBUG: lib_set_live_filter completed\n");
}

int lib_remove(struct track_info *ti)
{
	struct simple_track *track;

	list_for_each_entry(track, &lib_editable.head, node)
	{
		if (track->info == ti)
		{
			editable_remove_track(&lib_editable, track);
			return 1;
		}
	}
	return 0;
}

void lib_clear_store(void)
{
	int i;

	for (i = 0; i < FH_SIZE; i++)
	{
		struct fh_entry *e, *next;

		e = ti_hash[i];
		while (e)
		{
			next = e->next;
			track_info_unref(e->ti);
			free(e);
			e = next;
		}
		ti_hash[i] = NULL;
	}
}

void sorted_sel_current(void)
{
	if (lib_cur_track)
	{
		struct iter iter;

		sorted_track_to_iter(lib_cur_track, &iter);
		window_set_sel(lib_editable.shared->win, &iter);
	}
}

static int ti_cmp(const void *a, const void *b)
{
	const struct track_info *ai = *(const struct track_info **)a;
	const struct track_info *bi = *(const struct track_info **)b;

	return track_info_cmp(ai, bi, lib_editable.shared->sort_keys);
}

static int do_lib_for_each(int (*cb)(void *data, struct track_info *ti), void *data, int filtered)
{
	int i, rc = 0, count = 0, size = 1024;
	struct track_info **tis;

	tis = xnew(struct track_info *, size);

	/* collect all track_infos */
	for (i = 0; i < FH_SIZE; i++)
	{
		struct fh_entry *e;

		e = ti_hash[i];
		while (e)
		{
			if (count == size)
			{
				size *= 2;
				tis = xrenew(struct track_info *, tis, size);
			}
			if (!filtered || !filter || expr_eval(filter, e->ti))
				tis[count++] = e->ti;
			e = e->next;
		}
	}

	/* sort to speed up playlist loading */
	qsort(tis, count, sizeof(struct track_info *), ti_cmp);
	for (i = 0; i < count; i++)
	{
		rc = cb(data, tis[i]);
		if (rc)
			break;
	}

	free(tis);
	return rc;
}

int lib_for_each(int (*cb)(void *data, struct track_info *ti), void *data,
				 void *opaque)
{
	return do_lib_for_each(cb, data, 0);
}

int lib_for_each_filtered(int (*cb)(void *data, struct track_info *ti),
						  void *data, void *opaque)
{
	return do_lib_for_each(cb, data, 1);
}

void lib_debug_exit(void)
{
	lib_debug_close();
}
