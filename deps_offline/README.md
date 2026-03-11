# 离线依赖编译说明

本目录包含receiver_app所需的所有第三方库离线源码包。

## 依赖列表

| 库名称 | 版本 | 用途 | 文件 |
|--------|------|------|------|
| spdlog | 1.13.0 | 日志库 (header-only) | spdlog-1.13.0.tar.gz |
| yaml-cpp | 0.8.0 | YAML配置解析 | yaml-cpp-0.8.0.tar.gz |
| googletest | 1.14.0 | 单元测试框架 | googletest-1.14.0.tar.gz |

## 使用方式

### 方式1：使用CMake自动处理（推荐）

```bash
# 1. 解压所有依赖包
cd deps_offline
chmod +x extract_all.sh
./extract_all.sh

# 2. 使用离线模式编译
cd ..
cmake -B build -DFORCE_OFFLINE_BUILD=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 方式2：手动安装到系统

如果希望安装到系统（`~/.local`），可以使用build_deps.sh：

```bash
# 1. 将tar.gz文件复制到服务器的~/third_party/src/
mkdir -p ~/third_party/src
cp deps_offline/*.tar.gz ~/third_party/src/

# 2. 运行安装脚本
cd ~/third_party/src
bash /path/to/receiver_app/deps_offline/build_deps.sh

# 3. 编译项目时会自动找到系统安装的库
cd /path/to/receiver_app
cmake -B build -DUSE_LOCAL_DEPS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 方式3：WSL交叉编译

在WSL中交叉编译时，优先使用系统已安装的库，找不到再用离线源码：

```bash
# 先尝试使用系统库（WSL中可能已安装）
cmake -B build_wsl_arm64_cross \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake \
    -DFORCE_OFFLINE_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release

# 如果报错缺少依赖，解压离线包
cd deps_offline && ./extract_all.sh && cd ..

# 再次编译
cmake --build build_wsl_arm64_cross -j$(nproc)
```

## 三种依赖模式对比

| CMake选项 | 行为 | 适用场景 |
|-----------|------|----------|
| `-DUSE_LOCAL_DEPS=ON` (默认) | 优先find_package系统库，找不到则在线下载 | 开发环境 |
| `-DUSE_LOCAL_DEPS=OFF` | 强制使用FetchContent在线下载 | CI/CD环境 |
| `-DFORCE_OFFLINE_BUILD=ON` | 禁止在线下载，仅使用系统库或deps_offline | **生产环境（推荐）** |

## 依赖查找顺序（FORCE_OFFLINE_BUILD=ON）

```
1. find_package() 查找系统已安装的库
   ↓ (未找到)
2. 从 deps_offline/extracted/ 编译离线源码
   ↓ (也未找到)
3. 报错并提示手动解压或安装
```

## 常见问题

### Q1: 如何验证是否使用了离线源码？

编译时查看CMake输出：
- `✓ 使用系统已安装的 spdlog` → 使用系统库
- `⚙ 从离线源码编译 spdlog: ...` → 使用离线源码
- `⬇ 使用FetchContent在线下载 spdlog` → 在线下载（生产环境不应出现）

### Q2: 如何完全禁止在线下载？

```bash
# 方法1：CMake选项（推荐）
cmake -B build -DFORCE_OFFLINE_BUILD=ON

# 方法2：断网环境
# 直接在无外网连接的服务器上编译

# 方法3：修改CMakeLists.txt
# 删除或注释掉所有GIT_REPOSITORY行
```

### Q3: 离线源码包来源

所有tar.gz文件均从官方GitHub Releases下载：
- spdlog: https://github.com/gabime/spdlog/releases/tag/v1.13.0
- yaml-cpp: https://github.com/jbeder/yaml-cpp/releases/tag/0.8.0
- googletest: https://github.com/google/googletest/releases/tag/v1.14.0

### Q4: 如何更新依赖版本？

1. 下载新版本的tar.gz文件
2. 放入deps_offline目录
3. 修改CMakeLists.txt和tests/CMakeLists.txt中的版本号
4. 运行`./extract_all.sh`重新解压

## 目录结构

```
deps_offline/
├── README.md                      # 本文件
├── extract_all.sh                 # 一键解压脚本
├── build_deps.sh                  # 系统安装脚本
├── spdlog-1.13.0.tar.gz          # 离线源码包
├── yaml-cpp-0.8.0.tar.gz         # 离线源码包
├── googletest-1.14.0.tar.gz      # 离线源码包
└── extracted/                     # 解压目标目录
    ├── spdlog-1.13.0/
    ├── yaml-cpp-0.8.0/
    └── googletest-1.14.0/
```

## 验证完整性

```bash
# 验证tar.gz文件是否完整
cd deps_offline
md5sum *.tar.gz

# 验证解压后的目录
ls -la extracted/
```
