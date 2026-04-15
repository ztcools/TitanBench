#include "net/ssl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#if __has_include(<wolfssl/options.h>)
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#elif __has_include("wolfssl/options.h")
#include "wolfssl/options.h"
#include "wolfssl/ssl.h"
#elif __has_include("../../vcpkg_installed/x64-linux/include/wolfssl/options.h")
#include "../../vcpkg_installed/x64-linux/include/wolfssl/options.h"
#include "../../vcpkg_installed/x64-linux/include/wolfssl/ssl.h"
#else
#error "wolfSSL headers not found. Install via vcpkg and configure include paths."
#endif

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace titanbench {
namespace tls {
namespace {

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool ResolveEndpoint(const Endpoint& endpoint,
                     int socktype,
                     sockaddr_storage* out_addr,
                     socklen_t* out_len) {
  if (endpoint.host.empty() || endpoint.port == 0) {
    return false;
  }

  addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;

  addrinfo* result = nullptr;
  const std::string port_text = std::to_string(endpoint.port);
  const int rc = getaddrinfo(endpoint.host.c_str(), port_text.c_str(), &hints,
                             &result);
  if (rc != 0 || result == nullptr) {
    return false;
  }

  std::memcpy(out_addr, result->ai_addr, result->ai_addrlen);
  *out_len = static_cast<socklen_t>(result->ai_addrlen);
  freeaddrinfo(result);
  return true;
}

bool PollFd(int fd, short events, int timeout_ms) {
  pollfd pfd {};
  pfd.fd = fd;
  pfd.events = events;
  while (true) {
    const int rc = poll(&pfd, 1, timeout_ms);
    if (rc > 0) {
      return true;
    }
    if (rc == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
}

void SetErrorMessage(const char* prefix, int wolfssl_err, std::string* error) {
  if (error == nullptr) {
    return;
  }
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s (wolfssl err=%d)", prefix,
                wolfssl_err);
  *error = buffer;
}

}  // namespace

SslClient::SslClient() = default;

SslClient::~SslClient() { Close(); }

bool SslClient::Init(std::string* error) {
  if (initialized_) {
    return true;
  }

  if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
    if (error != nullptr) {
      *error = "wolfSSL_Init failed";
    }
    return false;
  }

  ctx_ = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
  if (ctx_ == nullptr) {
    if (error != nullptr) {
      *error = "wolfSSL_CTX_new failed";
    }
    return false;
  }

  initialized_ = true;
  return true;
}

bool SslClient::Connect(const SslOptions& options, std::string* error) {
  Close();
  if (!Init(error)) {
    return false;
  }

  verify_peer_ = options.verify_peer;
  keep_alive_ = options.keep_alive;

  if (verify_peer_) {
    wolfSSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
  } else {
    wolfSSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  }

  if (!ConnectTcp(options.endpoint, options.connect_timeout_ms, error)) {
    return false;
  }

  ssl_ = wolfSSL_new(ctx_);
  if (ssl_ == nullptr) {
    if (error != nullptr) {
      *error = "wolfSSL_new failed";
    }
    Close();
    return false;
  }

  if (wolfSSL_set_fd(ssl_, fd_) != WOLFSSL_SUCCESS) {
    if (error != nullptr) {
      *error = "wolfSSL_set_fd failed";
    }
    Close();
    return false;
  }

  if (!DoHandshake(options.handshake_timeout_ms, error)) {
    Close();
    return false;
  }

  connected_ = true;
  return true;
}

int SslClient::Send(const std::uint8_t* data, std::size_t len, std::string* error) {
  if (!connected_ || ssl_ == nullptr || data == nullptr || len == 0) {
    if (error != nullptr) {
      *error = "invalid send state";
    }
    return -1;
  }

  std::size_t offset = 0;
  while (offset < len) {
    const int rc = wolfSSL_write(ssl_, data + offset,
                                 static_cast<int>(len - offset));
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }

    const int err = wolfSSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ) {
      if (!WaitFdReadable(1000, error)) {
        return -1;
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      if (!WaitFdWritable(1000, error)) {
        return -1;
      }
      continue;
    }

    SetErrorMessage("wolfSSL_write failed", err, error);
    return -1;
  }
  return static_cast<int>(offset);
}

int SslClient::Recv(std::uint8_t* data, std::size_t len, std::string* error) {
  if (!connected_ || ssl_ == nullptr || data == nullptr || len == 0) {
    if (error != nullptr) {
      *error = "invalid recv state";
    }
    return -1;
  }

  while (true) {
    const int rc = wolfSSL_read(ssl_, data, static_cast<int>(len));
    if (rc > 0) {
      return rc;
    }
    if (rc == 0) {
      return 0;
    }

    const int err = wolfSSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ) {
      if (!WaitFdReadable(1000, error)) {
        return -1;
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      if (!WaitFdWritable(1000, error)) {
        return -1;
      }
      continue;
    }

    SetErrorMessage("wolfSSL_read failed", err, error);
    return -1;
  }
}

http::Result SslClient::PerformHttpGet(const http::Request& request,
                                       http::Response* response_out,
                                       std::string* body_out) {
  http::Result result;
  http::TransactionTimer timer;

  if (!connected_ || ssl_ == nullptr || response_out == nullptr ||
      body_out == nullptr) {
    result.status = http::ResultStatus::kNetworkError;
    result.error = "ssl client not connected";
    return result;
  }

  std::string req = http::BuildGetRequest(request);
  result.bytes_sent = static_cast<std::size_t>(req.size());
  std::string io_error;
  if (Send(reinterpret_cast<const std::uint8_t*>(req.data()), req.size(),
           &io_error) < 0) {
    result.status = http::ResultStatus::kNetworkError;
    result.error = io_error;
    result.latency_ns = timer.ElapsedNs();
    return result;
  }

  http::ResponseParser parser;
  std::vector<std::uint8_t> buffer(64 * 1024);
  while (!parser.Done()) {
    const int n = Recv(buffer.data(), buffer.size(), &io_error);
    if (n < 0) {
      result.status = http::ResultStatus::kNetworkError;
      result.error = io_error;
      result.latency_ns = timer.ElapsedNs();
      return result;
    }
    if (n == 0) {
      parser.MarkEof();
      break;
    }

    result.bytes_received += static_cast<std::size_t>(n);
    if (!parser.Feed(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::size_t>(n))) {
      result.status = http::ResultStatus::kParseError;
      result.error = parser.error_message();
      result.latency_ns = timer.ElapsedNs();
      return result;
    }
  }

  if (!parser.Done()) {
    result.status = http::ResultStatus::kIncomplete;
    result.error = "http response incomplete";
    result.latency_ns = timer.ElapsedNs();
    return result;
  }

  *response_out = parser.response();
  *body_out = parser.body();
  result.status = http::ResultStatus::kOk;
  result.latency_ns = timer.ElapsedNs();

  if (!request.keep_alive || !parser.response().keep_alive || !keep_alive_) {
    Close();
  }
  return result;
}

void SslClient::Close() {
  connected_ = false;

  if (ssl_ != nullptr) {
    wolfSSL_shutdown(ssl_);
    wolfSSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool SslClient::ConnectTcp(const Endpoint& endpoint,
                           int timeout_ms,
                           std::string* error) {
  if (timeout_ms <= 0) {
    if (error != nullptr) {
      *error = "invalid connect timeout";
    }
    return false;
  }

  sockaddr_storage addr {};
  socklen_t addr_len = 0;
  if (!ResolveEndpoint(endpoint, SOCK_STREAM, &addr, &addr_len)) {
    if (error != nullptr) {
      *error = "resolve endpoint failed";
    }
    return false;
  }

  fd_ = socket(reinterpret_cast<const sockaddr*>(&addr)->sa_family, SOCK_STREAM,
               IPPROTO_TCP);
  if (fd_ < 0) {
    if (error != nullptr) {
      *error = "socket create failed";
    }
    return false;
  }
  if (!SetNonBlocking(fd_)) {
    if (error != nullptr) {
      *error = "set non-blocking failed";
    }
    return false;
  }

  const int rc = connect(fd_, reinterpret_cast<const sockaddr*>(&addr), addr_len);
  if (rc == 0) {
    return true;
  }
  if (errno != EINPROGRESS) {
    if (error != nullptr) {
      *error = "connect failed";
    }
    return false;
  }

  if (!WaitFdWritable(timeout_ms, error)) {
    if (error != nullptr && error->empty()) {
      *error = "connect timeout";
    }
    return false;
  }

  int so_error = 0;
  socklen_t so_len = static_cast<socklen_t>(sizeof(so_error));
  if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 ||
      so_error != 0) {
    if (error != nullptr) {
      *error = "connect completion failed";
    }
    return false;
  }

  return true;
}

bool SslClient::DoHandshake(int timeout_ms, std::string* error) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const int rc = wolfSSL_connect(ssl_);
    if (rc == WOLFSSL_SUCCESS) {
      return true;
    }

    const int err = wolfSSL_get_error(ssl_, rc);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (elapsed_ms >= timeout_ms) {
      if (error != nullptr) {
        *error = "tls handshake timeout";
      }
      return false;
    }
    const int left_ms = timeout_ms - static_cast<int>(elapsed_ms);

    if (err == SSL_ERROR_WANT_READ) {
      if (!WaitFdReadable(left_ms, error)) {
        return false;
      }
      continue;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
      if (!WaitFdWritable(left_ms, error)) {
        return false;
      }
      continue;
    }

    SetErrorMessage("tls handshake failed", err, error);
    return false;
  }
}

bool SslClient::WaitFdReadable(int timeout_ms, std::string* error) {
  if (fd_ < 0) {
    if (error != nullptr) {
      *error = "invalid fd";
    }
    return false;
  }
  if (!PollFd(fd_, POLLIN, timeout_ms)) {
    if (error != nullptr) {
      *error = "wait readable timeout/failure";
    }
    return false;
  }
  return true;
}

bool SslClient::WaitFdWritable(int timeout_ms, std::string* error) {
  if (fd_ < 0) {
    if (error != nullptr) {
      *error = "invalid fd";
    }
    return false;
  }
  if (!PollFd(fd_, POLLOUT, timeout_ms)) {
    if (error != nullptr) {
      *error = "wait writable timeout/failure";
    }
    return false;
  }
  return true;
}

}  // namespace tls
}  // namespace titanbench
