---
tags: []
aliases:
  - 服务器环境基线文档 (V2.0)
date created: 星期一, 一月 5日 2026, 11:09:06 上午
date modified: 星期六, 三月 7日 2026, 9:30:00 下午
---

# 服务器环境基线文档 (V2.0)

> [!IMPORTANT] 部署环境约束 (Deployment Constraints)
> - **架构**: ARM64 (aarch64)，严禁混入 x86_64 二进制包。
> - **任务/管理分离**: 数据面固定在 **NUMA Node 1 / CPU 16-31**，管理面固定在 **CPU 0-15**。
> - **内核隔离**: 已通过 GRUB 参数 `isolcpus=16-31 nohz_full=16-31 rcu_nocbs=16-31` 将数据面核心从调度器摘除。
> - **业务防火墙**: `firewalld` 保持开启，雷达接收端口 `9999/udp` 必须显式放行（已由 `nic-optimization.service` 持久化）。
> - **启动方式**: `receiver_app` 必须以 `numactl --cpunodebind=1 --membind=1` 运行。
> - **时钟源**: 当前处于 **Orphan Mode (stratum 10)**，集群内部同步正常，需注意与标准时间的长期对齐。

## 第一部分：硬件基础设施 (Hardware Infrastructure)

本章节记录了服务器的核心硬件配置，包括计算、存储、网络及系统架构信息。

### 1.1 系统概览 (System Summary)

该服务器基于 **ARM64 (aarch64)** 架构，由 **HCG** 厂商生产，定位于高性能国产化计算平台。

| **项目**    | **详细参数**                          | **备注**                   |
| --------- | --------------------------------- | ------------------------ |
| **设备型号**  | HCG-975F                          | 序列号: Z00000000688RC91001 |
| **处理器架构** | aarch64 (ARMv8)                   | 兼容 32-bit / 64-bit 运行模式  |
| **操作系统**  | Kylin Linux Advanced Server (V10) | 内核版本: 4.19.90            |
| **字节序**   | Little Endian                     | 小端序                      |

---

### 1.2 计算资源 (CPU)

服务器搭载一颗国产 **飞腾 (Phytium)** 高性能处理器，采用多核多节点架构。

- **核心规格**：
    - **型号名称**：Phytium S5000C B-EN32-C
    - **核心数量**：32 核心 (单插槽，每个核 1 线程)
    - **主频范围**：50.0 MHz - 2300.0 MHz
- **缓存体系 (Cache)**：
    - **L1 (d/i)**：2 MiB / 2 MiB
    - **L2 Cache**：16 MiB
    - **L3 Cache**：16 MiB
- **NUMA 架构**：

    > **底层逻辑**：系统分为 2 个 NUMA 节点，旨在减少大规模多核并发下的内存总线竞争。在部署高性能应用时，建议进行 CPU 亲和性绑定。

    - **NUMA node 0**: CPU 0-15
    - **NUMA node 1**: CPU 16-31

- **频率策略基线**：
    - 数据面 CPU 16-31 由 `cpu-performance.service` 持久化锁定 `performance` governor。
    - 现场核验：`cpu0` / `cpu15` / `cpu16` / `cpu31` 采样值均为 `performance`。
    - 脚本路径：`/usr/local/bin/cpu-performance.sh`。

---

### 1.3 内存系统 (Memory)

系统当前配置 **64GB** 高速 DDR5 内存，具备纠错能力。

- **总量统计**：
    - **物理内存**：62 GiB (实际可用约 64GB)
    - **交换分区 (Swap)**：8.0 GiB (当前占用 0B)
- **插槽详情**：
    - **类型**：DDR5 Synchronous Registered (Buffered)
    - **插槽分布**：共 4 个插槽，当前使用 2 个 (DIMM0, DIMM2)
    - **单条规格**：32 GB / Samsung (三星) / 4800 MT/s (当前运行于 4000 MT/s)
    - **ECC 支持**：支持 Single-bit ECC
- **NUMA 内存策略**：
    - `receiver_app` 启动时通过 `numactl --cpunodebind=1 --membind=1` 将热路径内存优先分配在 Node 1。
    - 单阵面实测中，收包负载上来后 Node 1 占用显著高于空载基线。

---

### 1.4 存储系统 (Storage)

存储架构采用 **NVMe 固态硬盘（系统盘）+ 机械硬盘 RAID 10（数据盘）** 的高性能与高可靠组合。

#### 1.4.1 物理磁盘列表

|**设备路径**|**媒介类型**|**容量**|**型号/序列号**|**用途**|
|---|---|---|---|---|
|`/dev/nvme0n1`|NVMe SSD|2.0 TB|SanDisk Extreme 2TB X3N|系统与引导|
|`/dev/sda`|HDD (7200rpm)|20.0 TB|Seagate ST20000DM001|RAID 10 成员|
|`/dev/sdb`|HDD (7200rpm)|20.0 TB|Seagate ST20000DM001|RAID 10 成员|
|`/dev/sdc`|HDD (7200rpm)|20.0 TB|Seagate ST20000DM001|RAID 10 成员|
|`/dev/sdd`|HDD (7200rpm)|20.0 TB|Seagate ST20000DM001|RAID 10 成员|

#### 1.4.2 逻辑卷与 RAID 配置

- **数据阵列 (Software RAID 10)**：
    - **设备**：`/dev/md0`
    - **容量**：36.1 TB (可用)
    - **文件系统**：XFS
    - **挂载点**：`/data`
    - **状态**：`[UUUU]` (全部在线)，512K chunks
- **系统分区 (LVM)**：
    - `/` (根分区)：1.7 TB (XFS)
    - `/boot`：842.7 MB
    - `/boot/efi`：592.3 MB (VFAT)

