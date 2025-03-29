#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
拼音索引生成器 - 为cmus生成拼音首字母索引
"""

import os
import sys
import argparse
import json
import re
from pypinyin import pinyin, Style

def get_pinyin_initials(text):
    """
    获取文本的拼音首字母
    例如：'你好世界' -> 'nhsj'
    """
    if not text:
        return ""
    
    # 获取拼音首字母
    py_list = pinyin(text, style=Style.FIRST_LETTER)
    initials = ''.join([item[0] for item in py_list])
    return initials.lower()

def process_hybrid_text(text):
    """
    处理混合文本(中文和非中文)
    例如: "这是pine" -> "zspine"
    """
    result = []
    current_chinese = []
    current_non_chinese = []
    
    for char in text:
        # 检查是否为中文字符 (Unicode范围大致为中文字符的范围)
        if '\u4e00' <= char <= '\u9fff':
            # 如果之前有收集到非中文字符，先处理它们
            if current_non_chinese:
                result.append(''.join(current_non_chinese))
                current_non_chinese = []
            current_chinese.append(char)
        else:
            # 如果之前有收集到中文字符，先处理它们
            if current_chinese:
                chinese_text = ''.join(current_chinese)
                result.append(get_pinyin_initials(chinese_text))
                current_chinese = []
            # 保留非中文字符
            if not char.isspace():  # 忽略空白字符
                current_non_chinese.append(char)
    
    # 处理最后剩余的字符
    if current_chinese:
        chinese_text = ''.join(current_chinese)
        result.append(get_pinyin_initials(chinese_text))
    if current_non_chinese:
        result.append(''.join(current_non_chinese))
    
    return ''.join(result).lower()

def process_filename(filename):
    """
    处理文件名，提取拼音首字母
    返回原始文件名和对应的拼音首字母
    """
    basename = os.path.basename(filename)
    name, ext = os.path.splitext(basename)
    
    # 获取纯拼音首字母
    pure_py_initials = get_pinyin_initials(name)
    
    # 获取混合拼音处理结果
    hybrid_py_initials = process_hybrid_text(name)
    
    # 处理常见的艺术家-歌曲名分隔符
    parts_initials = []
    # 常见分隔符列表
    separators = ['-', '_', '–', '—', ' - ']
    
    for sep in separators:
        if sep in name:
            parts = name.split(sep, 1)  # 只分割一次，处理第一个分隔符
            for part in parts:
                part = part.strip()
                if part:
                    part_pinyin = get_pinyin_initials(part)
                    parts_initials.append(part_pinyin)
            break
    
    # 合并所有索引方式，用空格分隔，以便支持多种搜索方式
    all_initials = [pure_py_initials, hybrid_py_initials] + parts_initials
    combined_initials = " ".join(all_initials)
    
    return {
        "filename": filename,
        "basename": basename,
        "pinyin_initials": combined_initials
    }

def scan_music_directory(directory):
    """
    扫描音乐目录，生成拼音索引
    """
    result = []
    music_extensions = ('.mp3', '.flac', '.wav', '.ogg', '.m4a', '.ape', '.opus', '.wma')
    
    print(f"扫描目录 {directory} 中的音乐文件...")
    
    for root, dirs, files in os.walk(directory):
        for file in files:
            # 检查是否为音乐文件
            if file.lower().endswith(music_extensions):
                full_path = os.path.join(root, file)
                info = process_filename(full_path)
                result.append(info)
    
    return result

def save_index(data, output_file):
    """
    保存索引到文件
    """
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    
    print(f"索引已保存到 {output_file}，共 {len(data)} 个条目")

def main():
    parser = argparse.ArgumentParser(description='为cmus生成拼音首字母索引')
    parser.add_argument('directory', help='音乐文件目录')
    parser.add_argument('-o', '--output', default='~/.cmus/pinyin_index.json', help='输出文件路径 (默认: ~/.cmus/pinyin_index.json)')
    
    args = parser.parse_args()
    
    # 扩展~到用户主目录
    music_dir = os.path.expanduser(args.directory)
    output_file = os.path.expanduser(args.output)
    
    # 确保输出目录存在
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    
    print(f"扫描目录: {music_dir}")
    index_data = scan_music_directory(music_dir)
    save_index(index_data, output_file)

if __name__ == "__main__":
    main() 