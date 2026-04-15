#ifndef TITANBENCH_SRC_NETWORK_H_
#define TITANBENCH_SRC_NETWORK_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace titanbench {

// 连接 ID 类型
using ConnectionId = std::uint64_t;

// 网络错误码枚举
enum class NetErrc {
  kOk = 0,                  // 成功
  kInvalidArgument = 1,     // 无效参数
  kNotStarted = 2,          // 未启动
  kResolveFailed = 3,       // DNS 解析失败
  kSocketCreateFailed = 4,  // Socket 创建失败
  kSetNonBlockingFailed = 5,// 设置非阻塞失败
  kEpollCreateFailed = 6,   // Epoll 创建失败
  kEpollCtlFailed = 7,      // Epoll 控制失败
  kEventFdCreateFailed = 8, // EventFd 创建失败
  kIoFailed = 9,            // IO 操作失败
  kTimeout = 10,            // 超时
  kConnectionNotFound = 11,  // 连接未找到
  kQueueFull = 12,          // 队列已满
  kAlreadyStarted = 13,     // 已启动
};

// 网络错误类别，用于 C++ 标准错误码系统
class NetErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int ev) const override;
};

const std::error_category& GetNetErrorCategory();
std::error_code MakeNetError(NetErrc code);
std::error_code make_error_code(NetErrc code);

// 端点结构体，表示网络端点（主机 + 端口）
struct Endpoint {
  std::string host;         // 主机地址
  std::uint16_t port = 0;   // 端口号
};

// 接收事件结构体
struct RecvEvent {
  ConnectionId conn_id = 0;          // 连接 ID
  std::vector<std::uint8_t> data;    // 接收到的数据
  bool is_error = false;              // 是否为错误事件
  std::error_code error;              // 错误码
};

// 网络统计结构体
struct NetStats {
  std::uint64_t bytes_sent = 0;      // 发送字节数
  std::uint64_t bytes_received = 0;  // 接收字节数
  std::uint64_t send_ops = 0;        // 发送操作次数
  std::uint64_t recv_ops = 0;        // 接收操作次数
};

// 网络工作者类，基于 epoll 的事件驱动网络层
class NetWorker {
 public:
  NetWorker();
  ~NetWorker();

  NetWorker(const NetWorker&) = delete;
  NetWorker& operator=(const NetWorker&) = delete;

  // 启动网络工作者
  std::error_code Start();
  // 停止网络工作者
  std::error_code Stop();

  // 单次 I/O 执行点（单消费者）
  std::error_code PollOnce(int timeout_ms, std::vector<RecvEvent>* recv_events);

  // 线程安全提交接口（无锁 MPSC 命令队列）
  // 创建 TCP 连接
  std::error_code CreateTcpConnection(const Endpoint& endpoint,
                                      int connect_timeout_ms,
                                      ConnectionId* conn_id);
  // 创建 UDP 连接
  std::error_code CreateUdpConnection(const Endpoint& endpoint,
                                      ConnectionId* conn_id);
  // 发送数据
  std::error_code Send(ConnectionId conn_id, std::vector<std::uint8_t> payload);
  // 关闭连接
  std::error_code Close(ConnectionId conn_id);

  // 获取网络统计
  NetStats GetStats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace titanbench

namespace std {
template <>
struct is_error_code_enum<titanbench::NetErrc> : true_type {};
}  // namespace std

#endif  // TITANBENCH_SRC_NETWORK_H_
