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

#include "pinyin_search.h"
#include "xmalloc.h"
#include "debug.h"
#include "path.h"
#include "utils.h"
#include "file.h"
#include "gbuf.h"
#include "uchar.h"
#include "keyval.h"
#include "comment.h"
#include "xstrjoin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>

static pthread_mutex_t pinyin_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct growing_keyvals pinyin_index;
static int pinyin_index_loaded = 0;

// 从JSON文件中解析一行，简单的解析实现
static char *parse_json_line(char *line, const char *key, int key_len)
{
    if (!line || !key || key_len <= 0)
    {
        fprintf(stderr, "DEBUG: parse_json_line - invalid parameters\n");
        return NULL;
    }

    char *start = strstr(line, key);
    if (!start)
    {
        fprintf(stderr, "DEBUG: parse_json_line - key '%s' not found in line\n", key);
        return NULL;
    }

    start += key_len;
    // 跳过":"和空白字符
    while (start && *start && (*start == ':' || *start == ' ' || *start == '"'))
    {
        start++;
    }

    if (!start || !*start)
    {
        fprintf(stderr, "DEBUG: parse_json_line - no value after key '%s'\n", key);
        return NULL;
    }

    // 找到值的结尾（引号或逗号或大括号）
    char *end = start;
    while (end && *end && *end != '"' && *end != ',' && *end != '}')
    {
        end++;
    }

    if (end == start)
    {
        fprintf(stderr, "DEBUG: parse_json_line - empty value for key '%s'\n", key);
        return NULL;
    }

    // 创建一个新的字符串
    int len = end - start;
    char *value = xnew(char, len + 1);
    if (!value)
    {
        fprintf(stderr, "DEBUG: parse_json_line - memory allocation failed\n");
        return NULL;
    }

    memcpy(value, start, len);
    value[len] = '\0';
    fprintf(stderr, "DEBUG: parse_json_line - parsed value: '%s' for key '%s'\n", value, key);

    return value;
}

// 创建空的索引文件
static void create_empty_index_file(const char *index_path)
{
    if (!index_path)
        return;

    // 首先检查文件是否已存在
    if (access(index_path, F_OK) == 0)
        return; // 文件已存在，不需要创建

    // 检查目录是否存在，不存在则创建
    char *dir_path = xstrjoin(getenv("HOME"), "/.cmus");
    if (access(dir_path, F_OK) != 0)
    {
        fprintf(stderr, "DEBUG: Creating .cmus directory\n");
        mkdir(dir_path, 0755);
    }
    free(dir_path);

    // 创建一个空的JSON数组文件
    FILE *fp = fopen(index_path, "w");
    if (fp)
    {
        fprintf(stderr, "DEBUG: Creating empty pinyin index file: %s\n", index_path);
        fputs("[]", fp); // 写入一个空的JSON数组
        fclose(fp);
    }
    else
    {
        fprintf(stderr, "DEBUG: Failed to create empty pinyin index file: %s\n", index_path);
    }
}

// 加载拼音索引文件
void pinyin_load_index(void)
{
    pthread_mutex_lock(&pinyin_mutex);

    if (pinyin_index_loaded)
    {
        pthread_mutex_unlock(&pinyin_mutex);
        return;
    }

    pinyin_index.keyvals = NULL;
    pinyin_index.alloc = 0;
    pinyin_index.count = 0;

    char *index_path = xstrjoin(getenv("HOME"), "/.cmus/pinyin_index.json");
    fprintf(stderr, "DEBUG: Trying to load pinyin index from: %s\n", index_path);

    // 如果索引文件不存在，创建一个空的
    create_empty_index_file(index_path);

    // 检查文件是否存在
    if (access(index_path, F_OK) == -1)
    {
        fprintf(stderr, "DEBUG: Pinyin index file not found: %s\n", index_path);
        free(index_path);
        pthread_mutex_unlock(&pinyin_mutex);
        return;
    }

    // 读取索引文件
    size_t size;
    char *buf = mmap_file(index_path, &size);

    // 检查mmap是否成功
    if (buf == NULL || size == -1)
    {
        fprintf(stderr, "DEBUG: Could not mmap pinyin index file: %s\n", index_path);
        free(index_path);
        pthread_mutex_unlock(&pinyin_mutex);
        return;
    }

    // 如果文件大小为0，则提前返回
    if (size == 0)
    {
        fprintf(stderr, "DEBUG: Pinyin index file is empty: %s\n", index_path);
        // 不需要munmap，因为空文件没有映射内存
        free(index_path);
        pthread_mutex_unlock(&pinyin_mutex);
        return;
    }

    fprintf(stderr, "DEBUG: Loading pinyin index from %s, size: %zu\n", index_path, size);

    char *line, *next;
    line = buf;

    int in_file_object = 0;
    char *current_filename = NULL;
    char *current_pinyin = NULL;
    char *current_original_name = NULL;

    // 简单解析JSON数组
    // 实际上json格式应该用专门的库来解析，这里为了简单只做基本处理
    while (line && *line && line < buf + size) // 添加边界检查
    {
        // 找到下一行
        next = strchr(line, '\n');
        if (next)
        {
            if (next >= buf + size)
            { // 确保不超出缓冲区
                break;
            }
            *next = '\0';
            next++;
        }
        else
        {
            // 如果没有找到换行符，则指向缓冲区末尾
            next = buf + size;
        }

        // 检查是否开始一个新对象
        if (strstr(line, "{"))
        {
            in_file_object = 1;
            free(current_filename);
            free(current_pinyin);
            free(current_original_name);
            current_filename = NULL;
            current_pinyin = NULL;
            current_original_name = NULL;
        }

        // 解析键值对
        if (in_file_object)
        {
            if (!current_filename && strstr(line, "\"filename\""))
            {
                current_filename = parse_json_line(line, "\"filename\"", 10);
            }

            if (!current_pinyin && strstr(line, "\"pinyin_initials\""))
            {
                current_pinyin = parse_json_line(line, "\"pinyin_initials\"", 17);
            }

            if (!current_original_name && strstr(line, "\"basename\""))
            {
                current_original_name = parse_json_line(line, "\"basename\"", 10);
            }
        }

        // 检查是否结束一个对象
        if (strstr(line, "}") && in_file_object)
        {
            in_file_object = 0;

            // 如果同时有文件名和拼音首字母，添加到索引
            if (current_filename && current_pinyin)
            {
                keyvals_add(&pinyin_index, current_filename, current_pinyin);
                fprintf(stderr, "DEBUG: Added pinyin index: %s -> %s\n", current_filename, current_pinyin);
            }

            free(current_filename);
            free(current_pinyin);
            free(current_original_name);
            current_filename = NULL;
            current_pinyin = NULL;
            current_original_name = NULL;
        }

        line = next;
        if (line >= buf + size)
        {
            break; // 确保不超出缓冲区
        }
    }

    // 清理
    free(current_filename);
    free(current_pinyin);
    free(current_original_name);
    munmap(buf, size);
    free(index_path);

    pinyin_index_loaded = 1;
    fprintf(stderr, "DEBUG: Pinyin index loaded, %d entries\n", pinyin_index.count);

    pthread_mutex_unlock(&pinyin_mutex);
}

