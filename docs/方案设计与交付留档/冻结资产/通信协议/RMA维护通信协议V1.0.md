---
tags:
aliases:
  - "RMA 维护通信协议 V1.0"
date created: 星期日, 三月 29日 2026
date modified: 星期日, 三月 29日 2026
---

# RMA维护通信协议 V1.0

---

## 0 协议设计思想与宏观架构

### 0.1 系统角色

本协议服务于**非对称主从架构**：

- **SPS（Signal Processing System）**：信号处理系统，唯一的控制发起方（上位机/服务器）
- **DACS（Data Acquisition & Control System）**：数据采集与控制系统，前端 FPGA 设备，仅响应控制并主动推送数据

物理链路为 **10GbE 以太网（UDP/IPv4）**，每个 DACS 阵面通过独立万兆光口直连 SPS。

### 0.2 五平面分离与本文范围

系统整体协议采用平面分离设计，每个平面独立承载不同语义：

| 平面 | 报文类型 | 方向 | 核心语义 | 可靠性策略 |
|------|---------|------|---------|-----------|
| **控制平面** | `0x01` 控制指令 + `0x02` 确认应答 | SPS → DACS | 波束安排周期下发（一个周期内所有波束参数集合） | CRC32C + 确认应答闭环 + 超时重传 |
| **数据平面** | `0x03` 回波数据 | DACS → SPS | 原始 I/Q 回波上传 | 以太网 FCS + 丢包补零 |
| **遥测平面** | `0x04` 心跳状态 | DACS → SPS | 健康状态上报 | 尽力传输，允许丢包 |
| **时基边界平面** | `0x05` PPS Notify | DACS → SPS | 软件秒边界事件上送 | 尽力传输 + 去重/乱序检测 + 失边界降级 |
| **维护平面** | `0xFF` 远程维护 | SPS ↔ DACS | 固件升级/远程维护 | CRC32C + 会话令牌事务闭环 |

本文档专注定义**维护平面（PacketType=`0xFF`）**。控制、数据、遥测平面的字段定义见 [前端感知通信协议v1.0.md](./前端感知通信协议v1.0.md)，时基边界平面定义见 [时基边界事件协议（PPS Notify）V1.0.md](./时基边界事件协议（PPS Notify）V1.0.md)。

### 0.3 核心设计原则

1. **全小端序编码**：所有多字节字段采用小端序（Little-Endian），包括整数、浮点数、时间戳等
2. **协议层禁止浮点数**：维护面字段全部使用整数编码
3. **统一32字节通用报文头**：所有报文类型共享同一通用报文头，通过"报文类型"字段分发到不同处理流程
4. **校验策略**：移除通用头部 CRC16；维护载荷仍以 CRC32C 作为主要完整性校验
5. **四字节自然对齐**：所有数据结构遵循4字节自然对齐，优化DMA传输性能
6. **时间戳统一编码**：全协议仅保留一种时间戳：`uint64` Unix Epoch 毫秒（ms），小端序传输

### 0.4 设备标识

| 设备标识 | 角色 | IP 示例 |
|---------|------|--------|
| `0x00` | DACS 未开通态（UNPROVISIONED，仅允许作为 RMA/0xFF 的源标识） | 10.1.0.10 |
| `0x01` | SPS 主控 | 10.0.1.100 |
| `0x10` | DACS 广播（仅用于目标标识） | — |
| `0x11` | DACS 阵面 01（前向） | 10.0.1.11 |
| `0x12` | DACS 阵面 02（左侧） | 10.0.1.12 |
| `0x13` | DACS 阵面 03（右侧） | 10.0.1.13 |
| `0xF0-0xFE` | 测试设备 | — |
| `0xFF` | 保留，禁止使用 | — |

### 0.5 UDP 端口分配

| 报文类型 | 方向 | 源端口 | 目标端口 |
|---------|------|-------|---------|
| 远程维护 (0xFF) | SPS ↔ DACS | 7777 | 7777 |

### 0.6 协议版本

本手册对应 **协议版本 = `0x10`**（V1.0）。协议版本编码为 `uint8`，高4位=主版本，低4位=次版本。

---

## 1 通用报文头 — RMA 报文共用

### 1.1 包结构

```
┌──────────────────────────────────────────┐
│  通用报文头 (固定 32 字节)                 │
├──────────────────────────────────────────┤
│  后续载荷 (RMA Payload)                  │
└──────────────────────────────────────────┘
```

所有 UDP 报文的前 32 字节均为通用报文头。"载荷长度"字段描述紧随其后的载荷字节数，即 `UDP载荷总长 = 32 + 载荷长度`。

### 1.2 字段表

