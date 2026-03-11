# QDGZ300 部署说明

## 1. 目录布局

默认安装根目录固定为：

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

## 2. 资产对应关系

systemd：

- `deploy/systemd/qdgz300-receiver.service`
- `deploy/systemd/qdgz300-sysctl.service`
- `deploy/systemd/nic-optimization.service`
- `deploy/systemd/cpu-performance.service`

sysctl：

- `deploy/sysctl/90-qdgz300.conf`

运维脚本：

- `scripts/ops/enable_services.sh`
- `scripts/ops/start_services.sh`
- `scripts/ops/stop_services.sh`
- `scripts/ops/check_services.sh`
- `scripts/ops/tail_logs.sh`

## 3. 安装

```bash
bash scripts/build/build_production.sh
sudo bash deploy/install.sh build_production
sudo bash scripts/ops/enable_services.sh
sudo bash scripts/ops/start_services.sh
sudo bash scripts/ops/check_services.sh
```

## 4. 升级

```bash
git pull --ff-only
bash scripts/build/build_production.sh
sudo bash deploy/install.sh build_production
sudo systemctl restart qdgz300-receiver.service
```

说明：

- `deploy/install.sh` 在覆盖二进制前会把上一版 `bin/` 和 `config/` 备份到 `/opt/qdgz300_backend/releases/<timestamp>/`

## 5. 回滚

查看可回滚版本：

```bash
sudo ls -1 /opt/qdgz300_backend/releases
```

回滚二进制：

```bash
sudo cp -a /opt/qdgz300_backend/releases/<timestamp>/bin/. /opt/qdgz300_backend/bin/
sudo systemctl restart qdgz300-receiver.service
```

如果需要回滚配置模板：

```bash
sudo cp -a /opt/qdgz300_backend/releases/<timestamp>/config/. /opt/qdgz300_backend/config/
```

## 6. 卸载

```bash
sudo bash deploy/uninstall.sh
```

## 7. 验收

```bash
sudo bash scripts/ops/check_services.sh
sudo bash scripts/ops/tail_logs.sh
```