---

### 1.5 网络与外设 (Network & Peripherals)

本服务器配置了高性能网络子系统，包含国产千兆网卡与 Intel 万兆光纤网卡的组合，支持多种硬件加速特性。

#### 1.5.1 网卡硬件架构

服务器共计拥有 **8 个物理网口**，分布在两块 PCIe 网卡控制器上：

|**控制器名称**|**制造商**|**规格**|**对应接口名称**|**驱动**|
|---|---|---|---|---|
|**WX1860A4**|北京网讯 (Wangxun)|4 × 1GbE (电口)|`enP1s24f0` ~ `f3`|`ngbe`|
|**Intel X710**|Intel Corporation|4 × 10GbE SFP+ (光口)|`enP1s25f0` ~ `f3`|`i40e`|

#### 1.5.2 数据面/管理面基线与 IRQ 亲和性

> **架构要点**：为确保信号处理主干道的“确定性”，系统采用**物理分布与逻辑迁移**相结合的策略。

- **数据面光口**：`enP1s25f0` / `enP1s25f1` / `enP1s25f2` / `enP1s25f3`
    - 物理位于 **NUMA Node 1**。
    - IRQ 固定到 **CPU 16-31** （数据面专用）。
- **管理口**：`enP1s24f0`
    - 物理位于 **NUMA Node 1**，但通过 `smp_affinity` 将其所有中断逻辑绑定在 **CPU 0-15**。
    - 目的：将远程开发（SSH/VS Code）和显控数据转发产生的中断压力移出任务节点。
- **`irqbalance` 已禁用** (`disabled / inactive`)，避免覆盖手工 IRQ 绑定。
- **持久化机制**：
    - 脚本：`/usr/local/bin/nic-tuning.sh`
    - 服务：`nic-optimization.service`（开机自动执行防火墙放行、X710 ring 调优、IRQ 绑核）

#### 1.5.3 关键接口配置详情

- **环形缓冲区 (Ring Buffer)**：
    - Intel X710 四个光口均要求 `RX/TX ring = 4096`（已达硬件上限）。
    - **加固说明**：为应对 $1.25$ GB/s 的高吞吐量数据流，已完成 RX/TX 队列扩容，消除了缓冲区不足导致的 `rx_dropped` 丢包隐患。
- **硬件加速特性 (Offloading)**：
    - **已开启**：`rx-checksumming`, `tx-checksumming`, `tcp-segmentation-offload (TSO)`, `generic-receive-offload (GRO)`, `ntuple-filters`。
    - **作用**：允许网卡硬件处理 TCP 校验和分段，显著降低 CPU 负载。
- **持久化方案**：由 `nic-optimization.service` 开机自动执行（内部调用 `/usr/local/bin/nic-tuning.sh`）。

#### 1.5.4 路由与链路状态

- **当前管理网口**：`enP1s24f0` (北京网讯千兆电口)。
- **管理口 IP**：`192.168.31.234/24` (通过 DHCP 获取)。
- **默认网关**：`192.168.31.1`。
- **数据面光口 IP 配置**：通过 NetworkManager connection profiles 持久化（`radar1`/`radar2`/`radar3`）。
- **万兆雷达数据网口配置**（三阵面物理隔离）：

|**阵面**|**网口设备**|**IP 地址**|**端口**|**CPU 亲和性**|**NM 连接名**|**备注**|
|---|---|---|---|---|---|---|
|Array Face #1|`enP1s25f0`(Intel X710 Port0)|`192.168.1.101/24`|9999|CPU 16|`radar1`|阵面 1 数据接收|
|Array Face #2|`enP1s25f1`(Intel X710 Port1)|`192.168.2.101/24`|9999|CPU 17|`radar2`|阵面 2 数据接收|
|Array Face #3|`enP1s25f2`(Intel X710 Port2)|`192.168.3.101/24`|9999|CPU 18|`radar3`|阵面 3 数据接收|

> **注**：三阵面采用**相同端口 + 不同 IP**的物理隔离方案，每阵面对应独立的 SourceID（0x11/0x12/0x13）。

> [!NOTE] 本地单阵面真实自回环调试
> `enP1s25f3` 可被移入 `txface4` network namespace 并配置为 `192.168.1.102/24`，作为发送端。此时在 root namespace 下执行 `ip link show enP1s25f3` 会显示 `Cannot find device`，属于正常现象。

- **虚拟网桥**：`virbr0` (192.168.122.1)，由 libvirt 自动生成，当前为 `linkdown` 状态。

#### 1.5.5 图形与显示

- **控制器**：ASPEED Graphics Family [1a03:2000] (rev 41)。
- **说明**：集成在基板管理控制器 (BMC) 中的基本显示芯片，不支持 GPU 加速计算，仅用于控制台管理。

---

### 1.6 加速器与 AI 基础设施 (Accelerator & AI Stack)

本服务器集成了国产高性能 AI 加速芯片，采用驱动级兼容 CUDA 的技术路线，为深度学习与科学计算提供硬件支撑。

#### 1.6.1 硬件规格 (GPU/Accelerator)

- **设备名称**：Iluvatar MR-V100 (天数智芯 智铠系列)
- **总线 ID**：`0001:01:00.0` (PCIe x16, 16GT/s)
- **NUMA 亲和性**：绑定在 **NUMA Node 1**
- **说明**：当前位置与万兆网卡同属 Node 1，符合高吞吐信号处理的“同节点数据交换”原则。

#### 1.6.2 驱动与运行时环境 (Driver & Runtime)

系统已安装天数智芯 CoreX 软件包，其核心逻辑是通过二进制兼容与符号映射实现对 CUDA 生态的承接。