| 偏移 | 字段名称 | 类型 | 字节数 | 说明 |
|------|---------|------|-------|------|
| **0** | **魔数（Magic）** | `uint32` | 4 | **含义**：帧首同步字（固定值）。<br/>**取值**：`0x55AA55AA`（固定）。<br/>**设计目的**：快速定位帧首；`0x55AA`交替位模式利于硬件对齐。<br/>**表示**：`uint32`，小端序（线上字节：`[AA 55 AA 55]`）。<br/>**处理**：不匹配→静默丢弃。 |
| **4** | **序列号（SequenceNumber）** | `uint32` | 4 | **含义**：逻辑流内递增序列号。<br/>**作用**：丢包检测与链路观测。<br/>**递增作用域**：按`(源标识, 报文类型, 方向)`三元组划分逻辑流（同一逻辑流内对“新报文”递增；重传报文保持序号不变）。<br/>**回绕比较**：必须使用半区间算法（Δ=无符号减法；`0<Δ<2^31`为新包；`Δ==0`重复；`Δ≥2^31`旧包）。 |
| **8** | **时间戳（Timestamp）** | `uint64` | 8 | **含义**：统一时间戳（Unix Epoch，毫秒）。<br/>**按本文档解释**：`0xFF(远程维护)`：请求/响应时刻。<br/>**取值**：`ms_since_epoch`，即自 1970-01-01 00:00:00 UTC 起的毫秒数。<br/>**取值零处理**：允许为 `0`（表示未锁定授时/未实现时统）。 |
| **16** | **载荷长度（PayloadLen）** | `uint16` | 2 | **含义**：后续载荷字节长度（不含32字节报文头）。<br/>**约束**：`载荷长度 ≤ 65475`（`65507-32`）。<br/>**一致性检查**：必须满足`UDP载荷字节数 == 32 + 载荷长度`，否则静默丢弃。<br/>**安全约束**：必须在 Magic/ProtoVer/PacketType/长度一致性等基础检查通过后才允许按该长度访问载荷（防越界）。 |
| **18** | **报文类型（PacketType）** | `uint8` | 1 | **含义**：载荷类型/解析分发键。<br/>**取值**：`0xFF` 远程维护。<br/>**处理**：未知类型→静默丢弃并计数。 |
| **19** | **协议版本（ProtocolVersion）** | `uint8` | 1 | **含义**：协议版本（高4位主版本，低4位次版本）。<br/>**基线**：V1.0 = `0x10`。<br/>**处理**：主版本不匹配→静默丢弃（禁止猜测解析）。<br/>**兼容**：当`协议版本 > 0x10`时，保留字段非零可能表示扩展（需结合版本策略）。 |
| **20** | **源标识（SourceID）** | `uint8` | 1 | **含义**：发送端逻辑标识。<br/>**取值**：<br/>• `0x01` = SPS<br/>• `0x11-0x13` = DACS阵面<br/>• `0x10` 禁止作为源标识（仅用于目标标识广播）<br/>• `0x00` = DACS 未开通态（仅允许在 `PacketType=0xFF (RMA)` 中作为源标识）<br/>• `0xFF` 禁止使用<br/>**设计目的**：逻辑寻址与去重键组成部分（与IP解耦）。 |
| **21** | **目标标识（DestID）** | `uint8` | 1 | **含义**：接收端逻辑标识。<br/>**取值**：<br/>• `0x10` = 逻辑广播（所有DACS）<br/>• 其他取值同源标识（点对点）<br/>• `0xFF` 禁止使用<br/>**处理**：不匹配→静默丢弃（建议NIC/FPGA层早过滤）。 |
| **22** | **控制纪元（ControlEpoch / LinkEpoch）** | `uint16` | 2 | **含义（冻结）**：控制/链路纪元标识。<br/>**禁止互用声明（冻结）**：本字段与 RMA 的 `SessionID` **无关且禁止互用**（不得把 `SessionID` 写入本字段，亦不得把本字段当作 `SessionID` 传递）。 |
| **24** | **扩展标志（ExtFlags）** | `uint8` | 1 | **含义**：通用头扩展标志（保留）。<br/>**基线要求**：V1.0（0x10）发送端应填 `0x00`。<br/>**兼容策略**：接收端在主版本一致时，若该字段非 0，应继续解析已知字段并计数告警（视为小扩展）；主版本不一致则丢弃。 |
| **25** | **保留1（Reserved1）** | `uint8` | 1 | **含义**：对齐保留（Reserved）。<br/>**发送端**：应填 `0x00`。<br/>**接收端**：主版本一致时允许非 0（计数告警，继续解析）。 |
| **26** | **保留2（Reserved2）** | `uint16` | 2 | **含义**：保留（Reserved）。用于未来扩展（例如：头部特性协商/能力标识）。<br/>**发送端**：应填 `0x0000`。<br/>**接收端**：主版本一致时允许非 0（计数告警，继续解析）。 |
| **28** | **保留3（Reserved3）** | `uint8[4]` | 4 | **含义**：对齐与扩展保留（Reserved）。<br/>**发送端**：应填全 `0x00`。<br/>**接收端**：主版本一致时允许非 0（计数告警，继续解析）。 |

**总长度 = 32 字节**

### 1.2.1 Timestamp 语义矩阵（冻结）

> 说明：通用头 `Timestamp` 只有一种编码（Unix Epoch ms），但本文档只定义维护平面的解释方式。

| PacketType | 报文名称 | Common Header.Timestamp 语义 | 允许为 0 | 0 的语义 |
|---|---|---|---|---|
| `0xFF` | RMA | **请求/响应发生时刻** | 允许 | `0` 表示“时间不可用/未锁定/未实现” |

### 1.2.2 CRC 策略矩阵（冻结）

> 说明：V1.0 **移除通用头 CRC16**，维护平面载荷内含 CRC32C。

| PacketType | 报文名称 | 是否含 CRC32C 字段 | CRC32C 位置 | CRC32C 覆盖范围（相对本报文载荷起始 Offset） | 校验失败处理 |
|---|---|---|---|---|---|
| `0xFF` | RMA | 是 | RMA Payload 末尾 4B | `0 .. (PayloadLen-5)`（不含 CRC 字段本身） | 失败→静默丢弃（不得执行、不得回包） |

---

## 2 RMA Header 与核心命令（维护平面）— PacketType = 0xFF

### 2.1 包结构

```
┌──────────────────────────────────────────────────────────┐
│  UDP Payload (Total: 32 + RMA_Payload_Size)              │
├──────────────────────────────────────────────────────────┤
│  [0..31]     Common Header (32B)                        │
│              PacketType = 0xFF                           │
│              PayloadLen = RMA_Payload_Size               │
├──────────────────────────────────────────────────────────┤
│  [32..]      RMA Payload (变长)                          │
│              RMA Header (16B)                            │
│              Command-Specific Fields (变长)              │
│              CRC32C (4B，最后 4 字节)                     │
└──────────────────────────────────────────────────────────┘
```

RMA 为 SPS 与 DACS 之间的双向维护通道，支持会话管理与阶段化固件升级。采用 Token 机制防止会话劫持。阶段模型：`IDLE → PREPARE → TRANSFER → VERIFY → COMMIT → ACTIVATE`，严禁跳跃。

### 2.2 校验流程说明（三层保障）

固件升级流程采用"**单块校验 + 立即存储 + 最终完整性校验**"的三层保障模式：

| 阶段 | 操作 | 触发点 | 失败处理 |
|------|------|--------|----------|
| **TRANSFER（传输）** | 接收 Chunk → 验证 `ChunkCRC32C` → 通过则立即写入 Flash | 每接收一个 Chunk | `ERR_CHUNK_CRC_FAILED (0x20)`：拒绝存储，SPS 可重试或中止 |
| | | | `ERR_FLASH_WRITE_FAILED (0x32)`：CRC 通过但写入失败，中止传输 |
| **COMMIT（提交）** | 所有 Chunk 接收完后，对 Flash 中的完整镜像计算 CRC32C | 最后一个 Chunk 接收完毕 | `ERR_FIRMWARE_CRC_FAILED (0x24)`：整体校验失败，镜像不一致，需重新升级 |
| **ACTIVATE（激活）** | 校验通过后，激活新固件（切换槽位或重启） | 用户确认或自动触发 | `ERR_ACTIVATION_FAILED (0x34)`：激活失败，回滚或重试 |

