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

#include "job.h"
#include "convert.h"
#include "ui_curses.h"
#include "cmdline.h"
#include "search_mode.h"
#include "command_mode.h"
#include "options.h"
#include "play_queue.h"
#include "browser.h"
#include "filters.h"
#include "cmus.h"
#include "player.h"
#include "output.h"
#include "utils.h"
#include "lib.h"
#include "pl.h"
#include "xmalloc.h"
#include "xstrjoin.h"
#include "window.h"
#include "comment.h"
#include "misc.h"
#include "prog.h"
#include "uchar.h"
#include "spawn.h"
#include "server.h"
#include "keys.h"
#include "debug.h"
#include "help.h"
#include "worker.h"
#include "input.h"
#include "file.h"
#include "path.h"
#include "mixer.h"
#include "mpris.h"
#include "locking.h"
#include "pl_env.h"
#ifdef HAVE_CONFIG
#include "config/curses.h"
#include "config/iconv.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <ctype.h>
#include <dirent.h>
#include <locale.h>
#include <langinfo.h>
#ifdef HAVE_ICONV
#include <iconv.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <math.h>
#include <sys/time.h>

#if defined(__sun__) || defined(__CYGWIN__)
/* TIOCGWINSZ */
#include <termios.h>
#include <ncurses.h>
#else
#include <curses.h>
#endif

/* defined in <term.h> but without const */
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);

/* globals. documented in ui_curses.h */

volatile sig_atomic_t cmus_running = 1;
int ui_initialized = 0;
enum ui_input_mode input_mode = NORMAL_MODE;
int cur_view = TREE_VIEW;
int prev_view = -1;
struct searchable *searchable;
char *lib_filename = NULL;
char *lib_ext_filename = NULL;
char *play_queue_filename = NULL;
char *play_queue_ext_filename = NULL;
char *charset = NULL;
int using_utf8 = 0;

/* ------------------------------------------------------------------------- */

static char *lib_autosave_filename;
static char *play_queue_autosave_filename;

static GBUF(print_buffer);

/* destination buffer for utf8_encode_to_buf and utf8_decode */
static char conv_buffer[4096];

/* shown error message and time stamp
 * error is cleared if it is older than 3s and key was pressed
 */
static GBUF(error_buf);
static time_t error_time = 0;
/* info messages are displayed in different color */
static int msg_is_error;
static int error_count = 0;

static char *server_address = NULL;

/* used for messages to the client */
static int client_fd = -1;

static char tcap_buffer[64];
static const char *t_ts;
static const char *t_fs;

static int tree_win_x = 0;
static int tree_win_w = 0;

static int track_win_x = 0;
static int track_win_w = 0;

static int win_x = 0;
static int win_w = 0;
static int win_active = 1;

static int show_cursor;
static int cursor_x;
static int cursor_y;
static int cmdline_cursor_x;

static const int default_esc_delay = 25;

static char *title_buf = NULL;

static int in_bracketed_paste = 0;

enum
{
	CURSED_WIN,
	CURSED_WIN_CUR,
	CURSED_WIN_SEL,
	CURSED_WIN_SEL_CUR,

	CURSED_WIN_ACTIVE,
	CURSED_WIN_ACTIVE_CUR,
	CURSED_WIN_ACTIVE_SEL,
	CURSED_WIN_ACTIVE_SEL_CUR,

	CURSED_SEPARATOR,
	CURSED_WIN_TITLE,
	CURSED_COMMANDLINE,
	CURSED_STATUSLINE,

	CURSED_STATUSLINE_PROGRESS,
	CURSED_TITLELINE,
	CURSED_DIR,
	CURSED_ERROR,

	CURSED_INFO,
	CURSED_TRACKWIN_ALBUM,

	NR_CURSED
};

static unsigned char cursed_to_bg_idx[NR_CURSED] = {
	COLOR_WIN_BG,
	COLOR_WIN_BG,
	COLOR_WIN_INACTIVE_SEL_BG,
	COLOR_WIN_INACTIVE_CUR_SEL_BG,

	COLOR_WIN_BG,
	COLOR_WIN_BG,
	COLOR_WIN_SEL_BG,
	COLOR_WIN_CUR_SEL_BG,

	COLOR_WIN_BG,
	COLOR_WIN_TITLE_BG,
	COLOR_CMDLINE_BG,
	COLOR_STATUSLINE_BG,

	COLOR_STATUSLINE_PROGRESS_BG,
	COLOR_TITLELINE_BG,
	COLOR_WIN_BG,
	COLOR_CMDLINE_BG,

	COLOR_CMDLINE_BG,
	COLOR_TRACKWIN_ALBUM_BG,
};

static unsigned char cursed_to_fg_idx[NR_CURSED] = {
	COLOR_WIN_FG,
	COLOR_WIN_CUR,
	COLOR_WIN_INACTIVE_SEL_FG,
	COLOR_WIN_INACTIVE_CUR_SEL_FG,

	COLOR_WIN_FG,
	COLOR_WIN_CUR,
	COLOR_WIN_SEL_FG,
	COLOR_WIN_CUR_SEL_FG,

	COLOR_SEPARATOR,
	COLOR_WIN_TITLE_FG,
	COLOR_CMDLINE_FG,
	COLOR_STATUSLINE_FG,

	COLOR_STATUSLINE_PROGRESS_FG,
	COLOR_TITLELINE_FG,
	COLOR_WIN_DIR,
	COLOR_ERROR,

	COLOR_INFO,
	COLOR_TRACKWIN_ALBUM_FG,
};

static unsigned char cursed_to_attr_idx[NR_CURSED] = {
	COLOR_WIN_ATTR,
	COLOR_WIN_CUR_ATTR,
	COLOR_WIN_INACTIVE_SEL_ATTR,
	COLOR_WIN_INACTIVE_CUR_SEL_ATTR,

	COLOR_WIN_ATTR,
	COLOR_WIN_CUR_ATTR,
	COLOR_WIN_SEL_ATTR,
	COLOR_WIN_CUR_SEL_ATTR,

	COLOR_WIN_ATTR,
	COLOR_WIN_TITLE_ATTR,
	COLOR_CMDLINE_ATTR,
	COLOR_STATUSLINE_ATTR,

	COLOR_STATUSLINE_PROGRESS_ATTR,
	COLOR_TITLELINE_ATTR,
	COLOR_WIN_ATTR,
	COLOR_CMDLINE_ATTR,

	COLOR_CMDLINE_ATTR,
	COLOR_TRACKWIN_ALBUM_ATTR,
};

/* index is CURSED_*, value is fucking color pair */
static int pairs[NR_CURSED];

enum
{
	TF_ALBUMARTIST,
	TF_ARTIST,
	TF_ALBUM,
	TF_DISC,
	TF_TOTAL_DISCS,
	TF_TRACK,
	TF_TITLE,
	TF_PLAY_COUNT,
	TF_YEAR,
	TF_MAX_YEAR,
	TF_ORIGINALYEAR,
	TF_GENRE,
	TF_COMMENT,
	TF_DURATION,
	TF_DURATION_SEC,
	TF_ALBUMDURATION,
	TF_BITRATE,
	TF_CODEC,
	TF_CODEC_PROFILE,
	TF_PATHFILE,
	TF_FILE,
	TF_RG_TRACK_GAIN,
	TF_RG_TRACK_PEAK,
	TF_RG_ALBUM_GAIN,
	TF_RG_ALBUM_PEAK,
	TF_ARRANGER,
	TF_COMPOSER,
	TF_CONDUCTOR,
	TF_LYRICIST,
	TF_PERFORMER,
	TF_REMIXER,
	TF_LABEL,
	TF_PUBLISHER,
	TF_WORK,
	TF_OPUS,
	TF_PARTNUMBER,
	TF_PART,
	TF_SUBTITLE,
	TF_MEDIA,
	TF_VA,
	TF_STATUS,
	TF_POSITION,
	TF_POSITION_SEC,
	TF_TOTAL,
	TF_VOLUME,
	TF_LVOLUME,
	TF_RVOLUME,
	TF_BUFFER,
	TF_REPEAT,
	TF_CONTINUE,
	TF_FOLLOW,
	TF_SHUFFLE,
	TF_PLAYLISTMODE,
	TF_BPM,
	TF_PANEL,

	NR_TFS
};

