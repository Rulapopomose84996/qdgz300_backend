# Control

- 职责：系统状态机、事件分发、编排控制、健康检查与命令桥接
- 主要输入：控制命令、模块健康状态、运行时事件
- 主要输出：系统状态转移、编排动作
- 测试入口：
  - `ctest --test-dir <build>/tests/unit -R 'control_tests'`
