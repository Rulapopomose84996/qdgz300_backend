# QDGZ300 Backend

QDGZ300 是运行在麒麟 V10 ARM64 服务器上的雷达后端工程。当前重构目标是把仓库整理成适合长期维护的模块化 CMake 项目，并固化 `clang18 + CoreX + CUDA 兼容层` 的服务器构建基线。

## 当前基线

- 目标环境：Kylin Linux Advanced Server V10 (GFB), `aarch64`
- 最低 CMake：`3.16`
- GPU 编译链：`/usr/local/corex/bin/clang++` + CoreX 4.3.8
- 正式单测入口：`ctest --test-dir <build>/tests/unit --output-on-failure`
- 运行时数据目录：`data/`

## 当前目录

```text
qdgz300_backend/
├── CMakeLists.txt
├── cmake/
├── src/
│   ├── common/
│   ├── m01_receiver/
│   ├── m02_signal_proc/
│   └── logging/
├── tests/
│   ├── unit/
│   └── integration/
├── benches/
├── tools/
├── scripts/
├── deploy/
├── docs/
├── deps_offline/
└── data/
```

## 构建

### 服务器原生构建

```bash
bash scripts/build_production.sh
```

常用变量：

```bash
ENABLE_GPU=ON BUILD_TESTS=ON RUN_TESTS=ON bash scripts/build_production.sh
BUILD_TARGET=receiver_app RUN_TESTS=OFF bash scripts/build_production.sh
```

说明：

- 脚本默认使用 `/usr/local/corex/bin/clang` 和 `/usr/local/corex/bin/clang++`
- 当 `ENABLE_GPU=ON` 时，会自动补 `PATH` 和 `LD_LIBRARY_PATH`
- 正式测试固定跑 `build_production/tests/unit`

### WSL / Linux 交叉构建

```bash
bash scripts/dev_build_wsl_cross.sh
```

常用变量：

```bash
RUN_TESTS=ON bash scripts/dev_build_wsl_cross.sh
BUILD_TARGET=receiver_app bash scripts/dev_build_wsl_cross.sh
```

说明：

- 默认工具链文件：`cmake/toolchains/aarch64-linux-gnu.cmake`
- 默认构建目录：`build_wsl_cross_dev`
- 默认正式测试入口：`build_wsl_cross_dev/tests/unit`

## 测试

正式单测入口：

```bash
ctest --test-dir build_production/tests/unit --output-on-failure
```

集成测试入口：

```bash
ctest --test-dir build_production/tests/integration --output-on-failure
```

不要继续依赖旧环境下的顶层递归 `ctest --test-dir build_production`。

## 部署资产

当前部署目录包含安装脚本、配置模板和系统侧调优资产：

- `deploy/install.sh`
- `deploy/receiver_config_example.yaml`
- `deploy/systemd/qdgz300-receiver.service`
- `deploy/systemd/qdgz300-sysctl.service`
- `deploy/systemd/nic-optimization.service`
- `deploy/systemd/cpu-performance.service`
- `deploy/sysctl/90-qdgz300.conf`

更多说明见 [deploy/README.md](deploy/README.md) 和 [docs/部署基线/麒麟V10_ARM64_CoreX_CUDA_部署基线.md](docs/部署基线/麒麟V10_ARM64_CoreX_CUDA_部署基线.md)。

## 当前状态

- 已下沉模块：`common`、`m01_receiver`、`m02_signal_proc`
- 已重组测试目录：`tests/unit`、`tests/integration`
- 尚未实现的 M03 / M04 / control / towerguard 已从当前构建装配中移除，避免持续产生配置期警告

## 入口文档

- [重构工作入口](docs/项目进展/00_工作入口.md)
- [重构分阶段计划](docs/项目规划/QDGZ300_重构分阶段计划.md)
- [麒麟 V10 ARM64 CoreX/CUDA 部署基线](docs/部署基线/麒麟V10_ARM64_CoreX_CUDA_部署基线.md)