static struct format_option track_fopts[NR_TFS + 1] = {
	DEF_FO_STR('A', "albumartist", 0),
	DEF_FO_STR('a', "artist", 0),
	DEF_FO_STR('l', "album", 0),
	DEF_FO_INT('D', "discnumber", 1),
	DEF_FO_INT('T', "totaldiscs", 1),
	DEF_FO_INT('n', "tracknumber", 1),
	DEF_FO_STR('t', "title", 0),
	DEF_FO_INT('X', "play_count", 0),
	DEF_FO_INT('y', "date", 1),
	DEF_FO_INT('\0', "maxdate", 1),
	DEF_FO_INT('\0', "originaldate", 1),
	DEF_FO_STR('g', "genre", 0),
	DEF_FO_STR('c', "comment", 0),
	DEF_FO_TIME('d', "duration", 0),
	DEF_FO_INT('\0', "duration_sec", 1),
	DEF_FO_TIME('\0', "albumduration", 0),
	DEF_FO_INT('\0', "bitrate", 0),
	DEF_FO_STR('\0', "codec", 0),
	DEF_FO_STR('\0', "codec_profile", 0),
	DEF_FO_STR('f', "path", 0),
	DEF_FO_STR('F', "filename", 0),
	DEF_FO_DOUBLE('\0', "rg_track_gain", 0),
	DEF_FO_DOUBLE('\0', "rg_track_peak", 0),
	DEF_FO_DOUBLE('\0', "rg_album_gain", 0),
	DEF_FO_DOUBLE('\0', "rg_album_peak", 0),
	DEF_FO_STR('\0', "arranger", 0),
	DEF_FO_STR('\0', "composer", 0),
	DEF_FO_STR('\0', "conductor", 0),
	DEF_FO_STR('\0', "lyricist", 0),
	DEF_FO_STR('\0', "performer", 0),
	DEF_FO_STR('\0', "remixer", 0),
	DEF_FO_STR('\0', "label", 0),
	DEF_FO_STR('\0', "publisher", 0),
	DEF_FO_STR('\0', "work", 0),
	DEF_FO_STR('\0', "opus", 0),
	DEF_FO_STR('\0', "partnumber", 0),
	DEF_FO_STR('\0', "part", 0),
	DEF_FO_STR('\0', "subtitle", 0),
	DEF_FO_STR('\0', "media", 0),
	DEF_FO_INT('\0', "va", 0),
	DEF_FO_STR('\0', "status", 0),
	DEF_FO_TIME('\0', "position", 0),
	DEF_FO_INT('\0', "position_sec", 1),
	DEF_FO_TIME('\0', "total", 0),
	DEF_FO_INT('\0', "volume", 1),
	DEF_FO_INT('\0', "lvolume", 1),
	DEF_FO_INT('\0', "rvolume", 1),
	DEF_FO_INT('\0', "buffer", 1),
	DEF_FO_STR('\0', "repeat", 0),
	DEF_FO_STR('\0', "continue", 0),
	DEF_FO_STR('\0', "follow", 0),
	DEF_FO_STR('\0', "shuffle", 0),
	DEF_FO_STR('\0', "playlist_mode", 0),
	DEF_FO_INT('\0', "bpm", 0),
	DEF_FO_INT('\0', "panel", 0),
	DEF_FO_END};

int get_track_win_x(void)
{
	return track_win_x;
}

int track_format_valid(const char *format)
{
	return format_valid(format, track_fopts);
}

static void utf8_encode_to_buf(const char *buffer)
{
	int n;
#ifdef HAVE_ICONV
	static iconv_t cd = (iconv_t)-1;
	size_t is, os;
	const char *i;
	char *o;
	int rc;

	if (cd == (iconv_t)-1)
	{
		d_print("iconv_open(UTF-8, %s)\n", charset);
		cd = iconv_open("UTF-8", charset);
		if (cd == (iconv_t)-1)
		{
			d_print("iconv_open failed: %s\n", strerror(errno));
			goto fallback;
		}
	}
	i = buffer;
	o = conv_buffer;
	is = strlen(i);
	os = sizeof(conv_buffer) - 1;
	rc = iconv(cd, (void *)&i, &is, &o, &os);
	*o = 0;
	if (rc == -1)
	{
		d_print("iconv failed: %s\n", strerror(errno));
		goto fallback;
	}
	return;
fallback:
#endif
	n = min_i(sizeof(conv_buffer) - 1, strlen(buffer));
	memmove(conv_buffer, buffer, n);
	conv_buffer[n] = '\0';
}

static void utf8_decode(const char *buffer)
{
	int n;
#ifdef HAVE_ICONV
	static iconv_t cd = (iconv_t)-1;
	size_t is, os;
	const char *i;
	char *o;
	int rc;

	if (cd == (iconv_t)-1)
	{
		d_print("iconv_open(%s, UTF-8)\n", charset);
		cd = iconv_open(charset, "UTF-8");
		if (cd == (iconv_t)-1)
		{
			d_print("iconv_open failed: %s\n", strerror(errno));
			goto fallback;
		}
	}
	i = buffer;
	o = conv_buffer;
	is = strlen(i);
	os = sizeof(conv_buffer) - 1;
	rc = iconv(cd, (void *)&i, &is, &o, &os);
	*o = 0;
	if (rc == -1)
	{
		d_print("iconv failed: %s\n", strerror(errno));
		goto fallback;
	}
	return;
fallback:
#endif
	n = u_to_ascii(conv_buffer, buffer, sizeof(conv_buffer) - 1);
	conv_buffer[n] = '\0';
}

/* screen updates {{{ */

static void dump_print_buffer_no_clear(int row, int col, size_t offset)
{
	if (using_utf8)
	{
		(void)mvaddstr(row, col, print_buffer.buffer + offset);
	}
	else
	{
		utf8_decode(print_buffer.buffer + offset);
		(void)mvaddstr(row, col, conv_buffer);
	}
}

static void dump_print_buffer(int row, int col)
{
	dump_print_buffer_no_clear(row, col, 0);
	gbuf_clear(&print_buffer);
}

/* print @str into @buf
 *
 * if @str is shorter than @width pad with spaces
 * if @str is wider than @width truncate and add "..."
 */
static void format_str(struct gbuf *buf, const char *str, int width)
{
	gbuf_add_ustr(buf, str, &width);
	gbuf_set(buf, ' ', width);
}

static void sprint(int row, int col, const char *str, int width)
{
	gbuf_add_ch(&print_buffer, ' ');
	format_str(&print_buffer, str, width - 2);
	gbuf_add_ch(&print_buffer, ' ');
	dump_print_buffer(row, col);
}

static inline void fopt_set_str(struct format_option *fopt, const char *str)
{
	BUG_ON(fopt->type != FO_STR);
	if (str)
	{
		fopt->fo_str = str;
		fopt->empty = 0;
	}
	else
	{
		fopt->empty = 1;
	}
}

static inline void fopt_set_int(struct format_option *fopt, int value, int empty)
{
	BUG_ON(fopt->type != FO_INT);
	fopt->fo_int = value;
	fopt->empty = empty;
}

static inline void fopt_set_double(struct format_option *fopt, double value, int empty)
{
	BUG_ON(fopt->type != FO_DOUBLE);
	fopt->fo_double = value;
	fopt->empty = empty;
}

static inline void fopt_set_time(struct format_option *fopt, int value, int empty)
{
	BUG_ON(fopt->type != FO_TIME);
	fopt->fo_time = value;
	fopt->empty = empty;
}

static void fill_track_fopts_track_info(struct track_info *info)
{
	char *filename;

	if (using_utf8)
	{
		filename = info->filename;
	}
	else
	{
		utf8_encode_to_buf(info->filename);
		filename = conv_buffer;
	}

	fopt_set_str(&track_fopts[TF_ALBUMARTIST], info->albumartist);
	fopt_set_str(&track_fopts[TF_ARTIST], info->artist);
	fopt_set_str(&track_fopts[TF_ALBUM], info->album);
	fopt_set_int(&track_fopts[TF_PLAY_COUNT], info->play_count, 0);
	fopt_set_int(&track_fopts[TF_DISC], info->discnumber, info->discnumber == -1);
	fopt_set_int(&track_fopts[TF_TOTAL_DISCS], info->totaldiscs, info->totaldiscs == -1);
	fopt_set_int(&track_fopts[TF_TRACK], info->tracknumber, info->tracknumber == -1);
	fopt_set_str(&track_fopts[TF_TITLE], info->title);
	fopt_set_int(&track_fopts[TF_YEAR], info->date / 10000, info->date <= 0);
	fopt_set_str(&track_fopts[TF_GENRE], info->genre);
	fopt_set_str(&track_fopts[TF_COMMENT], info->comment);
	fopt_set_time(&track_fopts[TF_DURATION], info->duration, info->duration == -1);
	fopt_set_int(&track_fopts[TF_DURATION_SEC], info->duration, info->duration == -1);
	fopt_set_double(&track_fopts[TF_RG_TRACK_GAIN], info->rg_track_gain, isnan(info->rg_track_gain));
	fopt_set_double(&track_fopts[TF_RG_TRACK_PEAK], info->rg_track_peak, isnan(info->rg_track_peak));
	fopt_set_double(&track_fopts[TF_RG_ALBUM_GAIN], info->rg_album_gain, isnan(info->rg_album_gain));
	fopt_set_double(&track_fopts[TF_RG_ALBUM_PEAK], info->rg_album_peak, isnan(info->rg_album_peak));
	fopt_set_int(&track_fopts[TF_ORIGINALYEAR], info->originaldate / 10000, info->originaldate <= 0);
	fopt_set_int(&track_fopts[TF_BITRATE], (int)(info->bitrate / 1000. + 0.5), info->bitrate == -1);
	fopt_set_str(&track_fopts[TF_CODEC], info->codec);
	fopt_set_str(&track_fopts[TF_CODEC_PROFILE], info->codec_profile);
	fopt_set_str(&track_fopts[TF_PATHFILE], filename);
	fopt_set_str(&track_fopts[TF_ARRANGER], keyvals_get_val(info->comments, "arranger"));
	fopt_set_str(&track_fopts[TF_COMPOSER], keyvals_get_val(info->comments, "composer"));
	fopt_set_str(&track_fopts[TF_CONDUCTOR], keyvals_get_val(info->comments, "conductor"));
	fopt_set_str(&track_fopts[TF_LYRICIST], keyvals_get_val(info->comments, "lyricist"));
	fopt_set_str(&track_fopts[TF_PERFORMER], keyvals_get_val(info->comments, "performer"));
	fopt_set_str(&track_fopts[TF_REMIXER], keyvals_get_val(info->comments, "remixer"));
	fopt_set_str(&track_fopts[TF_LABEL], keyvals_get_val(info->comments, "label"));
	fopt_set_str(&track_fopts[TF_PUBLISHER], keyvals_get_val(info->comments, "publisher"));
	fopt_set_str(&track_fopts[TF_WORK], keyvals_get_val(info->comments, "work"));
	fopt_set_str(&track_fopts[TF_OPUS], keyvals_get_val(info->comments, "opus"));
	fopt_set_str(&track_fopts[TF_PARTNUMBER], keyvals_get_val(info->comments, "discnumber"));
	fopt_set_str(&track_fopts[TF_PART], keyvals_get_val(info->comments, "discnumber"));
	fopt_set_str(&track_fopts[TF_SUBTITLE], keyvals_get_val(info->comments, "subtitle"));
	fopt_set_str(&track_fopts[TF_MEDIA], info->media);
	fopt_set_int(&track_fopts[TF_VA], 0, !track_is_compilation(info->comments));
	if (is_http_url(info->filename))
	{
		fopt_set_str(&track_fopts[TF_FILE], filename);
	}
	else
	{
		fopt_set_str(&track_fopts[TF_FILE], path_basename(filename));
	}
	fopt_set_int(&track_fopts[TF_BPM], info->bpm, info->bpm == -1);
}

