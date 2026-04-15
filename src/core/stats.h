#ifndef TITANBENCH_SRC_STATS_H_
#define TITANBENCH_SRC_STATS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace titanbench {

// 错误类型枚举，用于分类记录不同类型的错误
enum class ErrorClass : std::uint8_t {
  kNone = 0,              // 无错误
  kConnectFailed = 1,     // 连接失败
  kTimeout = 2,           // 超时
  kHttp4xx = 3,           // HTTP 4xx 错误
  kHttp5xx = 4,           // HTTP 5xx 错误
  kNetworkIo = 5,         // 网络 IO 错误
  kTlsHandshake = 6,      // TLS 握手失败
  kParseError = 7,        // 解析错误
  kOther = 8,             // 其他错误
  kCount = 9,             // 错误类型总数（用于数组大小）
};

// 单个请求的采样数据结构
struct RequestSample {
  std::uint64_t timestamp_ns = 0;       // 请求完成的时间戳（纳秒）
  std::uint64_t latency_ns = 0;         // 请求延迟（纳秒）
  std::uint32_t bytes_sent = 0;         // 发送的字节数
  std::uint32_t bytes_received = 0;     // 接收的字节数
  int http_status = 0;                   // HTTP 状态码
  bool success = false;                  // 请求是否成功
  ErrorClass error = ErrorClass::kNone; // 错误类型
};

// 统计收集器的配置
struct StatsConfig {
  std::size_t ring_capacity = 4096;  // 每个 writer 的无锁环容量（会向上取 2 的幂次）
  std::size_t qps_slot_count = 64;   // 秒级窗口的槽位数，建议为 2 的幂次
};

// 统计快照结构，包含某一时刻的完整统计信息
struct StatsSnapshot {
  std::uint64_t total_requests = 0;    // 总请求数
  std::uint64_t success_requests = 0;  // 成功请求数
  std::uint64_t failed_requests = 0;   // 失败请求数

  double qps = 0.0;                     // 平均 QPS（每秒查询率）
  std::uint64_t qps_current_second = 0; // 当前秒的 QPS

  std::uint64_t avg_latency_ns = 0;    // 平均延迟（纳秒）
  std::uint64_t min_latency_ns = 0;    // 最小延迟（纳秒）
  std::uint64_t max_latency_ns = 0;    // 最大延迟（纳秒）
  std::uint64_t p50_latency_ns = 0;    // P50 延迟（纳秒）
  std::uint64_t p95_latency_ns = 0;    // P95 延迟（纳秒）
  std::uint64_t p99_latency_ns = 0;    // P99 延迟（纳秒）

  double throughput_bytes_per_sec = 0.0; // 吞吐量（字节/秒）
  std::uint64_t bytes_sent = 0;         // 总发送字节数
  std::uint64_t bytes_received = 0;     // 总接收字节数

  // 各类错误的计数
  std::array<std::uint64_t, static_cast<std::size_t>(ErrorClass::kCount)>
      error_counts {};

  std::uint64_t dropped_samples = 0;   // 丢弃的采样数
  std::uint64_t elapsed_ns = 0;         // 经过的时间（纳秒）
};

// 统计写入器，用于从 worker 线程记录请求样本
class StatsWriter {
 public:
  StatsWriter();
  ~StatsWriter();

  StatsWriter(StatsWriter&& other) noexcept;
  StatsWriter& operator=(StatsWriter&& other) noexcept;

  StatsWriter(const StatsWriter&) = delete;
  StatsWriter& operator=(const StatsWriter&) = delete;

  // 记录一个请求样本
  void Record(const RequestSample& sample);

 private:
  struct Impl;
  explicit StatsWriter(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;

  friend class StatsCollector;
};

// 统计收集器，用于收集和汇总来自多个 writer 的统计数据
class StatsCollector {
 public:
  explicit StatsCollector(const StatsConfig& config = StatsConfig {});
  ~StatsCollector();

  StatsCollector(const StatsCollector&) = delete;
  StatsCollector& operator=(const StatsCollector&) = delete;

  // 创建一个新的统计写入器
  std::unique_ptr<StatsWriter> CreateWriter();

  // 主线程/统计线程调用：消费无锁环中的样本（不阻塞 worker）
  void Drain();

  // 获取实时快照；默认先 Drain 再计算
  StatsSnapshot Snapshot(bool drain_before_snapshot = true);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace titanbench

#endif  // TITANBENCH_SRC_STATS_H_
