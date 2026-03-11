/**
 * @file app_run.h
 * @brief M01 接收器主运行循环与优雅关闭——声明 app_run() 和 app_shutdown()
 *
 * 本头文件声明的两个函数组成了应用程序生命周期的后半段：
 *
 *   main() → app_init() → **app_run()** → **app_shutdown()** → exit
 *
 * - app_run()     : 阻塞在主线程事件循环中，负责信号处理、指标采集、
 *                   超时检查和配置热加载，直到收到 SIGINT/SIGTERM 退出。
 * - app_shutdown() : 按安全顺序停止各子系统并清空管线残留数据（drain）。
 *
 * @note 实现位于 src/m01_receiver/app_run.cpp。
 * @see app_init.h  应用初始化（构建 AppContext）
 */

#ifndef RECEIVER_APP_RUN_H
#define RECEIVER_APP_RUN_H

#include "qdgz300/m01_receiver/app_init.h" // AppContext 定义

namespace receiver
{
    /**
     * @brief 进入主事件循环，阻塞直到收到退出信号
     *
     * 循环体每 100ms 执行一次，主要职责：
     * 1. 检查 SIGINT / SIGTERM / SIGHUP 信号并更新指标
     * 2. 若收到 SIGHUP → 触发 ConfigManager 热加载
     * 3. 调用 Reassembler / Reorderer 的超时检查（释放过期上下文/零填充）
     * 4. 读取 Reassembler / Reorderer 统计量，计算增量并上报 Prometheus 指标
     * 5. 每 5 秒采集系统级指标（CPU、内存等）
     *
     * @param[in,out] ctx 已初始化的应用上下文（由 app_init 填充）
     *
     * @return 运行结果
     * @retval 0 正常退出（收到信号或 dry-run 模式完成）
     * @retval 1 启动失败（指标端点或 UDP 接收器无法启动）
     *
     * @pre  ctx 已由 app_init() 成功初始化
     * @post 循环退出后应调用 app_shutdown() 完成清理
     */
    int app_run(AppContext &ctx);

    /**
     * @brief 优雅关闭：按依赖逆序停止所有子系统并清空管线
     *
     * 关闭顺序（3 秒总超时，超时则 std::_Exit 强制退出）：
     * 1. UdpReceiver::stop()          — 停止网络接收
     * 2. PcapWriter::stop()           — 刷写并关闭 pcap 文件
     * 3. Reassembler::flush_all()     — 强制完成所有未完成的重组上下文
     * 4. Reorderer::flush()           — 释放重排窗口中的所有包
     * 5. DeliveryInterface::flush()   — 确保投递缓冲区清空
     * 6. MetricsCollector::stop()     — 关闭 Prometheus HTTP 端点
     * 7. 打印最终统计日志
     *
     * @param[in,out] ctx 待关闭的应用上下文
     *
     * @return 关闭结果
     * @retval 0 正常关闭
     *
     * @note 若 ctx.started == false（主循环从未启动），直接返回 0 不做任何操作。
     */
    int app_shutdown(AppContext &ctx);
} // namespace receiver

#endif // RECEIVER_APP_RUN_H
