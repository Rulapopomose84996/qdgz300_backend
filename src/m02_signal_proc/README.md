# M02 SignalProc

- 职责：消费 `RawBlock`，进行 GPU/CPU fallback 处理，输出 `PlotBatch`
- 主要输入：`RawBlock`
- 主要输出：`PlotBatch`
- 当前实现级别：最小实现，CPU fallback 为正式路径，GPU 真正算子链路待恢复
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'gpu_dispatcher_tests|gpu_pipeline_inflight_tests|m02_resource_pool_tests|m02_signal_proc_tests'`