---

### 2.3 RMA Header 字段表

> Offset 基准：相对于 Common Header 结束位置（UDP Payload Offset 32）

**RMA Header 固定长度 = 16 字节**（不额外保留扩展字段；满足 4 字节对齐）

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **0** | **会话ID（SessionID）** | `uint32` | 4 | **含义**：RMA 会话标识符。<br/>**来源**：会话建立后由 DACS 返回并生效。<br/>**编码建议**：高 16 位=Epoch，低 16 位=递增序列。<br/>**禁用值**：`0x00000000`、`0xFFFFFFFF`。<br/>**使用**：`START_SESSION` 请求中填 0；其余命令必须携带有效 SessionID。<br/>**术语隔离（冻结）**：本字段与 Common Header 的 `ControlEpoch/LinkEpoch` 无关且禁止互用。<br/>**约束**：同一 DACS 同一时刻最多 1 个活动会话。 |
| **4** | **会话令牌（SessionToken）** | `uint64` | 8 | **含义**：会话安全令牌（防会话劫持/冒用）。<br/>**生成**：由 DACS 在 `START_SESSION_ACK` 中用 CSPRNG 生成并返回。<br/>**使用**：除 `START_SESSION` 外，所有 RMA 命令必须携带正确的 `(SessionID, SessionToken)`。<br/>**处理**：Token 不匹配→拒绝并返回 `ERR_INVALID_TOKEN (0x40)`；同一 SourceID 累计 5 次失败→锁定 10 秒，返回 `ERR_AUTH_LOCKED (0x45)`（锁定期间含 START_SESSION）。<br/>**取值**：`START_SESSION` 请求中填 0。 |
| **12** | **命令类型（CmdType）** | `uint8` | 1 | **含义**：命令类型（请求/响应分族）。<br/>**请求（SPS→DACS）**：<br/>• `0x01` START_SESSION<br/>• `0x02` TERMINATE_SESSION<br/>• `0x03` QUERY_STATUS<br/>• `0x10` PREPARE_UPGRADE<br/>• `0x11` BEGIN_TRANSFER<br/>• `0x12` WRITE_CHUNK<br/>• `0x13` COMMIT_FIRMWARE<br/>• `0x14` ACTIVATE_FIRMWARE<br/>• `0x20` PROV_QUERY_IDENTITY<br/>• `0x21` PROV_STAGE_CONFIG<br/>• `0x22` PROV_COMMIT_APPLY<br/>• `0x23` PROV_VERIFY<br/>• `0x1F` ABORT<br/>**响应（DACS→SPS）**：<br/>• `0x81` START_SESSION_ACK<br/>• `0x82` TERMINATE_SESSION_ACK<br/>• `0x83` STATUS_REPLY<br/>• `0x90` PREPARE_ACK<br/>• `0x91` BEGIN_TRANSFER_ACK<br/>• `0x92` WRITE_CHUNK_ACK<br/>• `0x93` COMMIT_ACK<br/>• `0x94` ACTIVATE_ACK<br/>• `0xA0` PROV_IDENTITY_REPLY<br/>• `0xA1` PROV_STAGE_ACK<br/>• `0xA2` PROV_COMMIT_ACK<br/>• `0xA3` PROV_VERIFY_REPLY<br/>• `0x9F` ABORT_ACK<br/>• `0xFF` ERROR_REPLY<br/>**处理**：未知 CmdType → 必须拒绝并返回 `ERR_INVALID_CMD (0x12)`。 |
| **13** | **命令标志（CmdFlags）** | `uint8` | 1 | **含义**：命令标志位。<br/>**位定义**：<br/>• Bit0 RequireAck：1=需要响应（当前所有 RMA 命令默认需要响应）<br/>• Bit1-7 Reserved（发送端置 0；接收端忽略并计数告警） |
| **14** | **命令序列号（CmdSeq）** | `uint16` | 2 | **含义**：命令序列号（幂等去重）。<br/>**去重键（SSOT）**：`(SessionID, CmdType, CmdSeq)`。<br/>**处理**：重复命令→返回缓存响应，不得重复执行。<br/>**回显**：DACS 响应必须回显该值。 |

**通用约束**：
- RMA Payload 的最后 4 字节固定为 CRC32C
- 除 `ChunkData` 外，所有字段按 4 字节自然对齐；如遇 `uint8/uint16` 造成非 4 对齐，使用**最少量** Reserved_Align 补齐到最近的 4 的倍数

### 2.4 START_SESSION (0x01) 请求

**PayloadLen 固定 = 16 (RMA Header) + 12 (Body) + 4 (CRC32C) = 32 字节**

> Offset 基准：相对于 RMA Payload 起始位置（即 RMA Header 起始，UDP Payload Offset 32）

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **请求会话ID（RequestedSessionID）** | `uint32` | 4 | **含义**：SPS 请求的 SessionID（建议值）。<br/>**使用**：DACS 接受后可采用该值建立会话，并在 `START_SESSION_ACK` 中回传最终 SessionID（以响应为准）。 |
| **20** | **能力位图（Capabilities）** | `uint32` | 4 | **含义**：SPS 能力位图（能力协商）。<br/>**取值**：各 bit 语义由实现约定；填 0 表示“不声明额外能力”。 |
| **24** | **最大Chunk大小（MaxChunkSize）** | `uint32` | 4 | **含义**：SPS 支持的最大 Chunk 数据长度（字节）。<br/>**范围**：4096 ~ 65536。<br/>**协商**：DACS 返回实际采用值（通常取两端较小值）。 |
| **28** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。<br/>**覆盖范围**：RMA Payload 起始至 CRC32C 前一字节。<br/>**处理**：失败→静默丢弃（不得执行、不得回包）。 |

### 2.5 START_SESSION_ACK (0x81) 响应

