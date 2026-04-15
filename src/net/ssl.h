#ifndef TITANBENCH_SRC_SSL_H_
#define TITANBENCH_SRC_SSL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "net/network.h"
#include "protocol/http.h"

struct WOLFSSL_CTX;
struct WOLFSSL;

namespace titanbench {
namespace tls {

// SSL 错误码枚举
enum class SslErrc {
  kOk = 0,                  // 成功
  kInvalidArgument = 1,     // 无效参数
  kNotInitialized = 2,      // 未初始化
  kSocketCreateFailed = 3,  // Socket 创建失败
  kSetNonBlockingFailed = 4,// 设置非阻塞失败
  kResolveFailed = 5,       // DNS 解析失败
  kConnectFailed = 6,       // 连接失败
  kConnectTimeout = 7,      // 连接超时
  kHandshakeFailed = 8,     // 握手失败
  kHandshakeTimeout = 9,    // 握手超时
  kIoFailed = 10,           // IO 失败
  kPeerVerifyFailed = 11,   // 对等端验证失败
};

// SSL 客户端配置选项
struct SslOptions {
  Endpoint endpoint;         // 目标端点
  bool verify_peer = false;  // 是否验证对等端证书
  int connect_timeout_ms = 3000;      // 连接超时（毫秒）
  int handshake_timeout_ms = 3000;    // 握手超时（毫秒）
  bool keep_alive = true;   // 是否保持连接
};

// SSL 客户端类，基于 wolfSSL 实现
class SslClient {
 public:
  SslClient();
  ~SslClient();

  SslClient(const SslClient&) = delete;
  SslClient& operator=(const SslClient&) = delete;

  // 初始化 TLS 上下文（进程级 wolfSSL_Init 由内部自动处理）
  bool Init(std::string* error);

  // 建立非阻塞 TCP + TLS 连接并完成握手
  bool Connect(const SslOptions& options, std::string* error);

  // 非阻塞发送/接收。返回 >=0 为字节数，<0 失败
  int Send(const std::uint8_t* data, std::size_t len, std::string* error);
  int Recv(std::uint8_t* data, std::size_t len, std::string* error);

  // 与 HTTP 模块无缝对接的一次事务（GET）
  http::Result PerformHttpGet(const http::Request& request,
                              http::Response* response_out,
                              std::string* body_out);

  // 关闭连接
  void Close();
  // 检查是否已连接
  bool connected() const { return connected_; }
  // 获取文件描述符
  int fd() const { return fd_; }

 private:
  // 建立 TCP 连接
  bool ConnectTcp(const Endpoint& endpoint, int timeout_ms, std::string* error);
  // 执行 TLS 握手
  bool DoHandshake(int timeout_ms, std::string* error);
  // 等待文件描述符可读
  bool WaitFdReadable(int timeout_ms, std::string* error);
  // 等待文件描述符可写
  bool WaitFdWritable(int timeout_ms, std::string* error);

  WOLFSSL_CTX* ctx_ = nullptr;     // SSL 上下文
  WOLFSSL* ssl_ = nullptr;         // SSL 对象
  int fd_ = -1;                     // 文件描述符
  bool initialized_ = false;        // 是否已初始化
  bool connected_ = false;          // 是否已连接
  bool verify_peer_ = false;       // 是否验证对等端
  bool keep_alive_ = true;          // 是否保持连接
};

}  // namespace tls
}  // namespace titanbench

#endif  // TITANBENCH_SRC_SSL_H_
