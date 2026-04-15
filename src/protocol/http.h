#ifndef TITANBENCH_SRC_HTTP_H_
#define TITANBENCH_SRC_HTTP_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace titanbench {
namespace http {

// HTTP 头部结构
struct Header {
  std::string key;    // 头部键
  std::string value;  // 头部值
};

// HTTP 请求结构
struct Request {
  std::string host;              // 主机名
  std::string path = "/";        // 请求路径
  std::vector<Header> headers;   // 请求头
  bool keep_alive = true;        // 是否保持连接
};

// HTTP 响应结构
struct Response {
  int status_code = 0;                  // 状态码
  std::size_t content_length = 0;       // 内容长度
  bool has_content_length = false;      // 是否有 Content-Length 头
  bool header_done = false;             // 头部是否解析完成
  bool message_complete = false;        // 消息是否完整
  bool keep_alive = true;               // 是否保持连接
};

// 结果状态枚举
enum class ResultStatus {
  kOk = 0,         // 成功
  kParseError = 1, // 解析错误
  kTimeout = 2,    // 超时
  kNetworkError = 3,// 网络错误
  kIncomplete = 4, // 未完成
};

// HTTP 事务结果结构
struct Result {
  ResultStatus status = ResultStatus::kIncomplete; // 结果状态
  std::int64_t latency_ns = 0;                      // 延迟（纳秒）
  std::size_t bytes_sent = 0;                       // 发送字节数
  std::size_t bytes_received = 0;                   // 接收字节数
  std::string error;                                // 错误信息
};

// 构造 GET 请求报文（HTTP/1.1）
// 约定：
// 1) 自动补齐 Host。
// 2) 若用户未显式传 Connection，则由 keep_alive 决定。
std::string BuildGetRequest(const Request& request);

// HTTP 响应解析器，支持增量解析
class ResponseParser {
 public:
  ResponseParser();

  // 重置解析器状态
  void Reset();

  // 增量喂入响应字节流
  // 返回 true 表示当前输入被成功接受（不代表一定解析完成）
  bool Feed(const char* data, std::size_t len);

  // 在连接关闭时调用（例如短连接且无 Content-Length）
  void MarkEof();

  // 获取解析的响应
  const Response& response() const { return response_; }
  // 获取响应体
  const std::string& body() const { return body_; }
  // 检查是否解析完成
  bool Done() const { return response_.message_complete; }
  // 获取错误信息
  const std::string& error_message() const { return error_message_; }

 private:
  // 解析器状态枚举
  enum class State {
    kStatusLine = 0, // 解析状态行
    kHeaders = 1,    // 解析头部
    kBody = 2,       // 解析响应体
    kDone = 3,       // 完成
    kError = 4,      // 错误
  };

  // 解析状态行
  bool ParseStatusLine();
  // 解析头部
  bool ParseHeaders();
  // 解析响应体
  bool ParseBody();
  // 设置错误
  void SetError(std::string msg);

  State state_ = State::kStatusLine; // 当前解析状态
  Response response_ {};              // 解析的响应
  std::string error_message_;         // 错误信息
  std::string pending_;               // 待解析的缓冲区
  std::string body_;                  // 响应体
};

// 事务计时器，用于测量 HTTP 事务的耗时
class TransactionTimer {
 public:
  TransactionTimer();
  // 获取经过的纳秒数
  std::int64_t ElapsedNs() const;

 private:
  std::chrono::steady_clock::time_point begin_; // 开始时间点
};

}  // namespace http
}  // namespace titanbench

#endif  // TITANBENCH_SRC_HTTP_H_