|**组件**|**版本/路径**|**关键备注**|
|---|---|---|
|**驱动版本**|4.3.8|内核模块: `iluvatar`, `mdev`, `vfio`|
|**软件栈版本**|CoreX 4.3.8|安装路径: `/usr/local/corex/`|
|**CUDA 兼容层**|10.2|模拟 CUDA 10.2 运行环境|
|**编译器**|clang 18.1.8|位于 `/usr/local/corex/bin/clang++`|

#### 1.6.3 符号链接与兼容性库 (Compatibility Libraries)

该设备在 `/usr/local/corex/lib64/` 目录下通过动态库劫持技术映射了大量的标准 CUDA 库，确保原 x86/CUDA 源码经由 `clang` 编译后可直接运行。

- **核心库文件清单**：
    - **深度学习**：`libcudnn.so.7` (约 1GB), `libcuinfer.so.7` (约 2GB)
    - **数学运算**：`libcublas.so.10`, `libcufft.so.10`, `libcurand.so`
    - **线性解法**：`libcusolver.so.10`, `libcusparse.so.10`
    - **并行通信**：`libixccl.so` (对应 NCCL), `libixml.so` (对应 NVML)
- **符号审计**：
    - 通过 `nm` 验证，`libcufft.so` 已实现标准入口如 `cufftPlan1d`，证明了其 API 级兼容性。

#### 1.6.4 开发工具链探测

- **关键头文件**：`/usr/local/corex/include` 下已包含全套 `cuda.h`, `cublas_v2.h`, `cudnn.h` 等。
- **缺失组件告警**：命令行暂未找到 `ixcc` (天数智芯专有编译器命令)。目前的编译工作流主要依赖 `/usr/local/corex/bin/clang++` 配合特定的 Target 指令。

---

### 1.7 硬件架构综合审计

> **[核心架构] 任务平面与管理平面物理隔离表**
>
> 硬件基线已调整为“纯净任务节点”模式，确保信号处理不受系统底噪干扰。

|**资源名称**|**物理位置**|**职责归属**|**建议绑定核心**|**访存代价**|
|---|---|---|---|---|
|**Intel 10G NIC**|**Node 1**|**数据采集 (1.25 GB/s)**|CPU 16-31|**本地访问 (数据面专用)**|
|**Iluvatar MR-V100**|**Node 1**|**信号处理/目标识别**|CPU 16-31|**本地访问 (与数据面同节点)**|
|**LSI SAS3008 RAID**|**Node 0**|**原始数据落盘/调试**|CPU 0-15|**跨节点 (保护计算岛)**|
|**Wangxun 1G NIC**|**Node 1**|**SSH/显控/运维**|**CPU 0-15**|**逻辑迁移 (消除抠动)**|
|**系统盘 (NVMe)**|**Node 0**|**OS 运行/日志/监控**|CPU 0-15|**本地访问 (降低对数据面干扰)**|

参考 [[202601051535_雷达系统数据流拓扑设计|雷达系统数据流拓扑设计]]

---

### 1.8 风险与局限性提示 (Critical Notes)

> 1. **架构兼容性**：此服务器为 **aarch64** 架构，所有二进制文件、Docker 镜像必须使用 ARM64 版本，x86_64 镜像无法直接运行。
> 2. **许可证告警**：系统当前处于 **[未激活]** 状态，试用期剩余 **98 天**（截至 2026-03-07 检查），请及时联系运维团队导入正式许可证以防服务中断。
> 3. **内存带宽**：当前内存运行频率为 4000 MT/s，低于硬件标称的 4800 MT/s。若业务涉及高频内存交换，需核实主板固件或 CPU 内存控制器的限制。
> 4. **存储带宽瓶颈**：机械硬盘 RAID 10 在 Node 0 的理论写入极限约为 $400$ MB/s [理论推导]。在开启原始数据录制（$1.25$ GB/s）时，必须采用“NVMe 一级缓冲 + 后台异步搬移至 HDD”的策略，否则会导致 Node 1 环形缓冲区溢出。
>
> 5. **跨节点录制负载**：原始数据通过 Node 0 的录制进程跨节点拉取，需监控飞腾内部总线利用率。虽然 $1.25$ GB/s 在总线承载范围内，但需防止 Node 0 的高频 IO 请求导致 Node 1 出现内存访问延迟波动。
>
> 6. **中断隔离红线**：由于网讯网卡物理位于 Node 1 域，若未正确配置 `smp_affinity`，远程开发产生的网络中断将直接打断雷达实时算法，导致确定性失效。

---

## 第二部分：操作系统与内核环境 (OS & Kernel)

本章节记录了操作系统的发行版本、内核规格、安全基线及系统级资源限制配置。

### 2.1 系统版本与内核 (Distribution & Kernel)

服务器运行国产自主指令集优化后的银河麒麟高级服务器操作系统。

|**项目**|**详细参数**|**备注**|
|---|---|---|
|**发行版本**|Kylin Linux Advanced Server V10 (GFB)|银河麒麟 V10|
|**内核版本**|4.19.90-52.23.v2207.gfb08.ky10.aarch64|SMP 架构内核|
|**系统语言**|zh_CN.UTF-8|默认中文字符集|
|**时区配置**|Asia/Shanghai (CST, +0800)|亚洲/上海时区|

当前内核启动参数已确认包含：

- `isolcpus=16-31` — 将数据面核心从内核调度器摘除
- `nohz_full=16-31` — 数据面核心无时钟中断（降低抠动）
- `rcu_nocbs=16-31` — RCU 回调卸载至 housekeeping 核心
- `audit=0` — 禁用内核审计子系统（减少开销）
- `smmu.bypassdev=0x1000:0x17` / `0x1000:0x15` — SMMU bypass 设备
- `module_blacklist=phytium_mci_pci,phytium_mci` — 禁用飞腾 MCI 驱动