// 使用拼音首字母搜索文件路径
// 如果找到匹配，返回1，否则返回0
int pinyin_search_match(const char *filename, const char *query)
{
    fprintf(stderr, "DEBUG: pinyin_search_match called with filename=%s, query=%s\n",
            filename ? filename : "NULL",
            query ? query : "NULL");

    // 参数验证
    if (!query || !filename || !query[0])
    {
        fprintf(stderr, "DEBUG: pinyin_search_match - invalid parameters\n");
        return 0;
    }

    if (!pinyin_index_loaded)
    {
        fprintf(stderr, "DEBUG: pinyin index not loaded, trying to load now\n");
        // 延迟加载索引
        pinyin_load_index();
    }

    if (!pinyin_index_loaded || !pinyin_index.keyvals || pinyin_index.count <= 0)
    {
        fprintf(stderr, "DEBUG: pinyin index still not loaded or invalid after attempt\n");
        return 0;
    }

    fprintf(stderr, "DEBUG: pinyin_search_match - acquiring mutex\n");
    pthread_mutex_lock(&pinyin_mutex);
    fprintf(stderr, "DEBUG: pinyin_search_match - mutex acquired\n");

    // 检查索引是否有效
    if (!pinyin_index.keyvals || pinyin_index.count <= 0)
    {
        fprintf(stderr, "DEBUG: pinyin_search_match - index is invalid inside mutex\n");
        pthread_mutex_unlock(&pinyin_mutex);
        return 0;
    }

    // 查找对应的拼音首字母
    fprintf(stderr, "DEBUG: pinyin_search_match - searching in %d entries\n", pinyin_index.count);
    int result = 0;

    for (int i = 0; i < pinyin_index.count; i++)
    {
        if (pinyin_index.keyvals[i].key == NULL)
        {
            fprintf(stderr, "DEBUG: pinyin_search_match - found NULL key at index %d\n", i);
            continue;
        }

        fprintf(stderr, "DEBUG: pinyin_search_match - comparing '%s' with '%s'\n",
                pinyin_index.keyvals[i].key, filename);

        if (strcmp(pinyin_index.keyvals[i].key, filename) == 0)
        {
            const char *pinyin = pinyin_index.keyvals[i].val;
            fprintf(stderr, "DEBUG: pinyin_search_match - found matching key, pinyin=%s\n",
                    pinyin ? pinyin : "NULL");

            // 检查是否匹配
            if (pinyin && u_strcasestr_base(pinyin, query))
            {
                fprintf(stderr, "DEBUG: pinyin_search_match - found match, returning 1\n");
                result = 1;
            }
            else
            {
                fprintf(stderr, "DEBUG: pinyin_search_match - no match for query\n");
            }
            break;
        }
    }

    fprintf(stderr, "DEBUG: pinyin_search_match - releasing mutex\n");
    pthread_mutex_unlock(&pinyin_mutex);
    fprintf(stderr, "DEBUG: pinyin_search_match - returning %d\n", result);
    return result;
}

// 释放拼音索引资源
void pinyin_free_index(void)
{
    pthread_mutex_lock(&pinyin_mutex);

    if (pinyin_index_loaded)
    {
        keyvals_free(pinyin_index.keyvals);
        pinyin_index.keyvals = NULL;
        pinyin_index.alloc = 0;
        pinyin_index.count = 0;
        pinyin_index_loaded = 0;
    }

    pthread_mutex_unlock(&pinyin_mutex);
}