# QDGZ300 部署资产说明

当前 `deploy/` 目录先承载系统侧调优资产，和代码仓库中的正式构建/测试入口保持一致。

## 当前包含内容

- `systemd/qdgz300-sysctl.service`
- `systemd/nic-optimization.service`
- `systemd/cpu-performance.service`
- `sysctl/90-qdgz300.conf`

## 当前正式入口

- 服务器原生构建：`bash scripts/build_production.sh`
- WSL/Linux 交叉构建：`bash scripts/dev_build_wsl_cross.sh`
- 正式单测入口：`ctest --test-dir build_production/tests/unit --output-on-failure`

## 当前限制

- 还没有完整的 `install.sh` 安装脚本
- 还没有统一的 service 安装目录布局
- 这部分会在后续部署阶段继续补齐