static int get_album_length(struct album *album)
{
	struct tree_track *track;
	struct rb_node *tmp;
	int duration = 0;

	rb_for_each_entry(track, tmp, &album->track_root, tree_node)
	{
		duration += max_i(0, tree_track_info(track)->duration);
	}

	return duration;
}

static int get_artist_length(struct artist *artist)
{
	struct album *album;
	struct rb_node *tmp;
	int duration = 0;

	rb_for_each_entry(album, tmp, &artist->album_root, tree_node)
	{
		duration += get_album_length(album);
	}

	return duration;
}

static void fill_track_fopts_album(struct album *album)
{
	fopt_set_int(&track_fopts[TF_YEAR], album->min_date / 10000, album->min_date <= 0);
	fopt_set_int(&track_fopts[TF_MAX_YEAR], album->date / 10000, album->date <= 0);
	fopt_set_str(&track_fopts[TF_ALBUMARTIST], album->artist->name);
	fopt_set_str(&track_fopts[TF_ARTIST], album->artist->name);
	fopt_set_str(&track_fopts[TF_ALBUM], album->name);
	int duration = get_album_length(album);
	fopt_set_time(&track_fopts[TF_DURATION], duration, 0);
	fopt_set_time(&track_fopts[TF_ALBUMDURATION], duration, 0);
}

static void fill_track_fopts_artist(struct artist *artist)
{
	const char *name = display_artist_sort_name ? artist_sort_name(artist) : artist->name;
	fopt_set_str(&track_fopts[TF_ARTIST], name);
	fopt_set_str(&track_fopts[TF_ALBUMARTIST], name);
	fopt_set_time(&track_fopts[TF_DURATION], get_artist_length(artist), 0);
}

const struct format_option *get_global_fopts(void)
{
	if (player_info.ti)
		fill_track_fopts_track_info(player_info.ti);

	static const char *status_strs[] = {".", ">", "|"};
	static const char *cont_strs[] = {" ", "C"};
	static const char *follow_strs[] = {" ", "F"};
	static const char *repeat_strs[] = {" ", "R"};
	static const char *shuffle_strs[] = {" ", "S", "&"};
	int buffer_fill, vol, vol_left, vol_right;
	int duration = -1;

	unsigned int total_time = pl_playing_total_time();
	if (cmus_queue_active())
		total_time = play_queue_total_time();
	else if (play_library)
		total_time = lib_editable.total_time;
	fopt_set_time(&track_fopts[TF_TOTAL], total_time, 0);

	fopt_set_str(&track_fopts[TF_FOLLOW], follow_strs[follow]);
	fopt_set_str(&track_fopts[TF_REPEAT], repeat_strs[repeat]);
	fopt_set_str(&track_fopts[TF_SHUFFLE], shuffle_strs[shuffle]);
	fopt_set_str(&track_fopts[TF_PLAYLISTMODE], aaa_mode_names[aaa_mode]);

	if (player_info.ti)
		duration = player_info.ti->duration;

	vol_left = vol_right = vol = -1;
	if (soft_vol)
	{
		vol_left = soft_vol_l;
		vol_right = soft_vol_r;
		vol = (vol_left + vol_right + 1) / 2;
	}
	else if (volume_max && volume_l >= 0 && volume_r >= 0)
	{
		vol_left = scale_to_percentage(volume_l, volume_max);
		vol_right = scale_to_percentage(volume_r, volume_max);
		vol = (vol_left + vol_right + 1) / 2;
	}
	buffer_fill = scale_to_percentage(player_info.buffer_fill, player_info.buffer_size);

	fopt_set_str(&track_fopts[TF_STATUS], status_strs[player_info.status]);

	if (show_remaining_time && duration != -1)
	{
		fopt_set_time(&track_fopts[TF_POSITION], player_info.pos - duration, 0);
	}
	else
	{
		fopt_set_time(&track_fopts[TF_POSITION], player_info.pos, 0);
	}

	fopt_set_int(&track_fopts[TF_POSITION_SEC], player_info.pos, player_info.pos < 0);
	fopt_set_time(&track_fopts[TF_DURATION], duration, duration < 0);
	fopt_set_int(&track_fopts[TF_VOLUME], vol, vol < 0);
	fopt_set_int(&track_fopts[TF_LVOLUME], vol_left, vol_left < 0);
	fopt_set_int(&track_fopts[TF_RVOLUME], vol_right, vol_right < 0);
	fopt_set_int(&track_fopts[TF_BUFFER], buffer_fill, 0);
	fopt_set_str(&track_fopts[TF_CONTINUE], cont_strs[player_cont]);
	fopt_set_int(&track_fopts[TF_BITRATE], player_info.current_bitrate / 1000. + 0.5, 0);

	return track_fopts;
}

static void print_tree(struct window *win, int row, struct iter *iter)
{
	struct artist *artist;
	struct album *album;
	struct iter sel;
	int current, selected, active;

	artist = iter_to_artist(iter);
	album = iter_to_album(iter);
	current = 0;
	if (lib_cur_track)
	{
		if (album)
		{
			current = CUR_ALBUM == album;
		}
		else
		{
			current = CUR_ARTIST == artist;
		}
	}
	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);
	active = lib_cur_win == lib_tree_win;
	bkgdset(pairs[(active << 2) | (selected << 1) | current]);

	if (active && selected)
	{
		cursor_x = 0;
		cursor_y = 1 + row;
	}

	gbuf_add_ch(&print_buffer, ' ');
	if (album)
	{
		fill_track_fopts_album(album);
		format_print(&print_buffer, tree_win_w - 1, tree_win_format, track_fopts);
	}
	else
	{
		fill_track_fopts_artist(artist);
		format_print(&print_buffer, tree_win_w - 1, tree_win_artist_format, track_fopts);
	}
	dump_print_buffer(row + 1, tree_win_x);
}

static void print_track(struct window *win, int row, struct iter *iter)
{
	struct tree_track *track;
	struct album *album;
	struct track_info *ti;
	struct iter sel;
	int current, selected, active;
	const char *format;

	track = iter_to_tree_track(iter);
	album = iter_to_album(iter);

	if (track == (struct tree_track *)album)
	{
		int pos;
		struct fp_len len;

		bkgdset(pairs[CURSED_TRACKWIN_ALBUM]);

		fill_track_fopts_album(album);

		len = format_print(&print_buffer, track_win_w, track_win_album_format, track_fopts);
		dump_print_buffer(row + 1, track_win_x);

		bkgdset(pairs[CURSED_SEPARATOR]);
		for (pos = track_win_x + len.llen + len.mlen; pos < win_w - len.rlen; ++pos)
			(void)mvaddch(row + 1, pos, ACS_HLINE);

		return;
	}

	current = lib_cur_track == track;
	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);
	active = lib_cur_win == lib_track_win;
	bkgdset(pairs[(active << 2) | (selected << 1) | current]);

	if (active && selected)
	{
		cursor_x = track_win_x;
		cursor_y = 1 + row;
	}

	ti = tree_track_info(track);
	fill_track_fopts_track_info(ti);

	format = track_win_format;
	if (track_info_has_tag(ti))
	{
		if (*track_win_format_va && track_is_compilation(ti->comments))
			format = track_win_format_va;
	}
	else if (*track_win_alt_format)
	{
		format = track_win_alt_format;
	}
	format_print(&print_buffer, track_win_w, format, track_fopts);
	dump_print_buffer(row + 1, track_win_x);
}

/* used by print_editable only */
static struct simple_track *current_track;

static void print_editable(struct window *win, int row, struct iter *iter)
{
	struct simple_track *track;
	struct iter sel;
	int current, selected, active;
	const char *format;

	track = iter_to_simple_track(iter);
	current = current_track == track;
	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);

	if (selected)
	{
		cursor_x = win_x;
		cursor_y = 1 + row;
	}

	active = win_active;
	if (!selected && !!track->marked)
	{
		selected = 1;
		active = 0;
	}

	bkgdset(pairs[(active << 2) | (selected << 1) | current]);

	fill_track_fopts_track_info(track->info);

	format = list_win_format;
	if (track_info_has_tag(track->info))
	{
		if (*list_win_format_va && track_is_compilation(track->info->comments))
			format = list_win_format_va;
	}
	else if (*list_win_alt_format)
	{
		format = list_win_alt_format;
	}
	format_print(&print_buffer, win_w, format, track_fopts);
	dump_print_buffer(row + 1, win_x);
}

static void print_browser(struct window *win, int row, struct iter *iter)
{
	struct browser_entry *e;
	struct iter sel;
	int selected;

	e = iter_to_browser_entry(iter);
	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);
	if (selected)
	{
		int active = 1;
		int current = 0;

		bkgdset(pairs[(active << 2) | (selected << 1) | current]);
	}
	else
	{
		if (e->type == BROWSER_ENTRY_DIR)
		{
			bkgdset(pairs[CURSED_DIR]);
		}
		else
		{
			bkgdset(pairs[CURSED_WIN]);
		}
	}

	if (selected)
	{
		cursor_x = 0;
		cursor_y = 1 + row;
	}

	sprint(row + 1, 0, e->name, win_w);
}