> **持久化位置**：`/etc/grub2-efi.cfg` 内核命令行参数。

---

### 2.2 安全配置基线 (Security Baseline)

系统的安全防护机制当前处于“混合”状态，需关注防火墙对业务端口的影响。

- **SELinux (Security-Enhanced Linux)**：
    - **状态**：`Disabled` (已禁用)
    - **影响**：降低了应用部署时的权限策略复杂度，但在公网暴露环境下需依赖其他安全手段。
- **Firewalld (动态防火墙)**：
    - **状态**：`Active (running)` (开启中)
    - **策略说明**：默认情况下会拦截未显式放行的非标准业务端口。
    - **已放行端口**：`7897/tcp`、`9999/udp`。
    - **持久化机制**：`9999/udp` 的放行已并入 `nic-optimization.service`，开机自动执行。
    - **核查命令**：

```bash
sudo firewall-cmd --query-port=9999/udp
sudo iptables-save | grep -E '9999|IN_public'
```

---

### 2.3 资源限制 (Resource Limits - ulimit)

当前系统的用户级资源配置多为默认值，高并发场景下可能存在瓶颈。

|**资源类型**|**配置参数**|**潜在风险/影响**|
|---|---|---|
|**最大文件打开数 (open files)**|**65535**|已通过修改 `/etc/security/limits.conf` 实现软硬限制同步提升，消除了雷达原始数据高速录制时的句柄瓶颈。|
|**最大用户进程数 (max user processes)**|255822|满足绝大多数应用需求。|
|**核心转储文件 (core file size)**|unlimited|允许生成完整 Core Dump，便于调试，需注意磁盘空间。|
|**栈限制 (stack size)**|8192 KiB|标准 Linux 默认配置。|

---

### 2.4 网络环境与服务 (Network & Services)

#### 2.4.1 网络基础配置

- **DNS 解析**：
    - 当前由 NetworkManager 生成，指向网关 `192.168.31.1`。
- **时间同步 (NTP)**：
    - **服务状态**：`chronyd` 已激活。
    - **同步状态**：`System clock synchronized: no`（外部 NTP 服务器不可达，已启用 Orphan Mode）。
    - **授时配置**：已修改 `/etc/chrony.conf`，启用 `local stratum 10` 并设内网源 `192.168.0.1` 为 Server。
    - **Chrony Tracking**：Stratum 10，Leak status: Normal，系统将自身作为时钟源。

#### 2.4.2 网络栈与持久化 sysctl

当前持久化文件：

- `/etc/sysctl.d/90-qdgz300.conf`
- `qdgz300-sysctl.service`（在麒麟默认 `kylin.conf` 覆盖后重新应用 QDGZ300 参数）

| **参数** | **基线值** | **作用** |
| --- | --- | --- |
| `net.core.rmem_default` | `67108864` (64 MB) | 默认接收缓冲 |
| `net.core.rmem_max` | `536870912` (512 MB) | 允许 `receiver_app` 申请 256MB socket 缓冲 |
| `net.ipv4.udp_rmem_min` | `262144` (256 KB) | UDP 最小接收缓冲 |
| `net.ipv4.udp_mem` | `262144 1048576 4194304` | UDP 全局内存水位 (pages) |
| `net.core.netdev_max_backlog` | `250000` | 增强网络突发承载 |

推荐核查命令：

```bash
sysctl net.core.rmem_default net.core.rmem_max net.ipv4.udp_rmem_min
sysctl net.ipv4.udp_mem net.core.netdev_max_backlog
```

> [!NOTE] 麒麟 sysctl 覆盖问题
> 银河麒麟 V10 的 `sysctl --system` 会在后段再次加载 `/usr/lib/sysctl.d/kylin.conf`（其中 `net.core.rmem_max=2097152`、`net.core.netdev_max_backlog=8000` 等值会覆盖我们的配置），因此 V2.0 基线新增 `qdgz300-sysctl.service`，在系统默认流程之后重新应用 `90-qdgz300.conf`。

#### 2.4.3 关键 systemd 服务基线

| **服务名** | **状态** | **职责** |
| --- | --- | --- |
| `qdgz300-sysctl.service` | enabled / active (exited) | 在麒麟默认 `kylin.conf` 覆盖后重新应用 QDGZ300 网络栈参数 |
| `nic-optimization.service` | enabled / active (exited) | 开机执行防火墙放行、X710 ring 调优、IRQ 绑核 |
| `cpu-performance.service` | enabled / active (exited) | 开机将数据面 CPU 16-31 governor 锁定为 `performance` |
| `iluvatar-node-init.service` | enabled | GPU 设备节点初始化 |
| `irqbalance.service` | **disabled / inactive** | 避免覆盖手工 IRQ 绑定 |
| `firewalld.service` | active | 动态防火墙 |
| `NetworkManager.service` | active | 网络管理 |

#### 2.4.4 关键监听端口 (Active Ports)

|**协议**|**端口**|**服务进程**|**监听范围**|**备注**|
|---|---|---|---|---|
|TCP|22|sshd|0.0.0.0|远程管理端口|
|TCP|8080|receiver_app (metrics)|0.0.0.0|监控指标端口|
|TCP|7897|sshd|127.0.0.1|SSH 隧道或转发|
|UDP|323|chronyd|127.0.0.1|时间同步服务|
|UDP|9999|receiver_app|`192.168.1.101` / `192.168.2.101` / `192.168.3.101`|雷达数据面接收端口，已在防火墙中放行|

