# QDGZ300 麒麟 V10 ARM64 CoreX/CUDA 部署基线

**文档日期**：2026-03-11  
**适用环境**：Kylin Linux Advanced Server V10 (GFB), aarch64  
**适用范围**：`qdgz300_backend` 在麒麟 V10 ARM64 服务器上的原生构建、测试、部署前置约束

---

## 1. 与通用重构模板的差异

本项目参考《同类项目构建与组织架构重构指南（麒麟V10_ARM64）》重构，但有一个关键差异：

- 通用模板默认把 `GCC 7.3.0` 作为稳定编译器基线
- 本项目的信号处理模块涉及 CUDA 兼容链路
- 按《202601160034_Baseline_TowerGuard_Infrastructure_服务器环境基线 (V2.0).md》记录，服务器 GPU 编译链必须使用：
  - `/usr/local/corex/bin/clang++`
  - `clang 18.1.8`
  - `CoreX 4.3.8`
  - `/usr/local/corex/include`
  - `/usr/local/corex/lib64`

因此，本项目后续重构采用“双基线”：

- 非 GPU 通用模块：继续遵守麒麟 V10 / CMake 3.16.5 / ARM64 / 离线依赖 / 稳定测试入口
- GPU 信号处理模块：切换到 `clang18 + CoreX + CUDA 10.2 兼容层`

---

## 2. 当前已确认的环境约束

### 2.1 操作系统与架构

- 操作系统：Kylin Linux Advanced Server V10 (GFB)
- 架构：`aarch64`
- 内核：4.19 系列
- 时区：`Asia/Shanghai`

### 2.2 构建工具链

- CMake：`3.16.5`
- Git：`2.27.0`
- CoreX Clang：`18.1.8`
- NUMA 运行库：`libnuma.so.1`

### 2.3 GPU / CUDA 兼容层

- 加速卡：Iluvatar MR-V100 / MR100 兼容环境
- SDK：CoreX `4.3.8`
- CUDA 兼容层：`10.2`
- 动态库目录：
  - `/usr/local/corex/lib64`
  - `/usr/local/corex/lib`
- 头文件目录：
  - `/usr/local/corex/include`

### 2.4 运行拓扑

- 数据面 CPU：`16-31`
- 管理面 CPU：`0-15`
- GPU 与数据面网卡同属 NUMA Node 1
- 数据面进程必须优先绑定 Node 1

---

## 3. 重构后的构建原则

### 3.1 顶层构建原则

- 顶层 `CMakeLists.txt` 只做模块编排
- 模块源码、依赖和编译定义下沉到各子目录
- `cmake_minimum_required(VERSION 3.16)` 保持不变

### 3.2 依赖原则

- 优先使用共享离线包目录：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/archives`
- 优先使用共享依赖缓存目录：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64`
- 仓库内 `deps_offline/` 不再作为默认构建来源
- 不把公网拉包作为服务器构建前提
- GPU 模块的 CoreX 头文件和库路径显式注入，不依赖隐式环境碰运气

### 3.3 测试原则

- 测试目录固定为：
  - `tests/unit`
  - `tests/integration`
- 正式单元测试入口固定为：

```bash
bash scripts/test/test_unit.sh build_production
```

- 正式集成测试入口固定为：

```bash
bash scripts/test/test_integration.sh build_production
```

### 3.4 运行时目录原则

- 运行时数据统一写入 `data/`
- 仓库只保留 `data/.gitkeep`
- 禁止把 SQLite、日志、转储文件散落到仓库根目录

---

## 4. 当前已固化的正式入口

### 4.1 服务器原生构建

```bash
bash scripts/build/build_production.sh
```

默认行为：

- 构建目录：`build_production`
- 构建类型：`Release`
- 编译器：`/usr/local/corex/bin/clang`、`/usr/local/corex/bin/clang++`
- GPU 开关：`ENABLE_GPU=ON`
- 共享离线包目录：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/archives`
- 共享原生缓存目录：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64`
- 正式单测入口：`scripts/test/test_unit.sh build_production`

