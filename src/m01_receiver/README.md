# M01 Receiver

- 职责：接收 UDP 数据、解析协议、重组分片、重排序并投递到下游
- 主要输入：V3.1 数据包、心跳包
- 主要输出：`OrderedPacket`、`RawBlock`
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'config_manager_tests|packet_pool_tests|reassembler_tests|reorderer_tests|udp_receiver_tests'`
  - `ctest --test-dir <build>/tests/integration -R 'integration_rawblock_delivery_tests|integration_tests_fpga'`
