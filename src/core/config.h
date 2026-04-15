#ifndef TITANBENCH_SRC_CONFIG_H_
#define TITANBENCH_SRC_CONFIG_H_

#include <cstdint>
#include <string>

namespace titanbench {

// 支持的协议类型枚举
enum class Protocol {
  kHttp,    // HTTP 协议
  kHttps,   // HTTPS 协议
  kTcp,     // 原始 TCP 协议
  kUdp,     // 原始 UDP 协议
};

// 压测配置结构体
struct Config {
  int concurrency = 0;                          // 并发连接数
  std::int64_t requests = 0;                    // 总请求数（与 duration_seconds 二选一）
  int duration_seconds = 0;                     // 压测持续秒数（与 requests 二选一）
  int threads = 1;                               // Worker 线程数
  Protocol protocol = Protocol::kHttp;          // 使用的协议类型
  std::string host;                              // 目标主机地址
  int port = 0;                                  // 目标端口号
  std::string path = "/";                        // HTTP/HTTPS 路径

  bool has_requests = false;                     // 是否设置了总请求数模式
  bool has_duration = false;                     // 是否设置了持续时间模式
  bool show_help = false;                        // 是否显示帮助信息
  bool show_version = false;                     // 是否显示版本信息
};

}  // namespace titanbench

#endif  // TITANBENCH_SRC_CONFIG_H_