static void print_filter(struct window *win, int row, struct iter *iter)
{
	char buf[256];
	struct filter_entry *e = iter_to_filter_entry(iter);
	struct iter sel;
	/* window active? */
	int active = 1;
	/* row selected? */
	int selected;
	/* is the filter currently active? */
	int current = !!e->act_stat;
	const char stat_chars[3] = " *!";
	int ch1, ch2, ch3;
	const char *e_filter;

	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);
	bkgdset(pairs[(active << 2) | (selected << 1) | current]);

	if (selected)
	{
		cursor_x = 0;
		cursor_y = 1 + row;
	}

	ch1 = ' ';
	ch3 = ' ';
	if (e->sel_stat != e->act_stat)
	{
		ch1 = '[';
		ch3 = ']';
	}
	ch2 = stat_chars[e->sel_stat];

	e_filter = e->filter;
	if (!using_utf8)
	{
		utf8_encode_to_buf(e_filter);
		e_filter = conv_buffer;
	}

	snprintf(buf, sizeof(buf), "%c%c%c%-15s  %.235s", ch1, ch2, ch3, e->name, e_filter);
	format_str(&print_buffer, buf, win_w - 1);
	gbuf_add_ch(&print_buffer, ' ');
	dump_print_buffer(row + 1, 0);
}

static void print_help(struct window *win, int row, struct iter *iter)
{
	struct iter sel;
	int selected;
	int active = 1;
	char buf[OPTION_MAX_SIZE];
	const struct help_entry *e = iter_to_help_entry(iter);
	const struct cmus_opt *opt;

	window_get_sel(win, &sel);
	selected = iters_equal(iter, &sel);
	bkgdset(pairs[(active << 2) | (selected << 1)]);

	if (selected)
	{
		cursor_x = 0;
		cursor_y = 1 + row;
	}

	switch (e->type)
	{
	case HE_TEXT:
		snprintf(buf, sizeof(buf), " %s", e->text);
		break;
	case HE_BOUND:
		snprintf(buf, sizeof(buf), " %-8s %-23s %s",
				 key_context_names[e->binding->ctx],
				 e->binding->key->name,
				 e->binding->cmd);
		break;
	case HE_UNBOUND:
		snprintf(buf, sizeof(buf), " %s", e->command->name);
		break;
	case HE_OPTION:
		opt = e->option;
		snprintf(buf, sizeof(buf), " %-29s ", opt->name);
		size_t len = strlen(buf);
		opt->get(opt->data, buf + len, sizeof(buf) - len);
		break;
	}
	format_str(&print_buffer, buf, win_w - 1);
	gbuf_add_ch(&print_buffer, ' ');
	dump_print_buffer(row + 1, 0);
}

static void update_window(struct window *win, int x, int y, int w, const char *title,
						  void (*print)(struct window *, int, struct iter *))
{
	struct iter iter;
	int nr_rows;
	int i;

	win->changed = 0;

	bkgdset(pairs[CURSED_WIN_TITLE]);
	sprint(y, x, title, w);

	nr_rows = window_get_nr_rows(win);
	i = 0;
	if (window_get_top(win, &iter))
	{
		while (i < nr_rows)
		{
			print(win, i, &iter);
			i++;
			if (!window_get_next(win, &iter))
				break;
		}
	}

	bkgdset(pairs[0]);
	gbuf_set(&print_buffer, ' ', w);
	while (i < nr_rows)
	{
		dump_print_buffer_no_clear(y + i + 1, x, 0);
		i++;
	}
	gbuf_clear(&print_buffer);
}

static void update_tree_window(void)
{
	static GBUF(buf);
	gbuf_clear(&buf);

	gbuf_add_str(&buf, "Library");
	if (worker_has_job())
		gbuf_addf(&buf, " - %d tracks", lib_editable.nr_tracks);
	update_window(lib_tree_win, tree_win_x, 0, tree_win_w + 1, buf.buffer, print_tree);
}

static void update_track_window(void)
{
	static GBUF(title);
	gbuf_clear(&title);

	struct iter iter;
	struct album *album;
	struct artist *artist;

	const char *format_str = "Empty (use :add)";

	if (window_get_sel(lib_tree_win, &iter))
	{
		if ((album = iter_to_album(&iter)))
		{
			fill_track_fopts_album(album);
			format_str = heading_album_format;
		}
		else if ((artist = iter_to_artist(&iter)))
		{
			fill_track_fopts_artist(artist);
			format_str = heading_artist_format;
		}
	}
	format_print(&title, track_win_w - 2, format_str, track_fopts);
	update_window(lib_track_win, track_win_x, 0, track_win_w, title.buffer,
				  print_track);
}

static void print_pl_list(struct window *win, int row, struct iter *iter)
{
	struct pl_list_info info;

	pl_list_iter_to_info(iter, &info);

	bkgdset(pairs[(info.active << 2) | (info.selected << 1) | info.current]);

	const char *prefix = "   ";
	if (info.marked)
		prefix = " * ";
	size_t prefix_w = strlen(prefix);
	format_str(&print_buffer, prefix, prefix_w);

	if (tree_win_w > prefix_w)
		format_str(&print_buffer, info.name,
				   tree_win_w - prefix_w);

	dump_print_buffer(row + 1, 0);
}

static void draw_separator(void)
{
	int row;

	bkgdset(pairs[CURSED_WIN_TITLE]);
	(void)mvaddch(0, tree_win_w, ' ');
	bkgdset(pairs[CURSED_SEPARATOR]);
	for (row = 1; row < LINES - 3; row++)
		(void)mvaddch(row, tree_win_w, ACS_VLINE);
}

static void update_pl_list(struct window *win)
{
	if (pl_show_panel())
	{
		update_window(win, tree_win_x, 0, tree_win_w + 1, "Playlist", print_pl_list);
		draw_separator();
	}
}

static void update_pl_tracks(struct window *win)
{
	static GBUF(title);
	gbuf_clear(&title);
	int win_w_tmp = win_w;

	if (pl_show_panel())
	{
		win_x = track_win_x;
		win_w = track_win_w;
	}
	else
	{
		win_x = 0;
		win_w = tree_win_w + 1 + track_win_w;
	}
	win_active = pl_get_cursor_in_track_window();

	get_global_fopts();
	fopt_set_int(&track_fopts[TF_PANEL], 1, !pl_show_panel());
	fopt_set_str(&track_fopts[TF_TITLE], pl_visible_get_name());
	fopt_set_time(&track_fopts[TF_DURATION], pl_visible_total_time(), 0);

	format_print(&title, win_w - 2, heading_playlist_format, track_fopts);
	update_window(win, win_x, 0, win_w, title.buffer, print_editable);

	win_active = 1;
	win_x = 0;
	win_w = win_w_tmp;
}

static const char *pretty_path(const char *path)
{
	static int home_len = -1;
	static GBUF(buf);

	if (home_len == -1)
		home_len = strlen(home_dir);

	if (strncmp(path, home_dir, home_len) || path[home_len] != '/')
		return path;

	gbuf_clear(&buf);
	gbuf_add_ch(&buf, '~');
	gbuf_add_str(&buf, path + home_len);
	return buf.buffer;
}

static const char *const sorted_names[2] = {"", "sorted by "};

static void update_editable_window(struct editable *e, const char *title, const char *filename)
{
	static GBUF(buf);
	gbuf_clear(&buf);

	if (filename)
	{
		if (using_utf8)
		{
			/* already UTF-8 */
		}
		else
		{
			utf8_encode_to_buf(filename);
			filename = conv_buffer;
		}
		gbuf_addf(&buf, "%s %.256s - %d tracks", title, pretty_path(filename), e->nr_tracks);
	}
	else
	{
		gbuf_addf(&buf, "%s - %d tracks", title, e->nr_tracks);
	}

	fopt_set_time(&track_fopts[TF_TOTAL], e->total_time, 0);
	format_print(&buf, 0, " (%{total})", track_fopts);

	if (e->nr_marked)
	{
		gbuf_addf(&buf, " (%d marked)", e->nr_marked);
	}
	gbuf_addf(&buf, " %s%s",
			  sorted_names[e->shared->sort_str[0] != 0],
			  e->shared->sort_str);

	update_window(e->shared->win, 0, 0, win_w, buf.buffer, &print_editable);
}

static void update_sorted_window(void)
{
	current_track = (struct simple_track *)lib_cur_track;
	update_editable_window(&lib_editable, "Library", NULL);
}

static void update_play_queue_window(void)
{
	current_track = NULL;
	update_editable_window(&pq_editable, "Play Queue", NULL);
}

static void update_browser_window(void)
{
	static GBUF(title);
	gbuf_clear(&title);
	char *dirname;

	if (using_utf8)
	{
		/* already UTF-8 */
		dirname = browser_dir;
	}
	else
	{
		utf8_encode_to_buf(browser_dir);
		dirname = conv_buffer;
	}
	gbuf_add_str(&title, "Browser - ");
	gbuf_add_str(&title, dirname);
	update_window(browser_win, 0, 0, win_w, title.buffer, print_browser);
}

static void update_filters_window(void)
{
	update_window(filters_win, 0, 0, win_w, "Library Filters", print_filter);
}

static void update_help_window(void)
{
	update_window(help_win, 0, 0, win_w, "Settings", print_help);
}

static void update_pl_view(int full)
{
	current_track = pl_get_playing_track();
	pl_draw(update_pl_list, update_pl_tracks, full);
}

static void do_update_view(int full)
{
	if (!ui_initialized)
		return;

	cursor_x = -1;
	cursor_y = -1;

	switch (cur_view)
	{
	case TREE_VIEW:
		if (full || lib_tree_win->changed)
			update_tree_window();
		if (full || lib_track_win->changed)
			update_track_window();
		draw_separator();
		update_filterline();
		break;
	case SORTED_VIEW:
		update_sorted_window();
		update_filterline();
		break;
	case PLAYLIST_VIEW:
		update_pl_view(full);
		break;
	case QUEUE_VIEW:
		update_play_queue_window();
		break;
	case BROWSER_VIEW:
		update_browser_window();
		break;
	case FILTERS_VIEW:
		update_filters_window();
		break;
	case HELP_VIEW:
		update_help_window();
		break;
	}
}