**PayloadLen 固定 = 16 (RMA Header) + 24 (Body) + 4 (CRC32C) = 44 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：会话建立结果。<br/>**取值**：<br/>• `0x0000` 成功<br/>• `0x0042` ERR_SESSION_BUSY（已有活动会话）<br/>• `0x0041` ERR_SESSION_EXPIRED（请求已过期/不接受） |
| **18** | **对齐保留1（Reserved_Align1）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **20** | **设备能力位图（DeviceCapabilities）** | `uint32` | 4 | **含义**：DACS 能力位图（能力协商）。<br/>**取值**：bit 语义由实现约定。 |
| **24** | **协商Chunk大小（MaxChunkSize）** | `uint32` | 4 | **含义**：协商后的 Chunk 大小（字节）。<br/>**规则**：取 SPS 请求与 DACS 上限的较小值（以本字段为准）。 |
| **28** | **Flash大小（FlashSize）** | `uint64` | 8 | **含义**：可用 Flash 空间（字节）。<br/>**用途**：SPS 预检查是否具备固件落盘空间。 |
| **36** | **固件槽位数（FirmwareSlots）** | `uint8` | 1 | **含义**：固件槽位数。<br/>**典型**：2（主/备双槽位）。 |
| **37** | **当前槽位（CurrentSlot）** | `uint8` | 1 | **含义**：当前激活槽位编号。<br/>**取值**：0/1（实现可能扩展）。 |
| **38** | **对齐保留2（Reserved_Align2）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **40** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。<br/>**覆盖范围**：RMA Payload 起始至 CRC32C 前一字节。 |

### 2.6 PREPARE_UPGRADE (0x10) 请求

**PayloadLen 固定 = 16 (RMA Header) + 20 (Body) + 4 (CRC32C) = 40 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **固件大小（FirmwareSize）** | `uint64` | 8 | **含义**：固件镜像总大小（字节）。<br/>**用途**：DACS 预分配/对齐擦除块。 |
| **24** | **固件版本（FirmwareVersion）** | `uint32` | 4 | **含义**：目标固件版本号（发布标识）。 |
| **28** | **固件CRC32C（FirmwareCRC32C）** | `uint32` | 4 | **含义**：整镜像 CRC32C（用于 VERIFY 阶段一致性校验）。<br/>**用途**：DACS 对完整接收数据计算 CRC32C 并与此值比较。 |
| **32** | **目标槽位（TargetSlot）** | `uint8` | 1 | **含义**：目标写入槽位选择。<br/>**取值**：<br/>• `0` 自动（通常选择"非当前激活槽位"）<br/>• `1`/`2` 指定槽位（实现相关）<br/>**用途**：支持多个固件版本并存与灰度升级。 |
| **33** | **激活策略（ActivationPolicy）** | `uint8` | 1 | **含义**：激活策略。<br/>**取值**：<br/>• `0x01` ACTIVATE_IMMEDIATE：提交后立即激活/重启<br/>• `0x02` ACTIVATE_ON_REBOOT：标记待激活，下次重启生效<br/>• `0x03` ACTIVATE_MANUAL：等待单独 ACTIVATE 命令<br/>• `0x04` ACTIVATE_TIMED：指定时间点激活 |
| **34** | **数据类型（DataType）** | `uint8` | 1 | **含义**：固件数据形式类型，明确所承载的数据结构类型。<br/>**取值**：<br/>• `0x00` 原始二进制（未指定数据结构；向后兼容）<br/>• `0x01` 波形数据（Waveform）：包含 8 个波形采样通道数据，支持多通道多功能探测<br/>• `0x02` DA 数据（Digital Amplitude）：包含数幅度编码数据，用于直接调制<br/>**示例定义**：<br/>  + 波形数据：`{ChannelCount=8, SampleRate, BitWidth, Payload}`<br/>  + DA 数据：`{AmplitudeResolution, EncodingScheme, Payload}`<br/>**影响**：DACS 依此选择合适的存储结构与验证算法。 |
| **35** | **波形编码（WaveformEncoding）** | `uint8` | 1 | **含义**：波形编码（用于波形数据升级）。<br/>**位定义**：<br/>• **D7–D4（高四位）**：长/短码选择：`0`=长码，`1`=短码，其余保留<br/>• **D3–D0（低四位）**：8 种波形编号（固定 0~7）：<br/>&nbsp;&nbsp;• `0` = 波形0<br/>&nbsp;&nbsp;• `1` = 波形1<br/>&nbsp;&nbsp;• `2` = 波形2<br/>&nbsp;&nbsp;• `3` = 波形3<br/>&nbsp;&nbsp;• `4` = 波形4<br/>&nbsp;&nbsp;• `5` = 波形5<br/>&nbsp;&nbsp;• `6` = 波形6<br/>&nbsp;&nbsp;• `7` = 波形7<br/>**约束**：<br/>• 仅当 `DataType=0x01`（波形数据）时该字段有效；否则必须填 0<br/>• 低四位仅允许 0~7 |
| **36** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.7 PREPARE_ACK (0x90) 响应

**PayloadLen 固定 = 16 (RMA Header) + 16 (Body) + 4 (CRC32C) = 36 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：准备阶段结果。<br/>**取值**：<br/>• `0x0000` 成功<br/>• `0x0031` ERR_INSUFFICIENT_SPACE（空间不足） |
| **18** | **对齐保留1（Reserved_Align1）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **20** | **分配缓冲区大小（AllocatedBufferSize）** | `uint64` | 8 | **含义**：DACS 分配的缓冲区大小（字节）。<br/>**注意**：可能 ≥ FirmwareSize（按擦除块对齐）。 |
| **28** | **总Chunk数（TotalChunks）** | `uint32` | 4 | **含义**：预计 Chunk 总数。<br/>**计算**：`ceil(FirmwareSize / MaxChunkSize)`。<br/>**用途**：SPS 跟踪传输进度。 |
| **32** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.8 WRITE_CHUNK (0x12) 请求

**PayloadLen 变长 = 16 (RMA Header) + 24 (BodyFixed) + N (ChunkData) + M (Padding) + 4 (CRC32C)**

