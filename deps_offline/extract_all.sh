#!/bin/bash
# ============================================================================
# Tower Receiver - 离线依赖解压脚本
# 功能：解压deps_offline目录下的tar.gz文件到extracted目录
# ============================================================================

set -e  # 遇错退出

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="$SCRIPT_DIR"
EXTRACTED_DIR="$DEPS_DIR/extracted"

echo "=========================================="
echo "离线依赖解压脚本"
echo "=========================================="
echo "依赖目录: $DEPS_DIR"
echo "解压目标: $EXTRACTED_DIR"
echo ""

# 创建extracted目录
mkdir -p "$EXTRACTED_DIR"

# 检查并解压文件的函数
extract_if_needed() {
    local tarball=$1
    local dirname=$2

    if [ ! -f "$DEPS_DIR/$tarball" ]; then
        echo "[跳过] $tarball 不存在"
        return
    fi

    if [ -d "$EXTRACTED_DIR/$dirname" ]; then
        echo "[跳过] $dirname 已解压"
        return
    fi

    echo "[解压] $tarball ..."
    tar xzf "$DEPS_DIR/$tarball" -C "$EXTRACTED_DIR"
    echo "  ✓ 完成: $EXTRACTED_DIR/$dirname"
}

# 解压所有依赖包
extract_if_needed "spdlog-1.13.0.tar.gz" "spdlog-1.13.0"
extract_if_needed "yaml-cpp-0.8.0.tar.gz" "yaml-cpp-0.8.0"
extract_if_needed "googletest-1.14.0.tar.gz" "googletest-1.14.0"

echo ""
echo "=========================================="
echo "解压完成！"
echo "=========================================="
echo ""
echo "已解压的目录："
ls -la "$EXTRACTED_DIR"
echo ""
echo "使用方法："
echo "  cmake -B build -DFORCE_OFFLINE_BUILD=ON"
echo "  cmake --build build"