static void do_update_statusline(void)
{
	struct fp_len len;
	len = format_print(&print_buffer, win_w, statusline_format, get_global_fopts());
	bkgdset(pairs[CURSED_STATUSLINE]);
	dump_print_buffer_no_clear(LINES - 2, 0, 0);

	if (progress_bar && player_info.ti)
	{
		int duration = player_info.ti->duration;
		if (duration && duration >= player_info.pos)
		{
			if (progress_bar == PROGRESS_BAR_LINE || progress_bar == PROGRESS_BAR_SHUTTLE)
			{
				/* Draw a bar or short position marker within the blank space */
				int shuttle_len = (progress_bar == PROGRESS_BAR_SHUTTLE) ? 2 : 0;
				int bar_start = len.llen + len.mlen;
				int bar_space = win_w - len.rlen - bar_start - shuttle_len;
				if (bar_space >= 5)
				{
					int bar_len = bar_space * player_info.pos / duration;
					if (progress_bar == PROGRESS_BAR_SHUTTLE)
					{
						bar_start += bar_len;
						bar_len = shuttle_len;
					}
					for (int x = bar_start; bar_len; --bar_len)
						(void)mvaddstr(LINES - 2, x++, using_utf8 ? "━" : "-");
				}
			}
			else if (progress_bar == PROGRESS_BAR_COLOR)
			{
				/* Draw over the played portion of bar in alt color */
				int w = win_w * player_info.pos / duration;

				int skip = w;
				int buf_index = u_skip_chars(print_buffer.buffer, &skip, false);
				print_buffer.buffer[buf_index] = '\0';

				bkgdset(pairs[CURSED_STATUSLINE_PROGRESS]);
				dump_print_buffer_no_clear(LINES - 2, 0, 0);
			}
			else
			{ // PROGRESS_BAR_COLOR_SHUTTLE
				/* Redraw a few cols in alt color to mark the current position */
				int shuttle_len = min_u(6, win_w);
				int x = (win_w - shuttle_len) * player_info.pos / duration;

				int skip = x;
				int buf_index = u_skip_chars(print_buffer.buffer, &skip, false);

				int end_offset = u_skip_chars(print_buffer.buffer + buf_index, &shuttle_len, true);
				print_buffer.buffer[buf_index + end_offset] = '\0';

				bkgdset(pairs[CURSED_STATUSLINE_PROGRESS]);
				dump_print_buffer_no_clear(LINES - 2, x, buf_index);
			}
		}
	}

	gbuf_clear(&print_buffer);

	if (player_info.error_msg)
		error_msg("%s", player_info.error_msg);
}

static void dump_buffer(const char *buffer)
{
	if (using_utf8)
	{
		addstr(buffer);
	}
	else
	{
		utf8_decode(buffer);
		addstr(conv_buffer);
	}
}

static void do_update_commandline(void)
{
	char *str;
	size_t idx = 0;
	char ch;

	move(LINES - 1, 0);
	if (error_buf.len != 0)
	{
		if (msg_is_error)
		{
			bkgdset(pairs[CURSED_ERROR]);
		}
		else
		{
			bkgdset(pairs[CURSED_INFO]);
		}
		addstr(error_buf.buffer);
		clrtoeol();
		return;
	}
	bkgdset(pairs[CURSED_COMMANDLINE]);
	if (input_mode == NORMAL_MODE)
	{
		clrtoeol();
		return;
	}

	str = cmdline.line;
	if (!using_utf8)
	{
		/* cmdline.line actually pretends to be UTF-8 but all non-ASCII
		 * characters are invalid UTF-8 so it really is in locale's
		 * encoding.
		 *
		 * This code should be safe because cmdline.bpos ==
		 * cmdline.cpos as every non-ASCII character is counted as one
		 * invalid UTF-8 byte.
		 *
		 * NOTE: This has nothing to do with widths of printed
		 * characters.  I.e. even if there were control characters
		 * (displayed as <xx>) there would be no problem because bpos
		 * still equals to cpos, I think.
		 */
		utf8_encode_to_buf(cmdline.line);
		str = conv_buffer;
	}

	/* COMMAND_MODE or SEARCH_MODE */
	ch = ':';
	if (input_mode == SEARCH_MODE)
		ch = search_direction == SEARCH_FORWARD ? '/' : '?';

	int width = win_w - 2; // ':' at start and ' ' at end

	/* width of the text in the buffer before and after cursor */
	int cw = u_str_nwidth(str, cmdline.cpos);
	int extra_w = u_str_width(str + cmdline.bpos);

	/* shift by third of bar width to provide visual context when editing */
	int context_w = min_u(extra_w, win_w / 3);

	int skip = cw + context_w - width;
	if (skip <= 0)
	{
		addch(ch);
		cmdline_cursor_x = 1 + cw;
	}
	else
	{
		/* ':' will not be printed */
		skip--;
		width++;
		idx = u_skip_chars(str, &skip, true);
		gbuf_set(&print_buffer, ' ', -skip);
		width += skip;
		cmdline_cursor_x = win_w - 1 - context_w;
	}
	/* allow printing in ' ' space we kept at end, cursor isn't always there */
	width++;
	gbuf_add_ustr(&print_buffer, str + idx, &width);
	dump_buffer(print_buffer.buffer);
	gbuf_clear(&print_buffer);
	clrtoeol();
}

static void set_title(const char *title)
{
	if (!set_term_title)
		return;

	if (t_ts)
	{
		printf("%s%s%s", tgoto(t_ts, 0, 0), title, t_fs);
		fflush(stdout);
	}
}

static void do_update_titleline(void)
{
	if (!ui_initialized)
		return;

	bkgdset(pairs[CURSED_TITLELINE]);
	if (player_info.ti)
	{
		int use_alt_format = 0;
		char *wtitle;

		fill_track_fopts_track_info(player_info.ti);

		use_alt_format = !track_info_has_tag(player_info.ti);

		if (is_http_url(player_info.ti->filename))
		{
			const char *title = get_stream_title();

			if (title != NULL)
			{
				free(title_buf);
				title_buf = to_utf8(title, icecast_default_charset);
				/*
				 * StreamTitle overrides radio station name
				 */
				use_alt_format = 0;
				fopt_set_str(&track_fopts[TF_TITLE], title_buf);
			}
		}

		if (use_alt_format && *current_alt_format)
		{
			format_print(&print_buffer, win_w, current_alt_format, track_fopts);
		}
		else
		{
			format_print(&print_buffer, win_w, current_format, track_fopts);
		}
		dump_print_buffer(LINES - 3, 0);

		/* set window title */
		if (use_alt_format && *window_title_alt_format)
		{
			format_print(&print_buffer, 0,
						 window_title_alt_format, track_fopts);
		}
		else
		{
			format_print(&print_buffer, 0,
						 window_title_format, track_fopts);
		}

		if (using_utf8)
		{
			wtitle = print_buffer.buffer;
		}
		else
		{
			utf8_decode(print_buffer.buffer);
			wtitle = conv_buffer;
		}

		set_title(wtitle);
		gbuf_clear(&print_buffer);
	}
	else
	{
		move(LINES - 3, 0);
		clrtoeol();

		set_title("cmus " VERSION);
	}
}

static void post_update(void)
{
	/* refresh makes cursor visible at least for urxvt */
	if (input_mode == COMMAND_MODE || input_mode == SEARCH_MODE)
	{
		move(LINES - 1, cmdline_cursor_x);
		refresh();
		curs_set(1);
	}
	else
	{
		if (cursor_x >= 0)
		{
			move(cursor_y, cursor_x);
		}
		else
		{
			move(LINES - 1, 0);
		}
		refresh();

		/* visible cursor is useful for screen readers */
		if (show_cursor)
		{
			curs_set(1);
		}
		else
		{
			curs_set(0);
		}
	}
}

static const char *get_stream_title_locked(void)
{
	static char stream_title[255 * 16 + 1];
	char *ptr, *title;

	ptr = strstr(player_metadata, "StreamTitle='");
	if (ptr == NULL)
		return NULL;
	ptr += 13;
	title = ptr;
	while (*ptr)
	{
		if (*ptr == '\'' && *(ptr + 1) == ';')
		{
			memcpy(stream_title, title, ptr - title);
			stream_title[ptr - title] = 0;
			return stream_title;
		}
		ptr++;
	}
	return NULL;
}

const char *get_stream_title(void)
{
	player_metadata_lock();
	const char *rv = get_stream_title_locked();
	player_metadata_unlock();
	return rv;
}

void update_titleline(void)
{
	curs_set(0);
	do_update_titleline();
	post_update();
}

void update_full(void)
{
	if (!ui_initialized)
		return;

	curs_set(0);

	do_update_view(1);
	do_update_titleline();
	do_update_statusline();
	do_update_commandline();

	post_update();
}

static void update_commandline(void)
{
	curs_set(0);
	do_update_commandline();
	post_update();
}

void update_statusline(void)
{
	if (!ui_initialized)
		return;

	curs_set(0);
	do_update_statusline();
	post_update();
}

void update_filterline(void)
{
	if (cur_view != TREE_VIEW && cur_view != SORTED_VIEW)
		return;
	if (lib_live_filter)
	{
		static GBUF(buf);
		gbuf_clear(&buf);
		int w;
		bkgdset(pairs[CURSED_STATUSLINE]);
		gbuf_addf(&buf, "filtered: %s", lib_live_filter);
		w = clamp(u_str_width(buf.buffer) + 2, win_w / 4, win_w / 2);
		sprint(LINES - 4, win_w - w, buf.buffer, w);
	}
}

void info_msg(const char *format, ...)
{
	va_list ap;

	gbuf_clear(&error_buf);
	va_start(ap, format);
	gbuf_vaddf(&error_buf, format, ap);
	va_end(ap);

	if (client_fd != -1)
	{
		write_all(client_fd, error_buf.buffer, error_buf.len);
		write_all(client_fd, "\n", 1);
	}

	msg_is_error = 0;

	update_commandline();
}

