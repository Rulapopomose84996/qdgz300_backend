# QDGZ300 部署资产说明

当前 `deploy/` 目录承载安装脚本、运行时配置模板和系统侧调优资产，和代码仓库中的正式构建/测试入口保持一致。

## 当前包含内容

- `systemd/qdgz300-sysctl.service`
- `systemd/nic-optimization.service`
- `systemd/cpu-performance.service`
- `systemd/qdgz300-receiver.service`
- `sysctl/90-qdgz300.conf`
- `receiver_config_example.yaml`
- `install.sh`

## 当前正式入口

- 服务器原生构建：`bash scripts/build/build_production.sh`
- WSL/Linux 交叉构建：`bash scripts/build/build_wsl_cross.sh`
- 正式单测入口：`ctest --test-dir build_production/tests/unit --output-on-failure`

## 安装方式

```bash
sudo bash deploy/install.sh build_production
```

默认安装布局：

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
└── scripts/
```
