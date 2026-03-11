# M04 Gateway

- 职责：消费 `TrackFrame`，进行序列化、输出节流、会话管理与网关发送
- 主要输入：`TrackFrame`
- 主要输出：序列化后的 TrackData、命令应答
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'm04_gateway_tests'`