void error_msg(const char *format, ...)
{
	va_list ap;

	gbuf_clear(&error_buf);
	gbuf_add_str(&error_buf, "Error: ");
	va_start(ap, format);
	gbuf_vaddf(&error_buf, format, ap);
	va_end(ap);

	d_print("%s\n", error_buf.buffer);
	if (client_fd != -1)
	{
		write_all(client_fd, error_buf.buffer, error_buf.len);
		write_all(client_fd, "\n", 1);
	}

	msg_is_error = 1;
	error_count++;

	if (ui_initialized)
	{
		error_time = time(NULL);
		update_commandline();
	}
	else
	{
		warn("%s\n", error_buf.buffer);
		gbuf_clear(&error_buf);
	}
}

enum ui_query_answer yes_no_query(const char *format, ...)
{
	static GBUF(buffer);
	gbuf_clear(&buffer);
	va_list ap;
	int ret = 0;

	va_start(ap, format);
	gbuf_vaddf(&buffer, format, ap);
	va_end(ap);

	move(LINES - 1, 0);
	bkgdset(pairs[CURSED_INFO]);

	/* no need to convert buffer.
	 * it is always encoded in the right charset (assuming filenames are
	 * encoded in same charset as LC_CTYPE).
	 */

	addstr(buffer.buffer);
	clrtoeol();
	refresh();

	while (1)
	{
		int ch = getch();
		if (ch == ERR || ch == 0)
		{
			if (!cmus_running)
			{
				ret = UI_QUERY_ANSWER_ERROR;
				break;
			}
			continue;
		}

		if (ch == 'y')
		{
			ret = UI_QUERY_ANSWER_YES;
			break;
		}
		else
		{
			ret = UI_QUERY_ANSWER_NO;
			break;
		}
	}
	update_commandline();
	return ret;
}

void search_not_found(void)
{
	const char *what = "Track";

	if (search_restricted)
	{
		switch (cur_view)
		{
		case TREE_VIEW:
			what = "Artist/album";
			break;
		case SORTED_VIEW:
		case PLAYLIST_VIEW:
		case QUEUE_VIEW:
			what = "Title";
			break;
		case BROWSER_VIEW:
			what = "File/Directory";
			break;
		case FILTERS_VIEW:
			what = "Filter";
			break;
		case HELP_VIEW:
			what = "Binding/command/option";
			break;
		}
	}
	else
	{
		switch (cur_view)
		{
		case TREE_VIEW:
		case SORTED_VIEW:
		case PLAYLIST_VIEW:
		case QUEUE_VIEW:
			what = "Track";
			break;
		case BROWSER_VIEW:
			what = "File/Directory";
			break;
		case FILTERS_VIEW:
			what = "Filter";
			break;
		case HELP_VIEW:
			what = "Binding/command/option";
			break;
		}
	}
	info_msg("%s not found: %s", what, search_str ? search_str : "");
}

void set_client_fd(int fd)
{
	client_fd = fd;
}

int get_client_fd(void)
{
	return client_fd;
}

void set_view(int view)
{
	if (view == cur_view)
		return;

	prev_view = cur_view;
	cur_view = view;
	switch (cur_view)
	{
	case TREE_VIEW:
		searchable = tree_searchable;
		break;
	case SORTED_VIEW:
		searchable = lib_editable.shared->searchable;
		break;
	case PLAYLIST_VIEW:
		searchable = pl_get_searchable();
		break;
	case QUEUE_VIEW:
		searchable = pq_editable.shared->searchable;
		break;
	case BROWSER_VIEW:
		searchable = browser_searchable;
		break;
	case FILTERS_VIEW:
		searchable = filters_searchable;
		break;
	case HELP_VIEW:
		searchable = help_searchable;
		update_help_window();
		break;
	}

	curs_set(0);
	do_update_view(1);
	post_update();
}

void enter_command_mode(void)
{
	gbuf_clear(&error_buf);
	error_time = 0;
	input_mode = COMMAND_MODE;
	update_commandline();
}

void enter_search_mode(void)
{
	gbuf_clear(&error_buf);
	error_time = 0;
	input_mode = SEARCH_MODE;
	search_direction = SEARCH_FORWARD;
	update_commandline();
}

void enter_search_backward_mode(void)
{
	gbuf_clear(&error_buf);
	error_time = 0;
	input_mode = SEARCH_MODE;
	search_direction = SEARCH_BACKWARD;
	update_commandline();
}

void update_colors(void)
{
	int i;

	if (!ui_initialized)
		return;

	for (i = 0; i < NR_CURSED; i++)
	{
		int bg = colors[cursed_to_bg_idx[i]];
		int fg = colors[cursed_to_fg_idx[i]];
		int attr = attrs[cursed_to_attr_idx[i]];
		int pair = i + 1;

		if (fg >= 8 && fg <= 15)
		{
			/* fg colors 8..15 are special (0..7 + bold) */
			init_pair(pair, fg & 7, bg);
			pairs[i] = COLOR_PAIR(pair) | (fg & BRIGHT ? A_BOLD : 0) | attr;
		}
		else
		{
			init_pair(pair, fg, bg);
			pairs[i] = COLOR_PAIR(pair) | attr;
		}
	}
}

static void clear_error(void)
{
	time_t t = time(NULL);

	/* prevent accidental clearing of error messages */
	if (t - error_time < 2)
		return;

	if (error_buf.len != 0)
	{
		error_time = 0;
		gbuf_clear(&error_buf);
		update_commandline();
	}
}

/* screen updates }}} */

static int fill_status_program_track_info_args(char **argv, int i, struct track_info *ti)
{
	/* returns first free argument index */

	const char *stream_title = NULL;
	if (player_info.status == PLAYER_STATUS_PLAYING && is_http_url(ti->filename))
		stream_title = get_stream_title();

	static const char *keys[] = {
		"artist", "albumartist", "album", "discnumber", "tracknumber", "title",
		"date", "musicbrainz_trackid", NULL};
	int j;

	if (is_http_url(ti->filename))
	{
		argv[i++] = xstrdup("url");
	}
	else
	{
		argv[i++] = xstrdup("file");
	}
	argv[i++] = xstrdup(ti->filename);

	if (track_info_has_tag(ti))
	{
		for (j = 0; keys[j]; j++)
		{
			const char *key = keys[j];
			const char *val;

			if (strcmp(key, "title") == 0 && stream_title)
				/*
				 * StreamTitle overrides radio station name
				 */
				val = stream_title;
			else
				val = keyvals_get_val(ti->comments, key);

			if (val)
			{
				argv[i++] = xstrdup(key);
				argv[i++] = xstrdup(val);
			}
		}
		if (ti->duration > 0)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%d", ti->duration);
			argv[i++] = xstrdup("duration");
			argv[i++] = xstrdup(buf);
		}
	}
	else if (stream_title)
	{
		argv[i++] = xstrdup("title");
		argv[i++] = xstrdup(stream_title);
	}

	return i;
}

static void spawn_status_program_inner(const char *status_text, struct track_info *ti)
{
	if (status_display_program == NULL || status_display_program[0] == 0)
		return;

	char *argv[32];
	int i = 0;

	argv[i++] = xstrdup(status_display_program);

	argv[i++] = xstrdup("status");
	argv[i++] = xstrdup(status_text);

	if (ti)
	{
		i = fill_status_program_track_info_args(argv, i, ti);
	}
	argv[i++] = NULL;

	if (spawn(argv, NULL, 0) == -1)
		error_msg("couldn't run `%s': %s", status_display_program, strerror(errno));
	for (i = 0; argv[i]; i++)
		free(argv[i]);
}

static void spawn_status_program(void)
{
	spawn_status_program_inner(player_status_names[player_info.status], player_info.ti);
}

static volatile sig_atomic_t ctrl_c_pressed = 0;

static void sig_int(int sig)
{
	ctrl_c_pressed = 1;
}

static void sig_shutdown(int sig)
{
	d_print("sig_shutdown %d\n", sig);
	cmus_running = 0;
}

static volatile sig_atomic_t needs_to_resize = 0;

static void sig_winch(int sig)
{
	needs_to_resize = 1;
}

void update_size(void)
{
	needs_to_resize = 1;
}

static int get_window_size(int *lines, int *columns)
{
	struct winsize ws;

	if (ioctl(0, TIOCGWINSZ, &ws) == -1)
		return -1;
	*columns = ws.ws_col;
	*lines = ws.ws_row;
	return 0;
}

static void resize_tree_view(int w, int h)
{
	tree_win_w = w * ((float)tree_width_percent / 100.0f);
	if (tree_width_max && tree_win_w > tree_width_max)
		tree_win_w = tree_width_max;
	/* at least one character of formatted text and one space either side */
	if (tree_win_w < 3)
		tree_win_w = 3;
	track_win_w = w - tree_win_w - 1;
	if (track_win_w < 3)
		track_win_w = 3;

	tree_win_x = 0;
	track_win_x = tree_win_w + 1;

	h--;
	window_set_nr_rows(lib_tree_win, h);
	window_set_nr_rows(lib_track_win, h);
}

static void update_window_size(void)
{
	int w, h;
	int columns, lines;

	if (get_window_size(&lines, &columns) == 0)
	{
		needs_to_resize = 0;
#if HAVE_RESIZETERM
		resizeterm(lines, columns);
#endif
		w = COLS;
		h = LINES - 3;
		if (w < 4)
			w = 4;
		if (h < 2)
			h = 2;
		win_w = w;
		resize_tree_view(w, h);
		window_set_nr_rows(lib_editable.shared->win, h - 1);
		pl_set_nr_rows(h - 1);
		window_set_nr_rows(pq_editable.shared->win, h - 1);
		window_set_nr_rows(filters_win, h - 1);
		window_set_nr_rows(help_win, h - 1);
		window_set_nr_rows(browser_win, h - 1);
	}
	clearok(curscr, TRUE);
	refresh();
}

