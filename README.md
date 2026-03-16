# QDGZ300 Backend

## WSL 交叉编译共享缓存

默认共享缓存目录：

- 离线压缩包：`D:\WorkSpace\ThirdPartyCache\qdgz300_backend\archives`
- WSL 依赖缓存：`D:\WorkSpace\ThirdPartyCache\qdgz300_backend\build\wsl-aarch64`
- WSL 共享前缀目录：`D:\WorkSpace\ThirdPartyCache\qdgz300_backend\build\wsl-aarch64\prefix`

在 WSL 中准备共享缓存并交叉编译：

```bash
cd /mnt/d/Workspace/Company/Tower/qdgz300_backend
bash scripts/prepare_wsl_cross_deps.sh
```

完整交叉构建：

```bash
cd /mnt/d/Workspace/Company/Tower/qdgz300_backend
bash scripts/build/build_wsl_cross.sh
```

离线包保留在 Windows 盘，WSL 通过 `/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/archives` 访问；项目不会做本地 Windows 原生编译。

## 构建

服务器原生构建：

```bash
bash scripts/build/build_production.sh
```

服务器默认共享缓存目录：

- 离线压缩包：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/archives`
- 原生依赖缓存：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64`
- 原生共享前缀：`/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64/prefix`

如需单独准备服务器共享依赖缓存：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/prepare_native_deps.sh
```

WSL / Linux 交叉构建：

```bash
bash scripts/build/build_wsl_cross.sh
```

构建脚本现在优先使用共享 `QDGZ300_DEPS_PREFIX`，仓库内 `deps_offline/` 不再作为默认依赖来源；仅在手动指定 `DEPS_OFFLINE_ROOT` 时作为兜底。

## 测试

单元测试：

```bash
bash scripts/test/test_unit.sh build_production
```

集成测试：

```bash
bash scripts/test/test_integration.sh build_production
```

正式入口固定为子目录测试，不使用顶层递归 `ctest`。

## 部署

安装：

```bash
sudo bash deploy/install.sh build_production
sudo bash scripts/ops/enable_services.sh
sudo bash scripts/ops/start_services.sh
```

检查：

```bash
sudo bash scripts/ops/check_services.sh
sudo bash scripts/ops/tail_logs.sh
```

卸载：

```bash
sudo bash deploy/uninstall.sh
```

完整部署说明见 [deploy/README.md](deploy/README.md)。

## 当前状态

- 目录、构建、脚本、部署资产已完成基础收口
- `M03`、`M04`、`control` 已恢复最小可测实现
- 服务器原生构建、测试、安装和 systemd 启动链路已验证通过

更多背景和规划见：

- [工作入口](docs/进展/00_工作入口.md)
- [重构计划](docs/规划/QDGZ300_重构分阶段计划.md)
- [部署基线](docs/基线/麒麟V10_ARM64_CoreX_CUDA_部署基线.md)
- [未实现能力清单](docs/规划/TODO_未实现能力.md)
