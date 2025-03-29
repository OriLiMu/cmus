#!/bin/bash

# 安装拼音索引所需的Python依赖

echo "开始安装拼音索引所需的Python依赖..."

# 确保pip已安装
if ! command -v pip3 &> /dev/null; then
    echo "请先安装pip3!"
    echo "例如: sudo apt install python3-pip (Debian/Ubuntu)"
    echo "或: sudo pacman -S python-pip (Arch Linux)"
    exit 1
fi

# 安装pypinyin
echo "安装pypinyin..."
pip3 install pypinyin

# 创建必要的目录
echo "创建~/.cmus目录..."
mkdir -p ~/.cmus

echo "依赖安装完成！"
echo ""
echo "使用方法:"
echo "1. 在cmus中，使用命令 ':pinyin-index 音乐目录路径' 生成拼音索引"
echo "   例如: :pinyin-index ~/Music"
echo ""
echo "2. 重启cmus以加载索引"
echo ""
echo "3. 现在你可以使用拼音首字母搜索中文歌曲了！"
echo "   例如: 对于'你好世界.mp3'，可以使用'nhsj'搜索" 