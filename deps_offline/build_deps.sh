#!/bin/bash
# ============================================================================
# Tower Receiver - 服务器端依赖编译安装脚本
# 环境要求: ARM64 Kylin Linux, GCC 7.3+, CMake 3.16+
# ============================================================================

set -e  # 遇错退出

INSTALL_PREFIX="${HOME}/.local"
BUILD_JOBS=$(nproc)

echo "=========================================="
echo "依赖库编译安装脚本"
echo "安装位置: $INSTALL_PREFIX"
echo "编译并发: $BUILD_JOBS 核心"
echo "=========================================="

# 检查必要工具
for tool in gcc g++ cmake make tar; do
    if ! command -v $tool &>/dev/null; then
        echo "[ERROR] 缺少必需工具: $tool"
        exit 1
    fi
done

# 显示环境信息
echo ""
echo "[INFO] 系统架构: $(uname -m)"
echo "[INFO] GCC 版本: $(gcc --version | head -1)"
echo "[INFO] CMake 版本: $(cmake --version | head -1)"
echo ""

# 创建构建目录
mkdir -p ~/third_party/build
cd ~/third_party/src

# ============================================================================
# 1. 编译 spdlog (Header-Only with Precompiled)
# ============================================================================
echo "----------------------------------------"
echo "[1/3] 编译 spdlog v1.13.0"
echo "----------------------------------------"

if [[ ! -d "spdlog-1.13.0" ]]; then
    echo "[INFO] 解压 spdlog-1.13.0.tar.gz..."
    tar xzf spdlog-1.13.0.tar.gz
fi

cd spdlog-1.13.0
mkdir -p build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -DSPDLOG_BUILD_SHARED=OFF \
      -DSPDLOG_BUILD_EXAMPLE=OFF \
      -DSPDLOG_BUILD_TESTS=OFF \
      ..

make -j${BUILD_JOBS}
make install

echo "[OK] spdlog 安装完成"
cd ~/third_party/src

# ============================================================================
# 2. 编译 yaml-cpp
# ============================================================================
echo ""
echo "----------------------------------------"
echo "[2/3] 编译 yaml-cpp v0.8.0"
echo "----------------------------------------"

if [[ ! -d "yaml-cpp-0.8.0" ]]; then
    echo "[INFO] 解压 yaml-cpp-0.8.0.tar.gz..."
    tar xzf yaml-cpp-0.8.0.tar.gz
fi

cd yaml-cpp-0.8.0
mkdir -p build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -DYAML_BUILD_SHARED_LIBS=OFF \
      -DYAML_CPP_BUILD_TESTS=OFF \
      -DYAML_CPP_BUILD_TOOLS=OFF \
      ..

make -j${BUILD_JOBS}
make install

echo "[OK] yaml-cpp 安装完成"
cd ~/third_party/src

# ============================================================================
# 3. 编译 googletest
# ============================================================================
echo ""
echo "----------------------------------------"
echo "[3/3] 编译 googletest v1.14.0"
echo "----------------------------------------"

if [[ ! -d "googletest-1.14.0" ]]; then
    echo "[INFO] 解压 googletest-1.14.0.tar.gz..."
    tar xzf googletest-1.14.0.tar.gz
fi

cd googletest-1.14.0
mkdir -p build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -DBUILD_GMOCK=ON \
      -DINSTALL_GTEST=ON \
      ..

make -j${BUILD_JOBS}
make install

echo "[OK] googletest 安装完成"
cd ~/third_party/src

# ============================================================================
# 完成
# ============================================================================
echo ""
echo "=========================================="
echo "所有依赖库安装完成！"
echo "=========================================="
echo ""
echo "安装位置:"
echo "  - 头文件: $INSTALL_PREFIX/include"
echo "  - 库文件: $INSTALL_PREFIX/lib"
echo ""
echo "使用方法:"
echo "  在 receiver_app 项目中构建时，CMake 会自动查找 ~/.local"
echo "  无需额外配置，find_package 将直接使用预安装的库。"
echo ""
echo "验证安装:"
echo "  ls -lh ~/.local/lib | grep -E '(spdlog|yaml|gtest)'"
echo "  ls -lh ~/.local/include | grep -E '(spdlog|yaml-cpp|gtest)'"
echo ""