---

### 2.5 软件仓库与包管理 (YUM/DNF)

系统配置了银河麒麟官方的软件仓库，可直接通过 `yum` 安装国产化适配包。

- **ks10-adv-os**: 核心操作系统基础包。
- **ks10-adv-updates**: 安全补丁与软件更新包。

---

### 2.6 风险点与优化建议 (Optimization Notes)

> 1. **sysctl 覆盖风险**：麒麟系统的 `/usr/lib/sysctl.d/kylin.conf` 会在 `sysctl --system` 流程中覆盖用户配置。已通过 `qdgz300-sysctl.service` 解决，但如果修改了 `90-qdgz300.conf`，需重启该服务以生效。
>
> 2. **时间同步异常**：`System clock synchronized` 为 `no`。当前处于 Orphan Mode (stratum 10)，系统将自身作为时钟源，长期运行可能存在毫秒级偏移。对于多机协同场景，必须引入外部 NTP 授时源。
>
> 3. **系统激活提醒**：试用期仅剩 **98 天**（截至 2026-03-07）。虽暂不影响技术功能，但在截止日期前必须完成激活，否则可能导致 `yum` 源失效或关键安全补丁停止更新。
>
> 4. **irqbalance 重启风险**：若后续重新启用 `irqbalance`，必须先评估是否会覆盖数据面 IRQ 亲和性。

---

## 第三部分：开发工具链与运行时栈 (Dev Stack & Runtime)

本章节记录了支持 AArch64 架构及天数智芯（Iluvatar）加速器的开发环境配置基线。

### 3.1 核心编译工具链 (Core Toolchain)

系统配置了适配 ARM64 原生开发及国产 GPU 交叉编译的工具集合。

|**组件**|**基线版本**|**关键属性 / 配置路径**|**备注**|
|---|---|---|---|
|**编译器 (Clang++)**|18.1.8|`/usr/local/corex/bin/clang++`|支持 Iluvatar 算子下发|
|**构建系统 (CMake)**|3.16.5|`/usr/bin/cmake`|标准 Makefile 生成工具|
|**版本控制 (Git)**|2.27.0|`/usr/bin/git`|源码管理|
|**NUMA 库**|libnuma.so.1|`/usr/lib64/libnuma.so`|用于进程/内存亲和性绑定|

---

### 3.2 加速卡开发环境 (AI/GPU Stack)

基于 **天数智芯 CoreX** 软件栈，实现对雷达信号处理算法（如 FFT）的硬件加速支持。

#### 3.2.1 软件栈规格

- **SDK 版本**：CoreX 4.3.8
- **生态兼容性**：提供 CUDA 10.2 API 兼容层，支持原位迁移基于 CUDA 的信号处理代码。
- **数学算子库**：已内置 `libcufft.so` (10.1.2.89)，支持 `cufftPlan1d` 等标准频域变换接口。

#### 3.2.2 硬件访问配置与持久化

为确保加速卡在系统重启及容器启动前可用，系统已配置自动节点创建服务。

- **管理服务**：`iluvatar-node-init.service` (Systemd)
    - **修复动作**：修正了 `oneshot` 服务类型下非法的 `Restart=` 参数配置，解决了服务启动被拒的问题。
    - **权限控制**：确认为设备节点 `/dev/ix0` 和 `/dev/ixctl` 自动分配了 `666` 权限，确保非 Root 容器进程可正常调用。
- **初始化脚本**：`/usr/local/bin/init-iluvatar-nodes.sh`
- **节点列表**：
    - `/dev/ix0` (数据平面，Major: 240)
    - `/dev/ixctl` (控制平面，Major: 240)
- **加载逻辑**：服务被配置为 `Before=docker.service`。这确保了容器在启动时，宿主机硬件设备号已探测完成并挂载，有效避免了容器因找不到 `/dev/ix*` 而启动失败的风险。

---

### 3.3 容器化基础设施 (Docker)

针对国产内核与 AArch64 架构深度优化的 Docker 运行环境，已完成生产级配置。

#### 3.3.1 版本与架构

- **Docker Engine**：18.09.0 (EulerVersion: 18.09.0.206)
- **架构支持**：`linux/arm64`

#### 3.3.2 关键守护进程配置 (`/etc/docker/daemon.json`)

为确保与银河麒麟内核行为一致并保护系统分区空间，已实施以下基线：

|**配置项**|**基线参数**|**审计逻辑**|
|---|---|---|
|**Cgroup Driver**|`systemd`|**[强制]** 与麒麟内核资源管理机制完全对齐。|
|**Log Driver**|`json-file`|限制日志文件大小。|
|**Log Max-size**|`100m`|防止雷达高频调试日志耗尽根分区（1.7TB）空间。|
|**Storage Driver**|`overlay2`|针对 Linux 4.19 内核的最佳 I/O 性能选择。|

---

### 3.4 运行时依赖与环境审计 (Runtime Audit)

#### 3.4.1 驱动状态

- 内核模块 `iluvatar`, `mdev`, `vfio` 已正确加载。
- 主设备号 `240` 已动态分配。

#### 3.4.2 性能拓扑建议 (Affinity)

> **[核心原则] 基于任务/管理分离的绑定策略：**
>
> 1. **任务岛隔离**：所有实时信号处理进程及天数智芯算子下发任务必须绑定在 **Node 1 (CPU 16-31)**。
>
> 2. **底噪卸载**：所有非实时任务（包括磁盘录制、1G 网卡中断处理、系统监控、日志守护进程）必须通过配置绑定在 **Node 0 (CPU 0-15)**，确保 Node 1 核心始终处于“算法专用”状态。

---

### 3.5 风险点提示 (Critical Risks)