static void update(void)
{
	static bool first_update = true;
	int needs_view_update = 0;
	int needs_title_update = 0;
	int needs_status_update = 0;
	int needs_command_update = 0;
	int needs_spawn = 0;

	if (first_update)
	{
		needs_title_update = 1;
		needs_command_update = 1;
		first_update = false;
	}

	if (needs_to_resize)
	{
		update_window_size();
		needs_title_update = 1;
		needs_status_update = 1;
		needs_command_update = 1;
	}

	if (player_info.status_changed)
		mpris_playback_status_changed();

	if (player_info.file_changed || player_info.metadata_changed)
		mpris_metadata_changed();

	needs_spawn = player_info.status_changed || player_info.file_changed ||
				  player_info.metadata_changed;

	if (player_info.file_changed)
	{
		needs_title_update = 1;
		needs_status_update = 1;
	}
	if (player_info.metadata_changed)
		needs_title_update = 1;
	if (player_info.position_changed || player_info.status_changed)
		needs_status_update = 1;
	switch (cur_view)
	{
	case TREE_VIEW:
		needs_view_update += lib_tree_win->changed || lib_track_win->changed;
		break;
	case SORTED_VIEW:
		needs_view_update += lib_editable.shared->win->changed;
		break;
	case PLAYLIST_VIEW:
		needs_view_update += pl_needs_redraw();
		break;
	case QUEUE_VIEW:
		needs_view_update += pq_editable.shared->win->changed;
		break;
	case BROWSER_VIEW:
		needs_view_update += browser_win->changed;
		break;
	case FILTERS_VIEW:
		needs_view_update += filters_win->changed;
		break;
	case HELP_VIEW:
		needs_view_update += help_win->changed;
		break;
	}

	/* total time changed? */
	if (cmus_queue_active())
	{
		needs_status_update += queue_needs_redraw();
	}
	else if (play_library)
	{
		needs_status_update += lib_editable.shared->win->changed;
		lib_editable.shared->win->changed = 0;
	}
	else
	{
		needs_status_update += pl_needs_redraw();
	}

	if (needs_spawn)
		spawn_status_program();

	if (needs_view_update || needs_title_update || needs_status_update || needs_command_update)
	{
		curs_set(0);

		if (needs_view_update)
			do_update_view(0);
		if (needs_title_update)
			do_update_titleline();
		if (needs_status_update)
			do_update_statusline();
		if (needs_command_update)
			do_update_commandline();
		post_update();
	}

	/* Reset changed flags */
	queue_post_update();
}

static void handle_ch(uchar ch)
{
	clear_error();
	if (input_mode == NORMAL_MODE)
	{
		if (!block_key_paste || !in_bracketed_paste)
		{
			normal_mode_ch(ch);
		}
	}
	else if (input_mode == COMMAND_MODE)
	{
		command_mode_ch(ch);
		update_commandline();
	}
	else if (input_mode == SEARCH_MODE)
	{
		search_mode_ch(ch);
		update_commandline();
	}
}

static void handle_csi(void)
{
	// after ESC[ until 0x40-0x7E (@A–Z[\]^_`a–z{|}~)
	// https://en.wikipedia.org/wiki/ANSI_escape_code#CSI_(Control_Sequence_Introducer)_sequences
	// https://www.ecma-international.org/wp-content/uploads/ECMA-48_5th_edition_june_1991.pdf

	int c;
	int buf[16]; // buffer a reasonable length
	size_t buf_n = 0;
	int overflow = 0;

	while (1)
	{
		c = getch();
		if (c == ERR || c == 0)
		{
			return;
		}
		if (buf_n < sizeof(buf) / sizeof(*buf))
		{
			buf[buf_n++] = c;
		}
		else
		{
			overflow = 1;
		}
		if (c >= 0x40 && c <= 0x7E)
		{
			break;
		}
	}

	if (overflow)
	{
		return;
	}

	if (buf_n == 4)
	{
		// bracketed paste
		// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Bracketed-Paste-Mode
		if (buf[0] == '2' && buf[1] == '0' && (buf[2] == '0' || buf[2] == '1') && buf[3] == '~')
		{
			in_bracketed_paste = buf[2] == '0';
			return;
		}
	}
}

static void handle_escape(int c)
{
	clear_error();
	if (input_mode == NORMAL_MODE)
	{
		normal_mode_ch(c + 128);
	}
	else if (input_mode == COMMAND_MODE)
	{
		command_mode_escape(c);
		update_commandline();
	}
	else if (input_mode == SEARCH_MODE)
	{
		search_mode_escape(c);
		update_commandline();
	}
}

static void handle_key(int key)
{
	clear_error();
	if (input_mode == NORMAL_MODE)
	{
		if (!block_key_paste || !in_bracketed_paste)
		{
			normal_mode_key(key);
		}
	}
	else if (input_mode == COMMAND_MODE)
	{
		command_mode_key(key);
		update_commandline();
	}
	else if (input_mode == SEARCH_MODE)
	{
		search_mode_key(key);
		update_commandline();
	}
}

static void handle_mouse(MEVENT *event)
{
#if NCURSES_MOUSE_VERSION <= 1
	static int last_mevent;

	if ((last_mevent & BUTTON1_PRESSED) && (event->bstate & REPORT_MOUSE_POSITION))
		event->bstate = BUTTON1_RELEASED;
	last_mevent = event->bstate;
#endif

	clear_error();
	if (input_mode == NORMAL_MODE)
	{
		normal_mode_mouse(event);
	}
	else if (input_mode == COMMAND_MODE)
	{
		command_mode_mouse(event);
		update_commandline();
	}
	else if (input_mode == SEARCH_MODE)
	{
		search_mode_mouse(event);
		update_commandline();
	}
}

static void u_getch(void)
{
	int key;
	int bit = 7;
	int mask = (1 << 7);
	uchar u, ch;

	key = getch();
	if (key == ERR || key == 0)
		return;

	if (key == KEY_MOUSE)
	{
		MEVENT event;
		if (getmouse(&event) == OK)
			handle_mouse(&event);
		return;
	}

	if (key > 255)
	{
		handle_key(key);
		return;
	}

	/* escape sequence */
	if (key == 0x1B)
	{
		cbreak();
		int e_key = getch();
		halfdelay(5);
		if (e_key != ERR)
		{
			if (e_key == '[')
				handle_csi();
			else if (e_key != 0)
				handle_escape(e_key);
			return;
		}
	}

	ch = (unsigned char)key;
	while (bit > 0 && ch & mask)
	{
		mask >>= 1;
		bit--;
	}
	if (bit == 7)
	{
		/* ascii */
		u = ch;
	}
	else if (using_utf8)
	{
		int count;

		u = ch & ((1 << bit) - 1);
		count = 6 - bit;
		while (count)
		{
			key = getch();
			if (key == ERR || key == 0)
				return;

			ch = (unsigned char)key;
			u = (u << 6) | (ch & 63);
			count--;
		}
	}
	else
		u = ch | U_INVALID_MASK;
	handle_ch(u);
}

static void main_loop(void)
{
	int rc, fd_high;

#define SELECT_ADD_FD(fd)   \
	do                      \
	{                       \
		FD_SET((fd), &set); \
		if ((fd) > fd_high) \
			fd_high = (fd); \
	} while (0)

	fd_high = server_socket;
	while (cmus_running)
	{
		fd_set set;
		struct timeval tv;
		int poll_mixer = 0;
		int i;
		int nr_fds_vol = 0, fds_vol[NR_MIXER_FDS];
		int nr_fds_out = 0, fds_out[NR_MIXER_FDS];
		struct list_head *item;
		struct client *client;

		player_info_snapshot();

		update();

		/* Timeout must be so small that screen updates seem instant.
		 * Only affects changes done in other threads (player).
		 *
		 * Too small timeout makes window updates too fast (wastes CPU).
		 *
		 * Too large timeout makes status line (position) updates too slow.
		 * The timeout is accuracy of player position.
		 */
		tv.tv_sec = 0;
		tv.tv_usec = 0;

		if (player_info.status == PLAYER_STATUS_PLAYING)
		{
			// player position updates need to be fast
			tv.tv_usec = 100e3;
		}

		FD_ZERO(&set);
		SELECT_ADD_FD(0);
		SELECT_ADD_FD(job_fd);
		SELECT_ADD_FD(cmus_next_track_request_fd);
		SELECT_ADD_FD(server_socket);
		if (mpris_fd != -1)
			SELECT_ADD_FD(mpris_fd);
		list_for_each_entry(client, &client_head, node)
		{
			SELECT_ADD_FD(client->fd);
		}
		if (!soft_vol)
		{
			nr_fds_vol = mixer_get_fds(MIXER_FDS_VOLUME, fds_vol);
			if (nr_fds_vol <= 0)
			{
				poll_mixer = 1;
				if (!tv.tv_usec)
					tv.tv_usec = 500e3;
			}
			for (i = 0; i < nr_fds_vol; i++)
			{
				BUG_ON(fds_vol[i] <= 0);
				SELECT_ADD_FD(fds_vol[i]);
			}
		}

		nr_fds_out = mixer_get_fds(MIXER_FDS_OUTPUT, fds_out);
		for (i = 0; i < nr_fds_out; i++)
		{
			BUG_ON(fds_out[i] <= 0);
			SELECT_ADD_FD(fds_out[i]);
		}

		rc = select(fd_high + 1, &set, NULL, NULL, tv.tv_usec ? &tv : NULL);
		if (poll_mixer)
		{
			int ol = volume_l;
			int or = volume_r;

			mixer_read_volume();
			if (ol != volume_l || or != volume_r)
			{
				mpris_volume_changed();
				update_statusline();
			}
		}
		if (rc <= 0)
		{
			if (ctrl_c_pressed)
			{
				handle_ch(0x03);
				ctrl_c_pressed = 0;
			}

			continue;
		}

		for (i = 0; i < nr_fds_vol; i++)
		{
			if (FD_ISSET(fds_vol[i], &set))
			{
				d_print("vol changed\n");
				mixer_read_volume();
				mpris_volume_changed();
				update_statusline();
			}
		}
		for (i = 0; i < nr_fds_out; i++)
		{
			if (FD_ISSET(fds_out[i], &set))
			{
				d_print("out changed\n");
				if (pause_on_output_change)
				{
					player_pause_playback();
					update_statusline();
				}
				clear_pipe(fds_out[i], -1);
			}
		}
		if (FD_ISSET(server_socket, &set))
			server_accept();

		// server_serve() can remove client from the list
		item = client_head.next;
		while (item != &client_head)
		{
			struct list_head *next = item->next;
			client = container_of(item, struct client, node);
			if (FD_ISSET(client->fd, &set))
				server_serve(client);
			item = next;
		}

		if (FD_ISSET(0, &set))
			u_getch();

		if (mpris_fd != -1 && FD_ISSET(mpris_fd, &set))
			mpris_process();

		if (FD_ISSET(job_fd, &set))
			job_handle();

		if (FD_ISSET(cmus_next_track_request_fd, &set))
			cmus_provide_next_track();
	}
}