其中：
- `N = ChunkLength`
- `M = (4 - (N mod 4)) mod 4`（补齐到最近的 4 的倍数；全 0；不计入 ChunkLength）

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **Chunk序号（ChunkIndex）** | `uint32` | 4 | **含义**：Chunk 序号。<br/>**取值**：从 0 开始递增；允许乱序到达。<br/>**用途**：DACS 以 Index 组织/去重 Chunk。 |
| **20** | **Chunk偏移（ChunkOffset）** | `uint64` | 8 | **含义**：固件镜像内字节偏移。<br/>**用途**：DACS 按 Offset 将数据写入正确位置（支持乱序/窗口）。 |
| **28** | **Chunk长度（ChunkLength）** | `uint32` | 4 | **含义**：当前 Chunk 有效数据长度（字节）。<br/>**约束**：`ChunkLength ≤ MaxChunkSize`；最后一个 Chunk 可小于 MaxChunkSize。<br/>**使用**：仅 `ChunkData[0..ChunkLength-1]` 为有效数据。 |
| **32** | **Chunk校验（ChunkCRC32C）** | `uint32` | 4 | **含义**：当前 ChunkData 的 CRC32C（单块校验）。<br/>**覆盖范围**：仅 ChunkData 部分（Offset 40 到 40+ChunkLength-1）。<br/>**处理**：校验失败→拒绝该 Chunk **不存入 Flash**，返回 `ERR_CHUNK_CRC_FAILED (0x20)`；SPS 可重试或中止。 |
| **36** | **Chunk标志（ChunkFlags）** | `uint8` | 1 | **含义**：Chunk 标志位。<br/>**位定义**：<br/>• Bit0 FirstChunk（首个 Chunk）<br/>• Bit1 LastChunk（最后一个 Chunk；收到后可触发 TRANSFER→VERIFY）<br/>• Bit2-7 Reserved（发送端置 0；接收端忽略并计数告警） |
| **37** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（Reserved）。<br/>**发送端**：应填全 `0x00`。<br/>**接收端**：主版本一致时允许非 0（计数告警，继续解析）。 |
| **40** | **Chunk数据（ChunkData）** | `uint8[N]` | N | **含义**：固件数据载荷。<br/>**约束**：`N == ChunkLength` 且 `N ≤ MaxChunkSize`。 |
| **40+N** | **对齐填充（Padding）** | `uint8[M]` | M | **含义**：对齐填充（MBZ）。<br/>**规则**：填充 0 至 4 字节边界（不计入 ChunkLength）。 |
| **末尾** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.9 WRITE_CHUNK_ACK (0x92) 响应

**PayloadLen 固定 = 16 (RMA Header) + 20 (Body) + 4 (CRC32C) = 40 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：Chunk 写入/接收结果。<br/>**取值**：<br/>• `0x0000` 成功（已通过单块 CRC 校验并存入 Flash）<br/>• `0x0020` ERR_CHUNK_CRC_FAILED（单块校验失败，不存入 Flash）<br/>• `0x0021` ERR_CHUNK_LENGTH_MISMATCH<br/>• `0x0032` ERR_FLASH_WRITE_FAILED（校验通过但 Flash 写入失败） |
| **18** | **对齐保留1（Reserved_Align1）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **20** | **确认Chunk序号（AckedChunkIndex）** | `uint32` | 4 | **含义**：被确认的 Chunk 序号（回显 ChunkIndex）。 |
| **24** | **Flash写入状态（FlashWriteStatus）** | `uint8` | 1 | **含义**：Flash 写入状态（单块写入结果）。<br/>**取值**：<br/>• `0x00` 未存储（CRC 失败或其他原因）<br/>• `0x01` 成功存储<br/>• `0x02` 存储进行中（用于大块异步写入）<br/>• `0x03` 存储失败（硬件错误） |
| **25** | **进度（Progress）** | `uint8` | 1 | **含义**：进度百分比。<br/>**范围**：0~100。 |
| **26** | **对齐保留2（Reserved_Align2）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **28** | **已接收字节数（ReceivedBytes）** | `uint64` | 8 | **含义**：累计接收字节数（含本 Chunk）。<br/>**用途**：SPS 计算整体传输进度。 |
| **36** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.10 COMMIT_FIRMWARE (0x13) 请求

**PayloadLen 固定 = 16 (RMA Header) + 4 (Body) + 4 (CRC32C) = 24 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **校验模式（VerifyMode）** | `uint8` | 1 | **含义**：验证模式选择。<br/>**取值**：<br/>• `0x00` AUTO（自动）<br/>• `0x01` SKIP_VERIFY（跳过整体校验；谨慎使用）<br/>• `0x02` FORCE_VERIFY（强制完全校验） |
| **17** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **20** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.11 COMMIT_ACK (0x93) 响应

**PayloadLen 固定 = 16 (RMA Header) + 16 (Body) + 4 (CRC32C) = 36 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：提交结果。<br/>**取值**：<br/>• `0x0000` 成功<br/>• `0x0024` ERR_FIRMWARE_CRC_FAILED（整体校验失败）<br/>• `0x0032` ERR_FLASH_WRITE_FAILED（Flash 操作失败） |
| **18** | **校验状态（VerifyStatus）** | `uint8` | 1 | **含义**：校验进度/状态。<br/>**取值**：`0x00` 未开始，`0x01` 进行中，`0x02` 通过，`0x03` 失败。 |
| **19** | **对齐保留1（Reserved_Align1）** | `uint8` | 1 | **含义**：对齐保留（MBZ）。 |
| **20** | **计算CRC32C（CalculatedCRC32C）** | `uint32` | 4 | **含义**：DACS 计算得出的完整镜像 CRC32C（可选回传）。 |
| **24** | **剩余Flash空间（FlashFreeSpace）** | `uint64` | 8 | **含义**：提交后剩余 Flash 空间（字节）。 |
| **32** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.12 ACTIVATE_FIRMWARE (0x14) 请求

**PayloadLen 固定 = 16 (RMA Header) + 4 (Body) + 4 (CRC32C) = 24 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **激活延迟（ActivateDelay）** | `uint32` | 4 | **含义**：激活延迟（秒）。<br/>**取值**：<br/>• `0` 立即激活<br/>• `>0` 延迟指定秒数后激活（可用于协调多设备同步）。 |
| **20** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.13 ACTIVATE_ACK (0x94) 响应

**PayloadLen 固定 = 16 (RMA Header) + 8 (Body) + 4 (CRC32C) = 28 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：激活结果。<br/>**取值**：<br/>• `0x0000` 成功<br/>• `0x0034` ERR_ACTIVATION_FAILED<br/>• `0x0035` ERR_DEVICE_FAULT |
| **18** | **激活状态（ActivationStatus）** | `uint8` | 1 | **含义**：激活状态。<br/>**取值**：`0x00` 未激活，`0x01` 进行中，`0x02` 成功，`0x03` 失败。 |
| **19** | **新槽位（NewSlot）** | `uint8` | 1 | **含义**：新激活的槽位编号。 |
| **20** | **固件版本（FirmwareVersion）** | `uint32` | 4 | **含义**：新激活的固件版本。 |
| **24** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.14 QUERY_STATUS (0x03) 请求

