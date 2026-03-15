# 共享第三方缓存重构指南（Windows / WSL / 服务器）

**适用对象**: 需要同时支持 Windows 开发、WSL 交叉编译、麒麟 V10 ARM64 服务器原生编译的 CMake 项目  
**目标**: 将“仓库内 `deps_offline/` 离线包模式”重构为“共享离线包目录 + 共享依赖缓存目录”模式，便于多个项目复用、降低仓库体积、减少重复构建

---

## 1. 文档目的

本文档总结了 `rma_manager` 当前已经验证通过的共享第三方缓存方案，供其他项目做同类重构时直接参考。

本方案重点解决四类问题：

1. 离线源码包长期堆在项目仓库里，导致仓库膨胀
2. 同一台机器上多个项目重复准备相同依赖，浪费时间
3. Windows / WSL / 服务器三端路径不统一，脚本难维护
4. 老环境 CMake / 线程库 / 多架构安装目录 / Debug 库名等兼容性问题频发

---

## 2. 最终目标形态

### 2.1 Windows 侧目录约定

项目源码目录：

```text
D:\Workspace\Company\Tower\rma_manager
```

共享离线源码包目录：

```text
D:\WorkSpace\ThirdPartyCache\rma_manager\archives
```

WSL 交叉构建共享依赖缓存目录：

```text
D:\WorkSpace\ThirdPartyCache\rma_manager\build\wsl-aarch64
```

### 2.2 WSL 侧目录约定

项目源码目录：

```text
/mnt/d/Workspace/Company/Tower/rma_manager
```

共享离线源码包目录：

```text
/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/archives
```

WSL 交叉构建共享依赖缓存目录：

```text
/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/build/wsl-aarch64
```

### 2.3 麒麟服务器侧目录约定

项目源码目录：

```text
/home/devuser/WorkSpace/rma_manager
```

共享离线源码包目录：

```text
/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/archives
```

服务器原生构建共享依赖缓存目录：

```text
/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/build/native-aarch64
```

---

## 3. 推荐目录结构

共享缓存根目录建议按项目单独隔离：

```text
ThirdPartyCache/
└── rma_manager/
    ├── archives/
    │   ├── yaml-cpp-0.8.0.tar.gz
    │   ├── nlohmann-json-3.11.3.tar.gz
    │   ├── spdlog-1.13.0.tar.gz
    │   ├── sqlite-amalgamation-3450000.tar.gz
    │   ├── Crow-1.2.0.tar.gz
    │   ├── asio-1.30.2.tar.gz
    │   └── googletest-1.14.0.tar.gz
    └── build/
        ├── native-aarch64/
        │   ├── src/
        │   ├── build/
        │   └── prefix/
        └── wsl-aarch64/
            ├── src/
            ├── build/
            └── prefix/
```

含义如下：

- `archives/`: 只放离线源码包，作为唯一离线来源
- `build/<platform>/src/`: 解压后的第三方源码
- `build/<platform>/build/`: 第三方依赖自身的构建中间产物
- `build/<platform>/prefix/`: 项目构建真正复用的头文件和静态库前缀目录

---

## 4. 重构原则

### 4.1 路径分层

必须把第三方依赖路径拆成两层：

1. `RMA_OFFLINE_DEPS_DIR`
   作用：指向共享离线源码包目录

2. `DEPS_ROOT`
   作用：指向共享依赖缓存根目录

不要把这两个目录混用。

### 4.2 仓库内 `deps_offline/` 不再作为默认值

重构完成后：

- 构建脚本默认应直接使用共享缓存目录
- 仓库内 `deps_offline/` 只能视为历史遗留或人工兜底材料
- 文档不能继续引导使用仓库内 `deps_offline/`

### 4.3 CMake 层和脚本层必须同步改

只改脚本不改 CMake 会失败，因为 CMake 仍可能继续寻找项目内旧路径。

必须同时修改：

- 构建脚本
- 依赖准备脚本
- `third_party/CMakeLists.txt`
- 部署与构建文档

### 4.4 共享缓存必须平台隔离

不要把服务器原生缓存和 WSL 交叉缓存混在同一个目录。

建议至少分为：

- `build/native-aarch64`
- `build/wsl-aarch64`

原因：

- 编译器不同
- 构建类型可能不同
- 安装目录结构可能不同
- 交叉工具链产物不能和原生产物混用

---

## 5. 实施步骤

### 5.1 第一步：建立共享离线源码包目录

将原项目中的离线包迁移到共享目录。

Windows 侧目录：

```text
D:\WorkSpace\ThirdPartyCache\rma_manager\archives
```

服务器侧目录：

```text
/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/archives
```

至少需要保证需要的压缩包已经全部存在。

### 5.2 第二步：新增共享依赖准备脚本

建议新增两个脚本：

- `scripts/prepare_native_deps.sh`
- `scripts/prepare_wsl_cross_deps.sh`

