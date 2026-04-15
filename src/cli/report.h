#ifndef TITANBENCH_SRC_REPORT_H_
#define TITANBENCH_SRC_REPORT_H_

#include <ostream>
#include <string>

#include "core/config.h"
#include "core/engine.h"

namespace titanbench {

// CLI 报告打印机类，用于输出压测进度和最终报告
class CliReportPrinter {
 public:
  // 构造函数
  // config: 压测配置
  // out: 输出流，默认为 std::cout
  explicit CliReportPrinter(const Config& config, std::ostream* out = nullptr);

  // 打印进度信息（实时更新）
  void PrintProgress(const EngineReport& report);

  // 打印最终报告
  void PrintFinal(const EngineReport& report);

 private:
  // 构建进度信息行
  std::string BuildProgressLine(const EngineReport& report) const;
  // 构建最终报告
  std::string BuildFinalReport(const EngineReport& report) const;

  Config config_;               // 压测配置
  std::ostream* out_ = nullptr; // 输出流
  bool printed_progress_ = false; // 是否已打印过进度
};

}  // namespace titanbench

#endif  // TITANBENCH_SRC_REPORT_H_