**PayloadLen 固定 = 16 (RMA Header) + 4 (Body) + 4 (CRC32C) = 24 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **查询标志（QueryFlags）** | `uint32` | 4 | **含义**：查询选项位图。<br/>**取值**：bit 语义由实现定义；`0` 表示“返回全部可用信息”。 |
| **20** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.15 STATUS_REPLY (0x83) 响应

**PayloadLen 固定 = 16 (RMA Header) + 36 (Body) + 4 (CRC32C) = 56 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **当前阶段（CurrentPhase）** | `uint8` | 1 | **含义**：当前阶段枚举。<br/>**取值**：<br/>• `0x00` IDLE<br/>• `0x01` PREPARE<br/>• `0x02` TRANSFER<br/>• `0x03` VERIFY<br/>• `0x04` COMMIT<br/>• `0x05` ACTIVATE<br/>• `0xFF` FAILED |
| **17** | **进度（Progress）** | `uint8` | 1 | **含义**：当前阶段内进度。<br/>**范围**：0~100%。 |
| **18** | **最近错误码（LastErrorCode）** | `uint16` | 2 | **含义**：最近一次错误码（用于定位失败原因）。 |
| **20** | **会话持续时间（SessionUptime）** | `uint32` | 4 | **含义**：会话持续时间（秒）。 |
| **24** | **已接收Chunks（ReceivedChunks）** | `uint32` | 4 | **含义**：已接收 Chunk 数。 |
| **28** | **总Chunks（TotalChunks）** | `uint32` | 4 | **含义**：总 Chunk 数。 |
| **32** | **已接收字节数（ReceivedBytes）** | `uint64` | 8 | **含义**：已接收字节数。 |
| **40** | **总字节数（TotalBytes）** | `uint64` | 8 | **含义**：总字节数（通常等于 FirmwareSize）。 |
| **48** | **校验状态（VerifyStatus）** | `uint8` | 1 | **含义**：校验状态枚举。<br/>**取值**：`0x00` 未开始，`0x01` 进行中，`0x02` 通过，`0x03` 失败。 |
| **49** | **提交状态（CommitStatus）** | `uint8` | 1 | **含义**：提交状态枚举。<br/>**取值**：`0x00` 未开始，`0x01` 进行中，`0x02` 成功，`0x03` 失败。 |
| **50** | **激活状态（ActivateStatus）** | `uint8` | 1 | **含义**：激活状态枚举。<br/>**取值**：`0x00` 未开始，`0x01` 进行中，`0x02` 成功，`0x03` 失败。 |
| **51** | **对齐保留1（Reserved_Align1）** | `uint8` | 1 | **含义**：对齐保留（MBZ）。 |
| **52** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

---

### 2.16 开通模式（Provisioning Mode）— 基准建立与网络/逻辑ID写入（RMA 扩展 CmdType=0x20~0x23）

#### 2.16.1 适用范围与目标

开通模式用于解决“出厂设备网络参数相同，安装现场需逐台写入运行态 IP 与逻辑 SourceID”的工程矛盾：

- **承载平面**：仅使用维护平面 RMA（PacketType=`0xFF`，UDP 端口 7777），不新增 PacketType/端口
- **写入目标**：运行态 IP/Netmask/Gateway 与运行态 SourceID（示例：`0x11/0x12/0x13`）
- **闭环要求**：提交后必须在新 IP 上完成 VERIFY，避免“切换后失联不可找回”

#### 2.16.2 未开通态标识与接收规则（关键约束）

- 未开通态 DACS：`SourceID = 0x00`（仅允许出现在 `PacketType=0xFF (RMA)`；其它 PacketType 出现必须丢弃）
- SPS → 未开通态 DACS 发送 RMA：推荐 `DestID = 0x10`（DACS 逻辑广播目标）
- 未开通态 DACS 的接收规则（用于开通阶段）：
   - 当 `PacketType=0xFF` 且 `DestID=0x10` 时允许进入 RMA 处理流程
   - 其它 PacketType 仍按“目标标识不匹配→静默丢弃”的通用规则处理

> 工程建议：现场开通时同一物理口仅连接 1 台未开通 DACS；否则广播目标可能导致多台同时响应。

#### 2.16.3 开通流程（现场闭环）

1. 物理上仅连接 1 台待开通 DACS 至 SPS 对应 10GbE 口
2. SPS 侧需确保可达出厂默认地址（部署细节如临时 secondary IP/临时路由不写入 ICD）
3. SPS → DACS（出厂 IP）发送 `START_SESSION (0x01)` 建立会话（获取 SessionID/Token）
4. SPS → DACS 发送 `PROV_QUERY_IDENTITY (0x20)`，读取 MAC/SN/当前 SourceID/IP 等用于防接错
5. SPS → DACS 发送 `PROV_STAGE_CONFIG (0x21)` 写入“待生效配置”（不立刻切网）
6. SPS 校验 `PROV_STAGE_ACK (0xA1)` 回传的 `StagedConfigCRC32C` 与期望一致
7. SPS → DACS 发送 `PROV_COMMIT_APPLY (0x22)` 提交并应用（可能导致网络栈软重启或整机重启）
8. SPS 等待 `PROV_COMMIT_ACK (0xA2)` 的 `ApplyEtaMs` 后，在**新 IP** 上发送 `PROV_VERIFY (0x23)`，收到 `PROV_VERIFY_REPLY (0xA3)` 视为开通完成
9. SPS → DACS 发送 `TERMINATE_SESSION (0x02)` 结束会话

失败处理（最小要求）：若 VERIFY 失败，操作员应回到“可达出厂地址”的网络配置并重新执行开通流程。实现可选支持“短暂回退窗口/双地址监听”以提升现场容错，但不作为协议强制要求。

---

### 2.17 PROV_QUERY_IDENTITY (0x20) 请求

**PayloadLen 固定 = 16 (RMA Header) + 0 (Body) + 4 (CRC32C) = 20 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。<br/>**覆盖范围**：RMA Payload 起始至 CRC32C 前一字节。 |

### 2.18 PROV_IDENTITY_REPLY (0xA0) 响应