### 4.2 WSL / Linux 交叉构建

```bash
bash scripts/build/build_wsl_cross.sh
```

默认行为：

- 构建目录：`build_wsl_cross_dev`
- 工具链：`cmake/toolchains/aarch64-linux-gnu.cmake`
- GPU 开关：默认 `OFF`
- 共享离线包目录：`/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/archives`
- 共享交叉缓存目录：`/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/build/wsl-aarch64`
- 正式单测入口：`scripts/test/test_unit.sh build_wsl_cross_dev`

### 4.3 正式测试入口

```bash
bash scripts/test/test_unit.sh build_production
```

集成测试入口：

```bash
bash scripts/test/test_integration.sh build_production
```

### 4.4 CoreX 环境约束

- `PATH` 需要包含 `/usr/local/corex/bin`
- `LD_LIBRARY_PATH` 需要包含 `/usr/local/corex/lib64:/usr/local/corex/lib`
- 服务器原生构建脚本会在 `ENABLE_GPU=ON` 时主动补齐这两个环境变量

### 4.5 当前安装入口

```bash
sudo bash deploy/install.sh build_production
```

默认安装目录：

```text
/opt/qdgz300_backend/
├── bin/
├── config/
├── data/
├── logs/
└── scripts/
```

### 4.6 当前共享缓存入口

服务器原生依赖缓存准备：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/prepare_native_deps.sh
```

WSL 交叉依赖缓存准备：

```bash
cd /mnt/d/Workspace/Company/Tower/qdgz300_backend
bash scripts/prepare_wsl_cross_deps.sh
```

### 4.7 已验证的 systemd 启停链路

已在服务器上验证通过的服务：

- `qdgz300-sysctl.service`
- `nic-optimization.service`
- `cpu-performance.service`
- `qdgz300-receiver.service`

已验证通过的命令：

```bash
sudo systemctl enable qdgz300-sysctl.service nic-optimization.service cpu-performance.service qdgz300-receiver.service
sudo systemctl start qdgz300-sysctl.service nic-optimization.service cpu-performance.service qdgz300-receiver.service
sudo systemctl status qdgz300-receiver.service --no-pager
sudo journalctl -u qdgz300-receiver.service -n 50 --no-pager
```

已验证的运行结果：

- `receiver_app` 由 systemd 正常拉起
- 运行路径：`/opt/qdgz300_backend/bin/receiver_app --config /opt/qdgz300_backend/config/receiver.yaml`
- 三阵面监听配置已生效：
  - `192.168.1.101:9999`
  - `192.168.2.101:9999`
  - `192.168.3.101:9999`

---

## 5. 当前阶段的验收目标

### Phase 1

- 重构工作入口文档建立完成
- 项目规划文档建立完成
- 部署基线文档建立完成
- `data/` 目录和忽略规则固化完成

### Phase 2

- 顶层 `CMakeLists.txt` 拆分为模块编排式结构
- 新增 `cmake/third_party.cmake`
- 新增 `cmake/options.cmake` 和 `cmake/warnings.cmake`
- 已实现模块下沉到 `src/*/CMakeLists.txt`

### Phase 3

- `tests/` 已按 `unit/integration` 分层
- 正式测试入口已写入脚本和 README
- README 与部署文档已同步新的目录和测试入口

### Phase 4

- 当前构建仅装配已实现模块
- 空壳模块已从装配链移除，不再产生持续 configure warning

### Phase 5

- `deploy/install.sh` 已补齐
- `qdgz300-receiver.service` 与安装目录布局已补齐
- 服务器安装链路已验证通过

### Phase 6

- README、部署基线、工作入口已同步当前真实状态
- 服务器 systemd 启停链路已写入文档

---

## 6. 风险提示

- 旧版麒麟环境的顶层 `ctest` 递归发现不稳定，不能继续依赖
- `.cu` 文件与 CoreX clang 链路的组合需要单独验证，不能直接假设与标准 CUDA/NVCC 完全等价
- 如果非 GPU 模块和 GPU 模块混用编译器，必须明确边界，避免 ABI 和链接参数漂移