这两个脚本的职责不是构建项目本身，而是只做第三方缓存准备：

1. 从 `archives/` 解压依赖源码
2. 编译必要的第三方静态库
3. 将头文件和静态库整理到 `prefix/`
4. 让项目后续构建只依赖 `prefix/`

### 5.3 第三步：主构建脚本先准备缓存，再构建项目

例如：

- 服务器原生：`scripts/build_production.sh`
- WSL 交叉：`scripts/dev_build_wsl_cross.sh`

推荐流程固定为：

1. 检查编译环境
2. 检查 `archives/`
3. 运行 `prepare_*_deps.sh`
4. 把 `RMA_OFFLINE_DEPS_DIR` 和 `RMA_DEPS_PREFIX` 传给 CMake
5. 构建项目
6. 运行测试

### 5.4 第四步：CMake 优先使用共享 `prefix`

推荐在 `third_party/CMakeLists.txt` 中引入两个缓存变量：

```cmake
set(RMA_OFFLINE_DEPS_DIR "" CACHE PATH "Shared offline dependency archive directory")
set(RMA_DEPS_PREFIX "" CACHE PATH "Shared dependency prefix directory")
```

行为建议：

1. 如果 `RMA_DEPS_PREFIX` 非空，则优先从 `prefix/` 导入目标
2. 如果 `RMA_DEPS_PREFIX` 为空，再回落到离线 `FetchContent`
3. 必须显式报错缺失的头文件或静态库，不能静默在线下载

### 5.5 第五步：文档同步改造

至少要改三类文档：

1. README
2. 部署基线文档
3. 给团队复用的重构指导文档

否则后续使用者仍会按照旧流程把离线包放回仓库。

---

## 6. 环境变量设计建议

### 6.1 服务器原生构建

推荐默认值：

```bash
RMA_OFFLINE_DEPS_DIR=/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/archives
DEPS_ROOT=/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/build/native-aarch64
```

### 6.2 WSL 交叉构建

推荐默认值：

```bash
RMA_OFFLINE_DEPS_DIR=/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/archives
DEPS_ROOT=/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/build/wsl-aarch64
```

### 6.3 设计原则

- `RMA_OFFLINE_DEPS_DIR` 表示离线源码包目录
- `DEPS_ROOT` 表示共享缓存工作目录
- `RMA_DEPS_PREFIX` 由主构建脚本内部推导为 `${DEPS_ROOT}/prefix`

---

## 7. 推荐脚本行为

### 7.1 服务器原生构建脚本

推荐入口：

在服务器目录：`/home/devuser/WorkSpace/rma_manager`

```bash
cd /home/devuser/WorkSpace/rma_manager
BUILD_SERVICE=ON BUILD_TESTS=OFF RUN_TESTS=OFF bash scripts/build_production.sh
```

推荐脚本内部默认值：

```bash
DEFAULT_SHARED_OFFLINE_DEPS_DIR="/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/archives"
DEPS_ROOT="${DEPS_ROOT:-/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/build/native-aarch64}"
OFFLINE_DEPS_DIR="${RMA_OFFLINE_DEPS_DIR:-${DEFAULT_SHARED_OFFLINE_DEPS_DIR}}"
```

### 7.2 WSL 交叉构建脚本

推荐入口：

在 WSL 目录：`/mnt/d/Workspace/Company/Tower/rma_manager`

```bash
cd /mnt/d/Workspace/Company/Tower/rma_manager
BUILD_SERVICE=ON BUILD_TESTS=OFF RUN_TESTS=OFF bash scripts/dev_build_wsl_cross.sh
```

推荐脚本内部默认值：

```bash
DEFAULT_SHARED_OFFLINE_DEPS_DIR="/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/archives"
DEPS_ROOT="${DEPS_ROOT:-/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/build/wsl-aarch64}"
OFFLINE_DEPS_DIR="${RMA_OFFLINE_DEPS_DIR:-${DEFAULT_SHARED_OFFLINE_DEPS_DIR}}"
```

---

## 8. 推荐的依赖准备策略

### 8.1 适合放进共享 `prefix/` 的依赖

优先放这些：

- `yaml-cpp`
- `sqlite-amalgamation` 产出的静态库
- `nlohmann-json` 头文件
- `spdlog` 头文件
- `asio` 头文件
- `crow` 头文件

### 8.2 测试依赖的处理

`googletest` 推荐做法不是直接装库，而是：

- 解压到 `${DEPS_ROOT}/src/googletest`
- 构建项目时通过 `add_subdirectory(...)` 引入

这样更稳定，也避免不同编译选项导致 ABI 不一致。

---

## 9. 关键兼容性问题与解决方案

### 9.1 `FindThreads` 在麒麟老环境误判

现象：

- `find_package(Threads)` 最终错误链接到 `-lpthreads`
- 实际系统只有 `-pthread` 或 `-lpthread`