**PayloadLen 固定 = 16 (RMA Header) + 32 (Body) + 4 (CRC32C) = 52 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **当前源标识（CurrentSourceID）** | `uint8` | 1 | **含义**：当前运行态源标识。<br/>**取值**：未开通=`0x00`；已开通=`0x11~0x13`。 |
| **17** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **20** | **当前IP（CurrentIP）** | `uint32` | 4 | **含义**：当前 IPv4 地址（小端序编码）。 |
| **24** | **子网掩码（Netmask）** | `uint32` | 4 | **含义**：子网掩码（小端序编码）。 |
| **28** | **网关（Gateway）** | `uint32` | 4 | **含义**：网关（小端序编码）。点对点可为 `0`。 |
| **32** | **状态标志（Flags）** | `uint16` | 2 | **含义**：状态标志位。<br/>**位定义**：<br/>• Bit0 IsProvisioned：1=已开通（运行态参数有效）<br/>• Bit1 HasStagedConfig：1=存在待生效 staged 配置<br/>• Bit2 LockedBySession：1=当前会话持有开通锁（防并发写入）<br/>• Bit3-15 Reserved（MBZ） |
| **34** | **对齐保留2（Reserved_Align2）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **36** | **MAC地址（MAC）** | `uint8[6]` | 6 | **含义**：设备 MAC 地址。 |
| **42** | **对齐保留3（Reserved_Align3）** | `uint8[2]` | 2 | **含义**：对齐保留（MBZ）。 |
| **44** | **设备序列号（DeviceSN）** | `uint32` | 4 | **含义**：设备序列号（若无则填 0）。用于现场记录/追溯。 |
| **48** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

---

### 2.19 PROV_STAGE_CONFIG (0x21) 请求（写入待生效配置）

**PayloadLen 固定 = 16 (RMA Header) + 32 (Body) + 4 (CRC32C) = 52 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **新源标识（NewSourceID）** | `uint8` | 1 | **含义**：目标运行态 SourceID。<br/>**典型**：`0x11/0x12/0x13`。 |
| **17** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **20** | **新IP（NewIP）** | `uint32` | 4 | **含义**：目标运行态 IPv4（小端序编码）。 |
| **24** | **子网掩码（Netmask）** | `uint32` | 4 | **含义**：目标子网掩码（小端序编码）。 |
| **28** | **网关（Gateway）** | `uint32` | 4 | **含义**：目标网关（小端序编码）。点对点可为 `0`。 |
| **32** | **应用策略（ApplyPolicy）** | `uint8` | 1 | **含义**：写入策略。<br/>**取值**：<br/>• `0` StageOnly（仅写入 staged，不切换）<br/>• `1` Stage+ApplyAfterCommit（提交后应用；与 `PROV_COMMIT_APPLY` 配合） |
| **33** | **对齐保留2（Reserved_Align2）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **36** | **配置CRC32C（ConfigCRC32C）** | `uint32` | 4 | **含义**：对“配置块”计算的 CRC32C（用于写入一致性确认）。<br/>**覆盖范围**：Offset 16..35（从 NewSourceID 到 Reserved_Align2）。 |
| **40** | **保留（Reserved）** | `uint32[2]` | 8 | **含义**：保留扩展（MBZ）。 |
| **48** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.20 PROV_STAGE_ACK (0xA1) 响应

**PayloadLen 固定 = 16 (RMA Header) + 16 (Body) + 4 (CRC32C) = 36 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：写入 staged 结果。<br/>**取值**：0=成功；非 0 参见 RMA 错误码。 |
| **18** | **对齐保留1（Reserved_Align1）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **20** | **Staged配置CRC32C（StagedConfigCRC32C）** | `uint32` | 4 | **含义**：DACS 计算得到的 staged 配置 CRC32C（应与请求的 ConfigCRC32C 一致）。 |
| **24** | **保留（Reserved）** | `uint32[2]` | 8 | **含义**：保留（MBZ）。 |
| **32** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

---

### 2.21 PROV_COMMIT_APPLY (0x22) 请求（提交并应用）

**PayloadLen 固定 = 16 (RMA Header) + 16 (Body) + 4 (CRC32C) = 36 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **期望StagedCRC32C（ExpectedStagedCRC32C）** | `uint32` | 4 | **含义**：期望的 staged CRC32C。<br/>**用途**：防止“提交的不是你以为的那份 staged 配置”。 |
| **20** | **切换动作（SwitchAction）** | `uint8` | 1 | **含义**：切换动作。<br/>**取值**：<br/>• `0` 立即切换 IP，不重启<br/>• `1` 切换 IP 并软重启网络栈<br/>• `2` 整机重启 |
| **21** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **24** | **保留（Reserved）** | `uint32[2]` | 8 | **含义**：保留（MBZ）。 |
| **32** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.22 PROV_COMMIT_ACK (0xA2) 响应

**PayloadLen 固定 = 16 (RMA Header) + 16 (Body) + 4 (CRC32C) = 36 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **错误码（ErrorCode）** | `uint16` | 2 | **含义**：提交/应用结果。<br/>**取值**：0=成功；非 0 参见 RMA 错误码。 |
| **18** | **对齐保留1（Reserved_Align1）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **20** | **预计生效等待时间（ApplyEtaMs）** | `uint32` | 4 | **含义**：预计新配置生效所需等待时间（ms）。SPS 用于决定何时进入 VERIFY。 |
| **24** | **保留（Reserved）** | `uint32[2]` | 8 | **含义**：保留（MBZ）。 |
| **32** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

---

### 2.23 PROV_VERIFY (0x23) 请求（新 IP 上验收）

**PayloadLen 固定 = 16 (RMA Header) + 0 (Body) + 4 (CRC32C) = 20 字节**

> 发送约束：SPS 必须在**新 IP**上发送本请求，且 `DestID` 必须等于目标运行态 `NewSourceID`。

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

### 2.24 PROV_VERIFY_REPLY (0xA3) 响应（新 IP 上返回）

**PayloadLen 固定 = 16 (RMA Header) + 32 (Body) + 4 (CRC32C) = 52 字节**

