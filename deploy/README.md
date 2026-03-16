# QDGZ300 部署说明

## 1. 当前正式入口

服务器原生构建：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/build/build_production.sh
```

安装：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
sudo bash deploy/install.sh build_production
sudo bash scripts/ops/enable_services.sh
sudo bash scripts/ops/start_services.sh
```

单元测试：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/test/test_unit.sh build_production
```

集成测试：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/test/test_integration.sh build_production
```

## 2. 共享第三方缓存约定

服务器原生构建默认使用以下共享目录：

```text
/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/
├── archives/
└── build/
    └── native-aarch64/
        ├── src/
        ├── build/
        └── prefix/
```

含义：

- `archives/`：离线源码包唯一来源
- `build/native-aarch64/src/`：第三方源码解压目录
- `build/native-aarch64/build/`：第三方库自身构建目录
- `build/native-aarch64/prefix/`：主项目复用的头文件和静态库前缀

仓库内 `deps_offline/` 不再作为默认依赖来源，只保留人工兜底能力。

## 3. 标准部署流程

### 3.1 更新代码

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
git pull --ff-only origin refactor/kylin-v10-arm64-cuda-clang18
```

### 3.2 构建

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
bash scripts/build/build_production.sh
```

构建脚本会自动完成：

1. 检查 CoreX / clang / ninja / cmake 环境
2. 准备共享第三方缓存
3. 检查并清理失配的旧 `build_production` 缓存
4. 配置并构建主工程
5. 调用 `scripts/test/test_unit.sh` 执行单元测试

### 3.3 安装与启停

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
sudo bash deploy/install.sh build_production
sudo bash scripts/ops/enable_services.sh
sudo bash scripts/ops/start_services.sh
sudo bash scripts/ops/check_services.sh
```

### 3.4 查看日志

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
sudo bash scripts/ops/tail_logs.sh
```

## 4. 安装目录布局

默认安装根目录：

```text
/opt/qdgz300_backend/
├── bin/
│   ├── receiver_app
│   └── fpga_emulator
├── config/
│   ├── receiver.yaml
│   └── receiver.yaml.example
├── data/
├── logs/
├── scripts/
└── releases/
    └── <timestamp>/
```

PCAP 旁路落盘目录：

- NVMe spool：`/opt/qdgz300_backend/data/receiver_spool`
- HDD archive：`/data/qdgz300/receiver/archive`
- 后台搬移：`qdgz300-spool-mover.service`

## 5. 升级与回滚

升级：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
git pull --ff-only
bash scripts/build/build_production.sh
sudo bash deploy/install.sh build_production
sudo systemctl restart qdgz300-receiver.service
```

回滚版本列表：

```bash
sudo ls -1 /opt/qdgz300_backend/releases
```

回滚二进制：

```bash
sudo cp -a /opt/qdgz300_backend/releases/<timestamp>/bin/. /opt/qdgz300_backend/bin/
sudo systemctl restart qdgz300-receiver.service
```

如需回滚配置：

```bash
sudo cp -a /opt/qdgz300_backend/releases/<timestamp>/config/. /opt/qdgz300_backend/config/
```

## 6. 当前测试策略

- 正式构建脚本默认执行单元测试
- 单元测试入口固定为 `scripts/test/test_unit.sh`
- 集成测试入口固定为 `scripts/test/test_integration.sh`
- 不再推荐直接在顶层运行裸 `ctest`

## 7. 验收清单

构建产物：

- `build_production/src/m01_receiver/receiver_app`
- `build_production/tools/fpga_emulator`
- `/home/devuser/WorkSpace/ThirdPartyCache/qdgz300_backend/build/native-aarch64/prefix`

服务验收：

```bash
cd /home/devuser/WorkSpace/qdgz300_backend
sudo bash scripts/ops/check_services.sh
sudo systemctl status qdgz300-receiver.service --no-pager
sudo systemctl status qdgz300-spool-mover.service --no-pager
sudo journalctl -u qdgz300-receiver.service -n 50 --no-pager
```
