#!/bin/bash
# 用于在合适的终端中启动cmus的调试包装脚本

# 检测可用的终端模拟器
if command -v xterm &>/dev/null; then
    TERM_CMD="xterm"
elif command -v gnome-terminal &>/dev/null; then
    TERM_CMD="gnome-terminal --"
elif command -v konsole &>/dev/null; then
    TERM_CMD="konsole -e"
elif command -v alacritty &>/dev/null; then
    TERM_CMD="alacritty -e"
elif command -v kitty &>/dev/null; then
    TERM_CMD="kitty"
else
    echo "找不到支持的终端模拟器。请安装xterm、gnome-terminal、konsole、alacritty或kitty。"
    exit 1
fi

# 使用检测到的终端模拟器启动cmus
exec $TERM_CMD /home/lizhe/CodeOri/cmus/cmus "$@" 