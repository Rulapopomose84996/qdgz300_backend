# Tower Receiver App: WSL 离线交叉编译指南

本文档指导如何在 **Windows WSL 环境 (Ubuntu)** 下，将 `receiver_app` 项目交叉编译为 **ARM64 (aarch64)** 架构的可执行文件。

适用于：没有公网权限、需要完全离线编译、且希望操作尽可能简单的开发场景。

---

## 1. 准备工作 (仅需一次)

### 1.1 安装交叉编译工具链
进入 WSL 终端，安装必要的构建工具和 ARM64 交叉编译器：
```bash
sudo apt update
sudo apt install -y cmake build-essential gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

### 1.2 预安装离线依赖库 (One-Time Setup)
为了避免每次编译都要解压和构建依赖，我们将 `spdlog`、`yaml-cpp` 和 `googletest` 预编译并安装到 WSL 系统目录 `~/.local/arm64`。

请在项目根目录下，直接复制粘贴执行以下脚本块：

```bash
# 1. 清理并解压源码
find /mnt/d/Workspace/Company/Tower/qdgz300_backend -type d -name "deps_offline"

# 2. 编译安装 spdlog
cd extracted/spdlog-1.13.0
cmake -B build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchains/aarch64-linux-gnu.cmake \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/arm64
cmake --build build_arm64 --target install -j

# 3. 编译安装 yaml-cpp
cd ../yaml-cpp-0.8.0
cmake -B build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchains/aarch64-linux-gnu.cmake \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/arm64 \
    -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF
cmake --build build_arm64 --target install -j

# 4. 编译安装 googletest
cd ../googletest-1.14.0
cmake -B build_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchains/aarch64-linux-gnu.cmake \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/arm64 \
    -DINSTALL_GTEST=ON -DBUILD_GMOCK=ON
cmake --build build_arm64 --target install -j

# 5. 返回根目录
cd ../../../
```

### 1.3 配置环境变量快捷方式 (Optional但推荐)
为了让 CMake 自动找到这些库，并且简化交叉编译命令，执行：

```bash
# 设置查找路径
echo 'export CMAKE_PREFIX_PATH="$HOME/.local/arm64:$CMAKE_PREFIX_PATH"' >> ~/.bashrc

# 设置快捷别名 (假设项目结构固定)
# 注意：此别名依赖于你在项目根目录下执行
echo "alias cmake-arm='cmake -DCMAKE_TOOLCHAIN_FILE=\$(pwd)/cmake/toolchains/aarch64-linux-gnu.cmake'" >> ~/.bashrc

# 立即生效
source ~/.bashrc
```

---

## 2. 日常编译流程

完成上述准备后，以后的编译将变得极其简单：

### Step 1: 创建构建目录
```bash
# 在项目根目录
mkdir -p build_wsl_cross
cd build_wsl_cross
```

### Step 2: 生成构建配置
使用快捷别名（如果你配置了 1.3）：
```bash
cmake-arm ..
```
或者手动输入完整命令：
```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/aarch64-linux-gnu.cmake
```

### Step 3: 执行编译
```bash
make -j$(nproc)
```

---

## 3. 产物验证

编译成功后，在 `build_wsl_cross` 目录下会生成：
*   `receiver_app` (主程序)
*   `pcap_replay`
*   `pcap_analyze`

验证架构是否正确：
```bash
file receiver_app
# 输出应包含: ELF 64-bit LSB executable, ARM aarch64
```

---

## 4. 常见问题 FAQ

**Q: 我换了台电脑怎么编译？**
A: 新电脑需要重新执行 **1.1** 和 **1.2** 步骤来安装编译器和依赖库。

**Q: 如何在远程 ARM 服务器上编译？**
A: 在真实 ARM 服务器上不需要交叉编译工具链。直接运行标准命令即可：
`cmake -B build && cmake --build build -j` (CMake 会自动使用系统安装的库)。
