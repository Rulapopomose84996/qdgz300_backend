# QDGZ300 未实现能力清单

> 目的：把当前仍未实现的能力集中记录在文档里，而不是继续放进构建主链或散落在源码 TODO 中。

## M02

- GPU 真正算子链路尚未恢复：
  - H2D/D2H 异步传输
  - kernel launch
  - CUDA stream / event 生命周期
- 当前状态：CPU fallback 为正式可测路径，GPU 路径属于后续恢复项

## M04

- 当前仅为最小可测网关实现
- 尚未接入：
  - 真实 protobuf TrackFrame 编解码
  - 真实 TCP 会话管理
  - 真实 UDP 发送与组播参数
  - HMI ping/pong 生产行为

## Control

- 当前仅为最小可测编排实现
- 尚未接入：
  - 真实模块健康来源
  - 真实 boot checks
  - 真实命令协议对象
  - 真实状态快照和指标联动

## TowerGuard

- 当前仍未恢复，不在构建主链

## 原则

- 未实现能力只写在本文件和模块 README 中
- 不通过空壳 target、假可执行文件或失效源文件占位
