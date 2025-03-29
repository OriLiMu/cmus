/*
 * 拼音首字母搜索 - Pinyin Initial Letter Search for cmus
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

#ifndef CMUS_PINYIN_SEARCH_H
#define CMUS_PINYIN_SEARCH_H

/**
 * 加载拼音索引文件
 * 从 ~/.cmus/pinyin_index.json 中加载拼音索引
 */
void pinyin_load_index(void);

/**
 * 使用拼音首字母搜索文件路径
 * @param filename 要匹配的文件路径
 * @param query 拼音搜索查询字符串
 * @return 如果找到匹配，返回1，否则返回0
 */
int pinyin_search_match(const char *filename, const char *query);

/**
 * 释放拼音索引资源
 */
void pinyin_free_index(void);

#endif /* CMUS_PINYIN_SEARCH_H */