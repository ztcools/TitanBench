#ifndef TITANBENCH_SRC_ENGINE_H_
#define TITANBENCH_SRC_ENGINE_H_

#include <cstdint>
#include <functional>
#include <string>

#include "core/config.h"
#include "core/stats.h"

namespace titanbench {

// 引擎报告结构体，包含压测过程中的实时统计信息
struct EngineReport {
  std::uint64_t started_requests = 0;   // 已启动的请求数
  std::uint64_t completed_requests = 0; // 已完成的请求数
  std::uint64_t succeeded_requests = 0; // 成功的请求数
  std::uint64_t failed_requests = 0;    // 失败的请求数
  std::uint64_t retried_requests = 0;   // 重试的请求数
  std::uint64_t bytes_sent = 0;          // 发送的总字节数
  std::uint64_t bytes_received = 0;      // 接收的总字节数
  std::int64_t elapsed_ms = 0;           // 已用时间（毫秒）
  StatsSnapshot stats;                   // 统计快照
};

// 压测引擎类，负责执行实际的压测任务
class BenchmarkEngine {
 public:
  // 进度回调函数类型
  using ProgressCallback = std::function<void(const EngineReport&)>;

  explicit BenchmarkEngine(Config config);

  // 运行压测任务
  // error_message: 输出错误信息
  // progress_callback: 进度回调函数，可选
  bool Run(std::string* error_message,
           const ProgressCallback& progress_callback = ProgressCallback());

  // 获取最终报告
  const EngineReport& report() const { return report_; }

 private:
  Config config_;       // 压测配置
  EngineReport report_ {}; // 引擎报告
};

}  // namespace titanbench

#endif  // TITANBENCH_SRC_ENGINE_H_