1. **Docker 镜像架构兼容性**：所有业务容器必须基于 `arm64` 基础镜像构建。若误拉取 `x86_64` 镜像，Docker 虽可能报错但不排除部分二进制在启动时才崩溃。
2. **CoreX 工具链依赖**：雷达算法代码的编译必须显式指定库路径 `/usr/local/corex/lib64`，否则链接器可能错误引用系统默认的低性能库。

---

### 3.6 receiver_app 运行基线

#### 3.6.1 启动命令

```bash
numactl --cpunodebind=1 --membind=1 ./build_release/receiver_app --config config/receiver.yaml
```

#### 3.6.2 线程结构基线

- **主线程**：Node 1 调度域
- **metrics 线程**：管理面 CPU 0-15，监听 `0.0.0.0:8080`
- **阵面接收线程**：固定到 CPU 16/17/18

#### 3.6.3 单阵面本地真实自回环基线

- **接收端**：`enP1s25f0 -> 192.168.1.101:9999`
- **发送端**：`txface4/enP1s25f3 -> 192.168.1.102`
- **发送器约束**：
    - 使用 `fpga_emulator --bind-ip --bind-device --cpu-list`
    - 将 sender 固定在 CPU 0-15，避免争抢数据面核心

#### 3.6.4 当前代码侧调试指标

已纳入 receiver metrics 的关键指标：

- `receiver_socket_packets_received_total`
- `receiver_socket_bytes_received_total`
- `receiver_socket_receive_batches_total`
- `receiver_socket_source_filtered_total`
- `receiver_socket_receive_errors_total`
- `receiver_pipeline_packets_entered_total`
- `receiver_pipeline_parse_ok_total`
- `receiver_pipeline_validate_ok_total`
- `receiver_packets_received_total`
- `receiver_packets_dropped_total`
- `numa_local_memory_pct`

---

### 3.7 环境变量与持久化配置 (Environment Persistence)

> **修订说明**：已完成 `devuser` 用户环境的持久化配置，确保编译工具链与动态库路径在登录时自动加载。

- **配置文件**：`~/.bashrc`
- **生效范围**：仅限 `devuser` 用户。
- **配置内容审计**：
    - `PATH`：已注入 `/usr/local/corex/bin`，确保 `clang++` 等工具可直接调用。
    - `LD_LIBRARY_PATH`：已注入 `/usr/local/corex/lib64` 及 `/usr/local/corex/lib`。

|**变量名**|**当前有效值**|**验证状态**|
|---|---|---|
|**`PATH`**|`…:/usr/local/corex/bin:…`|**已验证 (PASS)**|
|**`LD_LIBRARY_PATH`**|`/usr/local/corex/lib64:/usr/local/corex/lib:…`|**已验证 (PASS)**|

---

## 变更记录与审计详情 (Audit Logs)

### 1. 存储与资源限制修复 (Resource Limits Patch)

- **审计发现**：此前 `/etc/security/limits.conf` 存在书写偏差（`hard nofile 6553`），导致硬限制未达标。
- **修复动作**：通过 `sed` 指令完成了物理修复。
- **最终状态**：
    - `soft nofile 65535`
    - `hard nofile 65535`
- **运行时验证**：执行 `ulimit -n` 返回结果为 **$65535$**，已消除录制任务的句柄瓶颈。

### 2. 时钟同步策略调整 (Time Sync Redesign)

- **异常识别**：通过 `nmap` 探测发现，网关 `192.168.0.1` 的 **123/UDP (NTP)** 端口处于 `closed` 状态，导致原定同步方案失效。
- **应急方案**：
    - 已启用 **Orphan Mode (孤立模式)**：修改 `/etc/chrony.conf`，取消 `local stratum 10` 的注释。
    - **效果**：`chronyc tracking` 显示 `Leap status: Normal`，系统将自身作为时钟源提供给下游业务。
- **局限性**：当前时间（2026-01-07）与真实世界时间的一致性完全依赖于硬件时钟 (RTC)，由于缺乏外部授时，长期运行可能存在毫秒级偏移。

### 3. CoreX SDK 深度审计

- **物理路径**：`/usr/local/corex` 软链接至 `/usr/local/corex-4.3.8/`。
- **核心算子库规格验证**：
    - `libcufft.so.10.1.2.89`: **412 MiB** (频域处理核心)
    - `libcublas.so.10.2.3.254`: **133 MiB** (矩阵运算核心)
    - `libcusolver.so.10.3.0.89`: **3.2 MiB** (线性解法核心)
- **权限验证**：`/dev/ix0` 与 `/dev/ixctl` 权限已确认由 `iluvatar-node-init.service` 设置为 `666` (crw-rw-rw-)，非 root 用户可无障碍访问。

### 4. 万兆网卡性能验证

- **环形缓冲区 (Ring Buffer)**：通过 `ethtool -g` 验证。
    - **RX/TX 预设最大值**：$4096$
    - **RX/TX 当前设置值**：**$4096$** (已达硬件性能上限)
- **结论**：网络驱动层已完成针对 $1.25$ GB/s 数据流的冗余配置。

### 5. V2.0 新增持久化项

- **新增 `nic-optimization.service`**：
    - 放行 `9999/udp`
    - 将 Intel X710 四口 ring buffer 固定到 `4096`
    - 将 X710 IRQ 固定到 `16-31`
    - 将管理口 IRQ 固定到 `0-15`
    - 脚本实体：`/usr/local/bin/nic-tuning.sh`
- **新增 `cpu-performance.service`**：
    - 将 CPU 16-31 governor 固定为 `performance`
    - 脚本实体：`/usr/local/bin/cpu-performance.sh`