static void init_curses(void)
{
	struct sigaction act;
	char *ptr, *term;

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_int;
	sigaction(SIGINT, &act, NULL);

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_shutdown;
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGTERM, &act, NULL);

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, NULL);

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sig_winch;
	sigaction(SIGWINCH, &act, NULL);

	initscr();
	nodelay(stdscr, TRUE);
	keypad(stdscr, TRUE);
	halfdelay(5);
	noecho();

	if (has_colors())
	{
#if HAVE_USE_DEFAULT_COLORS
		start_color();
		use_default_colors();
#endif
	}
	d_print("Number of supported colors: %d\n", COLORS);
	ui_initialized = 1;

	/* this was disabled while initializing because it needs to be
	 * called only once after all colors have been set
	 */
	update_colors();

	ptr = tcap_buffer;
	t_ts = tgetstr("ts", &ptr);
	t_fs = tgetstr("fs", &ptr);
	d_print("ts: %d fs: %d\n", !!t_ts, !!t_fs);

	if (!t_fs)
		t_ts = NULL;

	term = getenv("TERM");
	if (!t_ts && term)
	{
		/*
		 * Eterm:            Eterm
		 * aterm:            rxvt
		 * mlterm:           xterm
		 * terminal (xfce):  xterm
		 * urxvt:            rxvt-unicode
		 * xterm:            xterm, xterm-{,16,88,256}color
		 */
		if (!strcmp(term, "screen"))
		{
			t_ts = "\033_";
			t_fs = "\033\\";
		}
		else if (!strncmp(term, "xterm", 5) ||
				 !strncmp(term, "rxvt", 4) ||
				 !strcmp(term, "Eterm"))
		{
			/* \033]1;  change icon
			 * \033]2;  change title
			 * \033]0;  change both
			 */
			t_ts = "\033]0;";
			t_fs = "\007";
		}
	}
	update_mouse();

	if (!getenv("ESCDELAY"))
	{
		set_escdelay(default_esc_delay);
	}

	update_window_size();
}

static void init_all(void)
{
	main_thread = pthread_self();
	cmus_track_request_init();

	server_init(server_address);

	/* does not select output plugin */
	player_init();

	/* plugins have been loaded so we know what plugin options are available */
	options_add();

	/* cache the normalized env vars for pl_env */
	pl_env_init();

	lib_init();
	searchable = tree_searchable;
	cmus_init();
	pl_init();
	browser_init();
	filters_init();
	help_init();
	cmdline_init();
	commands_init();
	search_mode_init();

	/* almost everything must be initialized now */
	options_load();
	pl_init_options();
	if (mpris)
		mpris_init();

	/* finally we can set the output plugin */
	player_set_op(output_plugin);
	if (!soft_vol || pause_on_output_change)
		mixer_open();

	lib_autosave_filename = xstrjoin(cmus_config_dir, "/lib.pl");
	play_queue_autosave_filename = xstrjoin(cmus_config_dir, "/queue.pl");
	lib_filename = xstrdup(lib_autosave_filename);

	if (error_count)
	{
		char buf[16];
		char *ret;

		warn("Press <enter> to continue.");

		ret = fgets(buf, sizeof(buf), stdin);
		BUG_ON(ret == NULL);
	}
	help_add_all_unbound();

	init_curses();

	// enable bracketed paste (will be ignored if not supported)
	printf("\033[?2004h");
	fflush(stdout);

	if (resume_cmus)
	{
		resume_load();
		cmus_add(play_queue_append, play_queue_autosave_filename,
				 FILE_TYPE_PL, JOB_TYPE_QUEUE, 0, NULL);
	}
	else
	{
		set_view(start_view);
	}

	cmus_add(lib_add_track, lib_autosave_filename, FILE_TYPE_PL,
			 JOB_TYPE_LIB, 0, NULL);

	worker_start();
}

static void exit_all(void)
{
	endwin();

	// disable bracketed paste
	printf("\033[?2004l");
	fflush(stdout);

	if (resume_cmus)
		resume_exit();
	options_exit();

	server_exit();
	cmus_exit();
	if (resume_cmus)
		cmus_save(play_queue_for_each, play_queue_autosave_filename,
				  NULL);
	cmus_save(lib_for_each, lib_autosave_filename, NULL);

	pl_exit();
	player_exit();
	op_exit_plugins();
	commands_exit();
	search_mode_exit();
	filters_exit();
	help_exit();
	browser_exit();
	mpris_free();
}

enum
{
	FLAG_LISTEN,
	FLAG_PLUGINS,
	FLAG_SHOW_CURSOR,
	FLAG_HELP,
	FLAG_VERSION,
	NR_FLAGS
};

static struct option options[NR_FLAGS + 1] = {
	{0, "listen", 1},
	{0, "plugins", 0},
	{0, "show-cursor", 0},
	{0, "help", 0},
	{0, "version", 0},
	{0, NULL, 0}};

static const char *usage =
	"Usage: %s [OPTION]...\n"
	"Curses based music player.\n"
	"\n"
	"      --listen ADDR   listen on ADDR instead of $CMUS_SOCKET or $XDG_RUNTIME_DIR/cmus-socket\n"
	"                      ADDR is either a UNIX socket or host[:port]\n"
	"                      WARNING: using TCP/IP is insecure!\n"
	"      --plugins       list available plugins and exit\n"
	"      --show-cursor   always visible cursor\n"
	"      --help          display this help and exit\n"
	"      --version       " VERSION "\n"
	"\n"
	"Use cmus-remote to control cmus from command line.\n"
	"Report bugs to <cmus-devel@lists.sourceforge.net>.\n";

int main(int argc, char *argv[])
{
	int list_plugins = 0;

	program_name = argv[0];
	argv++;
	while (1)
	{
		int idx;
		char *arg;

		idx = get_option(&argv, options, &arg);
		if (idx < 0)
			break;

		switch (idx)
		{
		case FLAG_HELP:
			printf(usage, program_name);
			return 0;
		case FLAG_VERSION:
			printf("cmus " VERSION
				   "\nCopyright 2004-2006 Timo Hirvonen"
				   "\nCopyright 2008-2016 Various Authors\n");
			return 0;
		case FLAG_PLUGINS:
			list_plugins = 1;
			break;
		case FLAG_LISTEN:
			server_address = xstrdup(arg);
			break;
		case FLAG_SHOW_CURSOR:
			show_cursor = 1;
			break;
		}
	}

	setlocale(LC_CTYPE, "");
	setlocale(LC_COLLATE, "");
	charset = getenv("CMUS_CHARSET");
	if (!charset || !charset[0])
	{
#ifdef CODESET
		charset = nl_langinfo(CODESET);
#else
		charset = "ISO-8859-1";
#endif
	}
	if (strcmp(charset, "UTF-8") == 0)
		using_utf8 = 1;

	misc_init();
	if (server_address == NULL)
		server_address = xstrdup(cmus_socket_path);
	debug_init();
	d_print("charset = '%s'\n", charset);

	ip_load_plugins();
	op_load_plugins();
	if (list_plugins)
	{
		ip_dump_plugins();
		op_dump_plugins();
		return 0;
	}
	init_all();
	main_loop();
	exit_all();
	spawn_status_program_inner("exiting", NULL);
	return 0;
}

// 调试文件处理函数
static FILE *ui_debug_fp = NULL;

static void ui_debug_init(void)
{
	if (!ui_debug_fp)
	{
		ui_debug_fp = fopen("/tmp/cmus_ui_debug.log", "w");
		if (ui_debug_fp)
		{
			fprintf(ui_debug_fp, "===== CMUS UI DEBUG LOG STARTED =====\n");
			fflush(ui_debug_fp);
		}
	}
}

static void ui_debug_log(const char *format, ...)
{
	if (!ui_debug_fp)
	{
		ui_debug_init();
		if (!ui_debug_fp)
			return;
	}

	va_list ap;
	va_start(ap, format);
	vfprintf(ui_debug_fp, format, ap);
	va_end(ap);
	fflush(ui_debug_fp);
}

static void ui_debug_close(void)
{
	if (ui_debug_fp)
	{
		fprintf(ui_debug_fp, "===== CMUS UI DEBUG LOG CLOSED =====\n");
		fclose(ui_debug_fp);
		ui_debug_fp = NULL;
	}
}

void ui_curses_display_error_msg(const char *msg)
{
	if (msg)
	{
		ui_debug_log("ERROR: %s\n", msg);
		error_msg("%s", msg);
	}
}

void ui_debug_exit(void)
{
	ui_debug_close();
}
