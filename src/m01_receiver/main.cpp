/**
 * @file main.cpp
 * @brief qdgz300_backend 接收器应用程序入口点
 *
 * 本文件实现了雷达 FPGA 前端数据接收系统的主程序入口，负责：
 * - 命令行参数解析与验证
 * - 应用程序初始化配置
 * - 主运行循环启动
 * - 优雅关闭处理
 *
 * @author qdgz300 Team
 * @version See RECEIVER_APP_VERSION macro
 * @date See RECEIVER_APP_BUILD_TIME_UTC macro
 */

#include "qdgz300/m01_receiver/app_init.h"
#include "qdgz300/m01_receiver/app_run.h"

#include <cstdlib>      // std::exit
#include <iostream>     // std::cout, std::endl
#include <string>       // std::string

namespace
{
    /**
     * @struct CliOptions
     * @brief 命令行选项配置结构体
     *
     * 封装应用程序启动时的所有命令行参数，提供默认值初始化
     */
    struct CliOptions
    {
        std::string config_file{"config/receiver.yaml"};  ///< 配置文件路径（默认：config/receiver.yaml）
        bool dry_run{false};                               ///< 试运行模式标志（true=仅初始化不运行）
        bool show_version{false};                          ///< 版本信息显示标志（true=显示版本后退出）
    };

    /**
     * @brief 打印程序使用说明
     *
     * 向标准输出打印程序的命令行用法，包括所有可用参数及其格式
     * @param argv0 程序名称（来自 argv[0]）
     *
     * @note 输出格式示例:
     * @code
     * Usage: receiver_app [--config <path>] [--dry-run] [--version]
     * @endcode
     */
    void print_usage(const char *argv0)
    {
        std::cout << "Usage: " << argv0 << " [--config <path>] [--dry-run] [--version]\n";
    }

    /**
     * @brief 解析命令行参数
     *
     * 遍历 argc/argv 数组，识别并提取有效的命令行选项，填充到 CliOptions 结构体
     *
     * @param[in] argc 命令行参数个数
     * @param[in] argv 命令行参数数组
     * @param[out] opts 输出参数，用于存储解析后的选项配置
     *
     * @return 解析结果
     * @retval true 解析成功，所有参数均有效
     * @retval false 解析失败，存在无效参数或缺少必需参数值
     *
     * @details 支持的参数：
     * - --config <path>: 指定配置文件路径（覆盖默认值）
     * - --dry-run: 启用试运行模式（仅初始化，不进入主循环）
     * - --version: 显示版本信息并退出
     * - --help: 显示帮助信息并退出
     * - 无参位置参数：直接作为配置文件路径处理
     *
     * @note 参数解析规则：
     *       1. 以 '-' 开头的视为选项标志
     *       2. --config 后必须跟随路径参数
     *       3. 遇到未知选项立即返回 false
     *       4. 第一个非选项参数被视为配置文件路径
     */
    bool parse_cli_options(int argc, char *argv[], CliOptions &opts)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];

            // 处理 --config 选项：需要后续参数提供路径值
            if (arg == "--config")
            {
                if (i + 1 >= argc)
                {
                    // --config 后缺少路径参数，解析失败
                    return false;
                }
                opts.config_file = argv[++i];  // 跳过当前选项，读取下一个参数作为路径
                continue;
            }

            // 处理 --dry-run 标志：布尔开关，无需额外参数
            if (arg == "--dry-run")
            {
                opts.dry_run = true;
                continue;
            }

            // 处理 --version 标志：布尔开关，无需额外参数
            if (arg == "--version")
            {
                opts.show_version = true;
                continue;
            }

            // 处理 --help 标志：显示帮助后立即退出程序
            if (arg == "--help")
            {
                print_usage(argv[0]);
                std::exit(0);  // 正常退出，返回码 0
            }

            // 处理位置参数：非选项参数直接作为配置文件路径
            if (!arg.empty() && arg[0] != '-')
            {
                opts.config_file = arg;
                continue;
            }

            // 未知选项：以'-'开头但未识别的标志
            return false;
        }

        // 所有参数解析完成
        return true;
    }
} // namespace

//=============================================================================
// 应用程序版本信息宏定义
// 这些宏在编译时通过 CMake 注入，若未定义则使用默认占位值
//=============================================================================

#ifndef RECEIVER_APP_VERSION
    #define RECEIVER_APP_VERSION "dev"           ///< 应用程序版本号（默认："dev"）
#endif

#ifndef RECEIVER_APP_GIT_COMMIT
    #define RECEIVER_APP_GIT_COMMIT "unknown"    ///< Git 提交哈希（默认："unknown"）
#endif

#ifndef RECEIVER_APP_BUILD_TIME_UTC
    #define RECEIVER_APP_BUILD_TIME_UTC "unknown"  ///< UTC 构建时间戳（默认："unknown"）