- **新增 `qdgz300-sysctl.service`**：
    - 解决麒麟 `/usr/lib/sysctl.d/kylin.conf` 覆盖 `/etc/sysctl.d/90-qdgz300.conf` 的问题
    - 在 `systemd-sysctl.service` 之后重新应用
- **新增 `/etc/sysctl.d/90-qdgz300.conf`**：
    - 固化 UDP 与网络栈缓冲参数
- **新增内核启动参数** (`/etc/grub2-efi.cfg`)：
    - `isolcpus=16-31 nohz_full=16-31 rcu_nocbs=16-31 audit=0`
- **禁用 `irqbalance.service`**：
    - 避免覆盖手工 IRQ 绑定
- **明确 `receiver_app` 的 `numactl` 启动要求**
- **明确 `enP1s25f3` 进入 `txface4` namespace 后的可见性说明**

### 6. V2.0 现场验证结论 (2026-03-07)

1. `nic-optimization.service` 已启用并正常执行（`enabled / active (exited)`）。
2. `cpu-performance.service` 已启用并正常执行（`enabled / active (exited)`）。
3. `qdgz300-sysctl.service` 已启用并正常执行（`enabled / active (exited)`）。
4. `irqbalance.service` 已禁用（`disabled / inactive`）。
5. `9999/udp` 已持久化放行（`firewall-cmd --list-ports` 确认）。
6. sysctl 参数均已生效（`rmem_max=536870912`, `netdev_max_backlog=250000`）。
7. CPU governor 全部为 `performance`（cpu0/15/16/31 采样验证）。
8. X710 ring buffer 全部为 `RX/TX = 4096`（enP1s25f0/f1 采样验证）。
9. 数据面三口 IP 已配置（`192.168.1.101`/`192.168.2.101`/`192.168.3.101`）。
10. `enP1s25f3` 已进入 `txface4` namespace 并配置为 `192.168.1.102/24`。
11. 管理口 `enP1s24f0` 通过 DHCP 获取 `192.168.31.234/24`。
12. `receiver_app` 正在运行，监听 `192.168.1.101:9999/udp` 和 `0.0.0.0:8080/tcp`。
13. GPU `Iluvatar MR-V100` 正常在线（`ixsmi` 确认，61°C，0% 利用率）。

---

## 风险提示更新 (Risk Re-assessment)

> [!CAUTION] 关键风险：时钟源可靠性
>
> 由于网关（192.168.0.1）拒绝 NTP 请求，当前系统处于"自同步"状态。对于涉及多机协同、或带精确时间戳的雷达原始数据采集任务，必须引入外部专用 NTP 授时服务器（如北斗/GPS 授时源），否则无法保证多节点间的时间确定性。

---

## 第四部分：配置文件/参数设置方法和位置汇总清单

本章节汇总了所有已配置的系统级参数，包括其持久化文件路径、修改方法和验证命令，作为运维人员的快速参考指南。

### 4.1 内核启动参数

| **参数** | **当前值** | **作用** | **配置文件位置** | **修改方法** | **验证命令** |
| --- | --- | --- | --- | --- | --- |
| `isolcpus` | `16-31` | 数据面核心从调度器摘除 | `/etc/grub2-efi.cfg` | 编辑 GRUB 内核命令行后 `grub2-mkconfig` | `cat /proc/cmdline` |
| `nohz_full` | `16-31` | 数据面核心无时钟中断 | `/etc/grub2-efi.cfg` | 同上 | `cat /proc/cmdline` |
| `rcu_nocbs` | `16-31` | RCU 回调卸载至 housekeeping 核心 | `/etc/grub2-efi.cfg` | 同上 | `cat /proc/cmdline` |
| `audit` | `0` | 禁用内核审计子系统 | `/etc/grub2-efi.cfg` | 同上 | `cat /proc/cmdline` |
| `smmu.bypassdev` | `0x1000:0x17`, `0x1000:0x15` | SMMU bypass 特定设备 | `/etc/grub2-efi.cfg` | 同上 | `cat /proc/cmdline` |
| `module_blacklist` | `phytium_mci_pci`, `phytium_mci` | 禁用飞腾 MCI 驱动 | `/etc/grub2-efi.cfg` | 同上 | `cat /proc/cmdline` |

### 4.2 sysctl 网络栈参数

| **参数** | **基线值** | **配置文件位置** | **修改方法** | **验证命令** |
| --- | --- | --- | --- | --- |
| `net.core.rmem_default` | `67108864` | `/etc/sysctl.d/90-qdgz300.conf` | 编辑文件后重启 `qdgz300-sysctl.service` | `sysctl net.core.rmem_default` |
| `net.core.rmem_max` | `536870912` | `/etc/sysctl.d/90-qdgz300.conf` | 同上 | `sysctl net.core.rmem_max` |
| `net.ipv4.udp_rmem_min` | `262144` | `/etc/sysctl.d/90-qdgz300.conf` | 同上 | `sysctl net.ipv4.udp_rmem_min` |
| `net.ipv4.udp_mem` | `262144 1048576 4194304` | `/etc/sysctl.d/90-qdgz300.conf` | 同上 | `sysctl net.ipv4.udp_mem` |
| `net.core.netdev_max_backlog` | `250000` | `/etc/sysctl.d/90-qdgz300.conf` | 同上 | `sysctl net.core.netdev_max_backlog` |

> **注意**：直接 `sysctl -p` 或 `sysctl --system` 后，麒麟的 `/usr/lib/sysctl.d/kylin.conf` 会覆盖上述配置。必须通过 `qdgz300-sysctl.service` 保证最终生效。

### 4.3 systemd 服务单元

