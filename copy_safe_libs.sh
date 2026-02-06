#!/bin/bash

EXECUTABLE="$1"
TARGET_LIB_DIR="$2"

if [ -z "$EXECUTABLE" ] || [ -z "$TARGET_LIB_DIR" ]; then
    echo "Usage: $0 <executable> <target_lib_directory>"
    exit 1
fi

mkdir -p "$TARGET_LIB_DIR"

# 系统基础库列表（不应该打包的）
SYSTEM_LIBS=(
    "libc.so"
    "libm.so"
    "libpthread.so"
    "libdl.so"
    "librt.so"
    "libstdc++.so"
    "libgcc_s.so"
    "ld-linux"
    "libz.so"
    "libresolv.so"
    "libnss_"
    "libselinux.so"
    "libpcre"
    "libglib-"
    "libgobject-"
    "libgio-"
)

echo "分析依赖: $EXECUTABLE"
echo "================================"

# 获取所有依赖
ldd "$EXECUTABLE" | grep "=>" | awk '{print $3}' | while read -r lib; do
    if [ -z "$lib" ] || [ "$lib" == "" ]; then
        continue
    fi
    
    # 检查是否是系统库
    is_system=0
    for sys_lib in "${SYSTEM_LIBS[@]}"; do
        if [[ "$lib" == *"$sys_lib"* ]]; then
            is_system=1
            echo "跳过系统库: $lib"
            break
        fi
    done
    
    # 如果不是系统库，复制它
    if [ $is_system -eq 0 ]; then
        echo "复制应用库: $lib"
        cp -L "$lib" "$TARGET_LIB_DIR/"
    fi
done

echo "================================"
echo "依赖库已复制到: $TARGET_LIB_DIR"