| Offset | Field | Type | Bytes | 说明 |
|--------|-------|------|-------|------|
| **16** | **生效源标识（ActiveSourceID）** | `uint8` | 1 | **含义**：当前生效的运行态 SourceID。 |
| **17** | **对齐保留1（Reserved_Align1）** | `uint8[3]` | 3 | **含义**：对齐保留（MBZ）。 |
| **20** | **生效IP（ActiveIP）** | `uint32` | 4 | **含义**：当前生效的 IPv4（小端序编码）。 |
| **24** | **子网掩码（Netmask）** | `uint32` | 4 | **含义**：当前生效的子网掩码（小端序编码）。 |
| **28** | **网关（Gateway）** | `uint32` | 4 | **含义**：当前生效的网关（小端序编码）。 |
| **32** | **提交计数器（CommitCounter）** | `uint32` | 4 | **含义**：提交计数器（每次 PROV_COMMIT_APPLY 成功后 +1）。用于现场排查与追溯。 |
| **36** | **最近错误码（LastErrorCode）** | `uint16` | 2 | **含义**：最近一次开通相关错误码（可选；无则 0）。 |
| **38** | **对齐保留2（Reserved_Align2）** | `uint16` | 2 | **含义**：对齐保留（MBZ）。 |
| **40** | **保留（Reserved）** | `uint32[2]` | 8 | **含义**：保留（MBZ）。 |
| **48** | **CRC32C（CRC32C）** | `uint32` | 4 | **含义**：整包 CRC32C。 |

---

## 3 RMA 错误码速查

| 范围 | 分类 | 错误码 | 名称 | 说明 |
|------|------|-------|------|------|
| `0x00` | — | `0x00` | ERR_NONE | 无错误 |
| `0x10-0x1F` | Protocol | `0x10` | ERR_INVALID_PHASE | 当前阶段不允许此命令 |
| | | `0x11` | ERR_PHASE_REGRESSION | 非法阶段回退 |
| | | `0x12` | ERR_INVALID_CMD | 未知命令类型 |
| | | `0x13` | ERR_INVALID_SEQ | 序列号异常 |
| | | `0x14` | ERR_SEQ_MISMATCH | 序列号内容不匹配 |
| | | `0x15` | ERR_STALE_COMMAND | 命令已过期 |
| `0x20-0x2F` | Transport | `0x20` | ERR_CHUNK_CRC_FAILED | **单块 CRC 校验失败**（接收 Chunk 时的校验失败；FPGA 无 DDR 缓存，失败则不存入 Flash） |
| | | `0x21` | ERR_CHUNK_LENGTH_MISMATCH | Chunk 长度不匹配 |
| | | `0x22` | ERR_CHUNK_INDEX_GAP | Chunk 索引跳跃（警告） |
| | | `0x23` | ERR_CHUNK_DUPLICATE | 重复 Chunk 但内容不同 |
| | | `0x24` | ERR_FIRMWARE_CRC_FAILED | **固件整体 CRC 校验失败**（VERIFY 阶段对完整镜像的一致性校验失败；所有 Chunk 接收完后触发；Fatal） |
| | | `0x25` | ERR_TRANSFER_TIMEOUT | 传输超时（Fatal） |
| `0x30-0x3F` | System | `0x30` | ERR_RESOURCE_BUSY | 设备忙 |
| | | `0x31` | ERR_INSUFFICIENT_SPACE | 存储空间不足（Fatal） |
| | | `0x32` | ERR_FLASH_WRITE_FAILED | **Flash 写入失败**（单块通过 CRC 校验但 Flash 写入失败；须重试或中止；Fatal：若持续失败） |
| | | `0x33` | ERR_FLASH_ERASE_FAILED | Flash 擦除失败（Fatal） |
| | | `0x34` | ERR_ACTIVATION_FAILED | 激活失败（Fatal） |
| | | `0x35` | ERR_DEVICE_FAULT | 硬件故障（Critical） |
| `0x40-0x4F` | Security | `0x40` | ERR_INVALID_TOKEN | Token 验证失败 |
| | | `0x41` | ERR_SESSION_EXPIRED | 会话已过期 |
| | | `0x42` | ERR_SESSION_BUSY | 存在其他活动会话 |
| | | `0x43` | ERR_PERMISSION_DENIED | 权限不足 |
| | | `0x44` | ERR_REPLAY_DETECTED | 检测到重放攻击（终止会话） |
| | | `0x45` | ERR_AUTH_LOCKED | 认证锁定中（等待 10 秒） |
| `0x50-0x5F` | Provisioning | `0x50` | ERR_PROV_NOT_ALLOWED | 当前状态不允许开通（例如：非维护模式/策略禁止） |
| | | `0x51` | ERR_PROV_STAGED_CRC_MISMATCH | staged 配置 CRC 不匹配（ExpectedStagedCRC32C 校验失败） |
| | | `0x52` | ERR_PROV_APPLY_FAILED | 应用失败（网络栈/存储/NVM 写入失败等） |
| | | `0x53` | ERR_PROV_VERIFY_FAILED | 验证失败（新 IP 上未能完成闭环） |

**错误分级**：`0x01-0x0F` Warning（自动恢复），`0x10-0x7F` Recoverable（重试可恢复），`0x80-0xFE` Fatal（需 ABORT），`0xFF` Critical（需设备重启）。

**三层校验错误分布**：
- **单块校验层** (WRITE_CHUNK)：`0x20` 单块 CRC 失败 → 丢弃该 Chunk，SPS 可重试
- **存储层** (Flash 操作)：`0x32` Flash 写入失败 → 中止本次传输，检查硬件
- **完整性校验层** (VERIFY)：`0x24` 完整镜像 CRC 失败 → 整体失败，须重新开始升级

---

## 4 校验算法参数速查

### 4.1 HCS（已移除）

V1.0 起通用报文头不再包含 CRC16/HCS 字段。

### 4.2 CRC32C — CRC-32C (Castagnoli)

| 参数 | 值 |
|------|-----|
| 多项式 | `0x1EDC6F41`（反射：`0x82F63B78`） |
| 初始值 | `0xFFFFFFFF` |
| 输入反射 | true |
| 输出反射 | true |
| 最终异或 | `0xFFFFFFFF` |
| 线上字节序 | Little-Endian |

**各报文 CRC32C 覆盖范围**：

| 报文 | 覆盖起止（Payload 内 Offset） | 校验字段位置 |
|------|----------------------------|------------|
| RMA (0xFF) | “RMA Payload 起始 .. CRC32C 前一字节” | “RMA Payload 最后 4 字节” |

> 说明：若某些报文为了 DMA/缓存对齐在 CRC32C 后追加全 0 Padding，则 Padding **不参与 CRC32C**，但计入 `PayloadLen`。

### 4.3 FCS — CRC-32 (IEEE 802.3)

由 NIC 硬件处理，协议层无感知。多项式 `0x04C11DB7`，FCS 失败时 NIC 静默丢弃帧。
