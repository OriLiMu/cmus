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

#ifndef CMUS_UI_CURSES_H
#define CMUS_UI_CURSES_H

#include "search.h"
#include "compiler.h"
#include "format_print.h"

enum ui_input_mode
{
	NORMAL_MODE,
	COMMAND_MODE,
	SEARCH_MODE
};

enum ui_query_answer
{
	UI_QUERY_ANSWER_ERROR = -1,
	UI_QUERY_ANSWER_NO = 0,
	UI_QUERY_ANSWER_YES = 1
};

#include <signal.h>

extern volatile sig_atomic_t cmus_running;
extern int ui_initialized;
extern enum ui_input_mode input_mode;
extern int cur_view;
extern int prev_view;
extern struct searchable *searchable;

extern char *lib_filename;
extern char *lib_ext_filename;
extern char *pl_filename;
extern char *play_queue_filename;
extern char *play_queue_ext_filename;

extern char *charset;
extern int using_utf8;

void update_titleline(void);
void update_statusline(void);
void update_filterline(void);
void update_colors(void);
void update_full(void);
void update_size(void);
void info_msg(const char *format, ...) CMUS_FORMAT(1, 2);
void error_msg(const char *format, ...) CMUS_FORMAT(1, 2);
enum ui_query_answer yes_no_query(const char *format, ...) CMUS_FORMAT(1, 2);
void search_not_found(void);
void set_view(int view);
void set_client_fd(int fd);
int get_client_fd(void);
void enter_command_mode(void);
void enter_search_mode(void);
void enter_search_backward_mode(void);

int track_format_valid(const char *format);

/* lock player_info ! */
const char *get_stream_title(void);
const struct format_option *get_global_fopts(void);

int get_track_win_x(void);

void ui_curses_display_error_msg(const char *msg);

void ui_debug_exit(void);

#endif