推荐做法：

- 不依赖 `Threads::Threads`
- 自己定义接口目标，例如 `rma_threads`
- Linux 下统一加：

```cmake
target_compile_options(rma_threads INTERFACE -pthread)
target_link_options(rma_threads INTERFACE -pthread)
```

### 9.2 `yaml-cpp` 安装到 `lib64/`

现象：

- 服务器原生构建完成后，静态库可能落到 `prefix/lib64/libyaml-cpp.a`

解决方案：

- CMake 查找库时同时兼容：
  - `prefix/lib`
  - `prefix/lib64`

### 9.3 WSL 交叉安装到多架构目录

现象：

- 静态库可能落到：
  - `prefix/lib/aarch64-linux-gnu`

解决方案：

- 查找逻辑同时兼容：
  - `prefix/lib`
  - `prefix/lib64`
  - `prefix/lib/aarch64-linux-gnu`
  - `prefix/lib/x86_64-linux-gnu`

### 9.4 Debug 模式库名带 `d` 后缀

现象：

- WSL 交叉构建 `yaml-cpp` 时，Debug 安装的实际文件可能是：
  - `libyaml-cppd.a`

解决方案：

- 依赖准备脚本在规范化复制时同时尝试：
  - `libyaml-cpp.a`
  - `libyaml-cppd.a`

然后统一复制为：

```text
prefix/lib/libyaml-cpp.a
```

这样主项目无需区分 Debug / Release 库名。

### 9.5 `IMPORTED` target 别名可见性问题

现象：

```text
add_library cannot create ALIAS target ... because target ... is imported but not globally visible
```

解决方案：

- 导入目标时使用 `IMPORTED GLOBAL`

例如：

```cmake
add_library(nlohmann_json INTERFACE IMPORTED GLOBAL)
add_library(spdlog INTERFACE IMPORTED GLOBAL)
```

---

## 10. 验证清单

### 10.1 服务器原生构建验证

在服务器目录：`/home/devuser/WorkSpace/rma_manager`

```bash
cd /home/devuser/WorkSpace/rma_manager
BUILD_SERVICE=ON BUILD_TESTS=OFF RUN_TESTS=OFF bash scripts/build_production.sh
```

通过标准：

- 成功生成 `build_production/bin/rma_service`
- 成功生成 `/home/devuser/WorkSpace/ThirdPartyCache/rma_manager/build/native-aarch64/prefix`

### 10.2 WSL 交叉构建验证

在 WSL 目录：`/mnt/d/Workspace/Company/Tower/rma_manager`

```bash
cd /mnt/d/Workspace/Company/Tower/rma_manager
BUILD_SERVICE=ON BUILD_TESTS=OFF RUN_TESTS=OFF bash scripts/dev_build_wsl_cross.sh
```

通过标准：

- 成功生成 `build_wsl_cross_dev/bin/rma_service`
- `file build_wsl_cross_dev/bin/rma_service` 显示为 `ARM aarch64`
- 成功生成 `/mnt/d/WorkSpace/ThirdPartyCache/rma_manager/build/wsl-aarch64/prefix`

### 10.3 缓存目录验证

在 WSL 目录：`/mnt/d/WorkSpace/ThirdPartyCache/rma_manager`

```bash
cd /mnt/d/WorkSpace/ThirdPartyCache/rma_manager
find build/wsl-aarch64 -maxdepth 3 | sort
```

至少应看到：

- `build/wsl-aarch64/src`
- `build/wsl-aarch64/build`
- `build/wsl-aarch64/prefix/include`
- `build/wsl-aarch64/prefix/lib`

---

## 11. 对其他项目的复用建议

如果你要把这套机制迁移到另一个项目，建议直接照以下顺序执行：

1. 先建立 `ThirdPartyCache/<project>/archives`
2. 再建立 `ThirdPartyCache/<project>/build/native-aarch64` 和 `build/wsl-aarch64`
3. 新增 `prepare_native_deps.sh` 与 `prepare_wsl_cross_deps.sh`
4. 修改主构建脚本，使其先准备缓存
5. 修改 `third_party/CMakeLists.txt`，优先吃共享 `prefix`
6. 固化文档和标准命令
7. 在服务器和 WSL 上各做一次闭环验证

不要跳步骤，也不要只改脚本不改 CMake。

---

## 12. 当前方案结论

截至当前版本，`rma_manager` 已完成并验证通过：

1. 服务器麒麟 V10 ARM64 原生构建接入共享离线包目录
2. WSL aarch64 交叉构建接入共享离线包目录
3. 服务器原生共享依赖缓存目录接入
4. WSL 交叉共享依赖缓存目录接入
5. `rma_service` 在服务器原生路径构建通过
6. `rma_service` 在 WSL 交叉路径构建通过

这意味着该模式已经具备给其他项目复用的条件。
