# M03 DataProc

- 职责：消费 `PlotBatch`，执行关联、跟踪、融合，构造 `TrackFrame`
- 主要输入：`PlotBatch`
- 主要输出：`TrackFrame`
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'm03_data_proc_tests'`
