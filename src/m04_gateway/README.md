# M04 Gateway

- 职责：消费 `TrackFrame`，进行序列化、输出节流、会话管理与网关发送
- 主要输入：`TrackFrame`
- 主要输出：序列化后的 TrackData、命令应答
- 当前实现级别：最小实现，真实协议、真实会话与 protobuf 生产链路待恢复
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'm04_gateway_tests'`