| **服务名** | **单元文件位置** | **依赖脚本** | **启用/修改方法** | **验证命令** |
| --- | --- | --- | --- | --- |
| `nic-optimization.service` | `/etc/systemd/system/nic-optimization.service` | `/usr/local/bin/nic-tuning.sh` | 编辑脚本后 `systemctl daemon-reload && systemctl restart nic-optimization` | `systemctl status nic-optimization` |
| `cpu-performance.service` | `/etc/systemd/system/cpu-performance.service` | `/usr/local/bin/cpu-performance.sh` | 同上模式 | `systemctl status cpu-performance` |
| `qdgz300-sysctl.service` | `/etc/systemd/system/qdgz300-sysctl.service` | `/etc/sysctl.d/90-qdgz300.conf` | 编辑 conf 后 `systemctl restart qdgz300-sysctl` | `systemctl status qdgz300-sysctl` |
| `iluvatar-node-init.service` | `/etc/systemd/system/iluvatar-node-init.service` | `/usr/local/bin/init-iluvatar-nodes.sh` | 编辑脚本后 `systemctl daemon-reload` | `ls -la /dev/ix*` |
| `irqbalance.service` | 系统自带 | — | `systemctl disable irqbalance` | `systemctl is-enabled irqbalance` |

### 4.4 网卡与网络配置

| **配置项** | **配置位置/方法** | **关键参数** | **验证命令** |
| --- | --- | --- | --- |
| 管理口 IP (DHCP) | NetworkManager `enP1s24f0` 连接 | `192.168.31.234/24` | `ip addr show enP1s24f0` |
| 数据面 IP (阵面1) | NetworkManager `radar1` 连接 | `192.168.1.101/24` on `enP1s25f0` | `ip addr show enP1s25f0` |
| 数据面 IP (阵面2) | NetworkManager `radar2` 连接 | `192.168.2.101/24` on `enP1s25f1` | `ip addr show enP1s25f1` |
| 数据面 IP (阵面3) | NetworkManager `radar3` 连接 | `192.168.3.101/24` on `enP1s25f2` | `ip addr show enP1s25f2` |
| 自回环发送端 | `txface4` network namespace | `192.168.1.102/24` on `enP1s25f3` | `ip netns exec txface4 ip addr show enP1s25f3` |
| Ring Buffer (X710) | `/usr/local/bin/nic-tuning.sh` → `nic-optimization.service` | `RX/TX = 4096` | `ethtool -g enP1s25f0` |
| IRQ 亲和性 (数据面) | `/usr/local/bin/nic-tuning.sh` → `nic-optimization.service` | CPU 16-31 | `grep enP1s25 /proc/interrupts` |
| IRQ 亲和性 (管理面) | `/usr/local/bin/nic-tuning.sh` → `nic-optimization.service` | CPU 0-15 | `cat /proc/irq/<IRQ>/smp_affinity_list` |
| 防火墙端口放行 | `/usr/local/bin/nic-tuning.sh` → `nic-optimization.service` | `9999/udp` | `firewall-cmd --list-ports` |

### 4.5 CPU 调频策略

| **配置项** | **配置位置** | **关键参数** | **验证命令** |
| --- | --- | --- | --- |
| 数据面 governor | `/usr/local/bin/cpu-performance.sh` → `cpu-performance.service` | CPU 16-31 → `performance` | `cat /sys/devices/system/cpu/cpu16/cpufreq/scaling_governor` |

### 4.6 资源限制

| **配置项** | **配置位置** | **基线值** | **验证命令** |
| --- | --- | --- | --- |
| open files (soft) | `/etc/security/limits.conf` | `65535` | `ulimit -n` |
| open files (hard) | `/etc/security/limits.conf` | `65535` | `ulimit -Hn` |

### 4.7 时钟同步

| **配置项** | **配置位置** | **关键参数** | **验证命令** |
| --- | --- | --- | --- |
| NTP 服务 | `/etc/chrony.conf` | `server 192.168.0.1 iburst` + `local stratum 10` | `chronyc tracking` |
| 时区 | 系统级 | `Asia/Shanghai (CST, +0800)` | `timedatectl` |

### 4.8 GPU/加速卡

| **配置项** | **配置位置** | **关键参数** | **验证命令** |
| --- | --- | --- | --- |
| 设备节点创建 | `/usr/local/bin/init-iluvatar-nodes.sh` → `iluvatar-node-init.service` | `/dev/ix0`, `/dev/ixctl` (mode 666) | `ls -la /dev/ix*` |
| CoreX SDK 路径 | `/usr/local/corex` → `/usr/local/corex-4.3.8/` | 包含 CUDA 10.2 兼容层 | `ixsmi` |
| 环境变量 (devuser) | `~/.bashrc` | `PATH` 含 `/usr/local/corex/bin`；`LD_LIBRARY_PATH` 含 `/usr/local/corex/lib64` | `echo $PATH && echo $LD_LIBRARY_PATH` |

### 4.9 Docker 守护进程

| **配置项** | **配置位置** | **基线值** | **验证命令** |
| --- | --- | --- | --- |
| Cgroup Driver | `/etc/docker/daemon.json` | `systemd` | `docker info \| grep Cgroup` |
| Log Max-size | `/etc/docker/daemon.json` | `100m` (max-file: 3) | `cat /etc/docker/daemon.json` |
| Storage Driver | `/etc/docker/daemon.json` | `overlay2` | `docker info \| grep Storage` |

### 4.10 安全基线

| **配置项** | **配置位置** | **当前状态** | **验证命令** |
| --- | --- | --- | --- |
| SELinux | `/etc/selinux/config` | `Disabled` | `sestatus` |
| firewalld | systemd | `Active` | `systemctl is-active firewalld` |

---