#endif

/**
 * @brief 应用程序主入口函数
 *
 * 实现接收器应用程序的完整生命周期管理：
 * 1. 命令行参数解析与验证
 * 2. 版本信息显示（如请求）
 * 3. 应用上下文初始化
 * 4. 主运行循环执行
 * 5. 资源清理与关闭
 *
 * @param[in] argc 命令行参数个数
 * @param[in] argv 命令行参数数组（C 风格字符串）
 *
 * @return 程序退出码
 * @retval 0 成功执行（正常运行并退出，或显示版本/帮助后退出）
 * @retval 1 运行时错误（app_run 失败）
 * @retval 2 参数解析错误（无效的命令行参数）
 * @retval N 初始化错误（app_init 返回的非零错误码，见 error_codes.h）
 *
 * @details 执行流程：
 * @code
 * ┌─────────────┐
 * │   Start     │
 * └──────┬──────┘
 *        │
 *        ▼
 * ┌─────────────┐
 * │ Parse CLI   │───Error───> Return 2
 * └──────┬──────┘
 *        │ OK
 *        ▼
 * ┌─────────────┐
 * │ Show Ver?   │───Yes───> Print & Exit 0
 * └──────┬──────┘
 *        │ No
 *        ▼
 * ┌─────────────┐
 * │  app_init() │───Fail──> Return init_rc
 * └──────┬──────┘
 *        │ OK
 *        ▼
 * ┌─────────────┐
 * │  app_run()  │
 * └──────┬──────┘
 *        │
 *        ▼
 * ┌─────────────┐
 * │app_shutdown()│
 * └──────┬──────┘
 *        │
 *        ▼
 * ┌─────────────┐
 * │ Return Code │
 * └─────────────┘
 * @endcode
 *
 * @note 错误处理策略：
 *       - 参数错误：立即退出，返回码 2
 *       - 初始化失败：返回具体错误码（由 app_init 定义）
 *       - 运行失败：优先返回 run_rc，其次返回 shutdown_rc
 *       - 正常退出：返回 0
 *
 * @see receiver::app_init()  初始化函数
 * @see receiver::app_run()   主循环函数
 * @see receiver::app_shutdown() 关闭函数
 */
int main(int argc, char *argv[])
{
    // Step 1: 初始化命令行选项容器（使用默认值）
    CliOptions cli_options{};

    // Step 2: 解析命令行参数
    if (!parse_cli_options(argc, argv, cli_options))
    {
        // 解析失败：打印使用说明并退出
        print_usage(argv[0]);
        return 2;  // 约定返回码：2 = 用户输入错误
    }

    // Step 3: 检查是否需要显示版本信息
    if (cli_options.show_version)
    {
        // 输出三要素版本信息：版本号、Git 提交、构建时间
        std::cout << "receiver_app version=" << RECEIVER_APP_VERSION
                  << " git=" << RECEIVER_APP_GIT_COMMIT
                  << " build_time_utc=" << RECEIVER_APP_BUILD_TIME_UTC << std::endl;
    }
    
    // 始终显示版本信息（用于调试和追踪）
    std::cout << "receiver_app version=" << RECEIVER_APP_VERSION
              << " git=" << RECEIVER_APP_GIT_COMMIT
              << " build_time_utc=" << RECEIVER_APP_BUILD_TIME_UTC << std::endl;

    // Step 4: 构建应用程序选项对象
    // 将 CLI 参数转换为内部 AppOptions 结构，传递给初始化函数
    receiver::AppOptions app_options{};
    app_options.config_file = cli_options.config_file;  // 配置文件路径
    app_options.dry_run = cli_options.dry_run;          // 试运行模式标志

    // Step 5: 创建应用程序上下文对象
    // AppContext 持有所有运行时组件的句柄和状态
    receiver::AppContext app_ctx{};

    // Step 6: 执行应用程序初始化
    // 包括：加载配置、初始化日志、创建线程、分配资源等
    const int init_rc = receiver::app_init(app_options, app_ctx);
    if (init_rc != 0)
    {
        // 初始化失败：直接返回错误码
        // 错误码定义见 include/qdgz300/common/error_codes.h
        return init_rc;
    }

    // Step 7: 进入主运行循环
    // 阻塞直到收到停止信号或发生错误
    const int run_rc = receiver::app_run(app_ctx);

    // Step 8: 执行优雅关闭
    // 释放资源、停止线程、保存状态等
    const int shutdown_rc = receiver::app_shutdown(app_ctx);

    // Step 9: 返回最终退出码
    // 优先级：运行错误 > 关闭错误 > 成功
    return run_rc != 0 ? run_rc : shutdown_rc;
}
