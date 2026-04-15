#include "net/network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace titanbench {
namespace {

constexpr std::size_t kMaxEvents = 1024;
constexpr std::size_t kReadBufferSize = 64 * 1024;
constexpr std::size_t kCommandWakeValue = 1;
constexpr std::size_t kPreallocDrainCommands = 4096;

using Clock = std::chrono::steady_clock;

int SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }
  return 0;
}

void ApplySocketOptimizations(int fd, bool is_tcp) {
  const int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
  if (is_tcp) {
    const int no_delay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
  }
}

bool BuildSockAddr(const Endpoint& endpoint,
                   sockaddr_storage* storage,
                   socklen_t* addr_len,
                   int socket_type) {
  if (endpoint.host.empty() || endpoint.port == 0) {
    return false;
  }

  addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socket_type;
  hints.ai_protocol = 0;

  addrinfo* result = nullptr;
  const std::string port_text = std::to_string(endpoint.port);
  const int rc = getaddrinfo(endpoint.host.c_str(), port_text.c_str(), &hints,
                             &result);
  if (rc != 0 || result == nullptr) {
    return false;
  }

  std::memcpy(storage, result->ai_addr, result->ai_addrlen);
  *addr_len = static_cast<socklen_t>(result->ai_addrlen);
  freeaddrinfo(result);
  return true;
}

}  // namespace

const char* NetErrorCategory::name() const noexcept { return "titanbench.net"; }

std::string NetErrorCategory::message(int ev) const {
  switch (static_cast<NetErrc>(ev)) {
    case NetErrc::kOk:
      return "ok";
    case NetErrc::kInvalidArgument:
      return "invalid argument";
    case NetErrc::kNotStarted:
      return "network worker not started";
    case NetErrc::kResolveFailed:
      return "dns resolve failed";
    case NetErrc::kSocketCreateFailed:
      return "socket create failed";
    case NetErrc::kSetNonBlockingFailed:
      return "set non-blocking failed";
    case NetErrc::kEpollCreateFailed:
      return "epoll create failed";
    case NetErrc::kEpollCtlFailed:
      return "epoll ctl failed";
    case NetErrc::kEventFdCreateFailed:
      return "eventfd create failed";
    case NetErrc::kIoFailed:
      return "io failed";
    case NetErrc::kTimeout:
      return "connection timeout";
    case NetErrc::kConnectionNotFound:
      return "connection not found";
    case NetErrc::kQueueFull:
      return "command queue allocation failed";
    case NetErrc::kAlreadyStarted:
      return "already started";
  }
  return "unknown net error";
}

const std::error_category& GetNetErrorCategory() {
  static NetErrorCategory category;
  return category;
}

std::error_code MakeNetError(NetErrc code) {
  return {static_cast<int>(code), GetNetErrorCategory()};
}

std::error_code make_error_code(NetErrc code) { return MakeNetError(code); }

struct NetWorker::Impl {
  enum class ConnType { kTcp, kUdp };
  enum class ConnState { kConnecting, kConnected, kClosed };
  enum class CommandType { kCreateTcp, kCreateUdp, kSend, kClose };

  struct Command {
    CommandType type = CommandType::kClose;
    ConnectionId conn_id = 0;
    Endpoint endpoint;
    int connect_timeout_ms = 0;
    std::vector<std::uint8_t> payload;
    std::atomic<Command*> next {nullptr};
  };

  struct OutboundChunk {
    std::vector<std::uint8_t> data;
    std::size_t sent_offset = 0;
  };

  struct Connection {
    int fd = -1;
    ConnectionId id = 0;
    ConnType type = ConnType::kTcp;
    ConnState state = ConnState::kClosed;
    Clock::time_point connect_begin {};
    int connect_timeout_ms = 0;
    std::deque<OutboundChunk> pending_send;
    std::error_code last_error;
  };

  int epoll_fd = -1;
  int wake_fd = -1;
  std::atomic<bool> started {false};
  std::atomic<ConnectionId> next_conn_id {1};
  std::atomic<Command*> command_head {nullptr};  // lock-free MPSC stack
  std::atomic<Command*> command_free_head {nullptr};
  std::unordered_map<ConnectionId, Connection> connections;
  std::unordered_map<int, ConnectionId> fd_to_conn;
  std::array<epoll_event, kMaxEvents> events {};
  std::array<std::uint8_t, kReadBufferSize> read_buffer {};
  std::atomic<std::uint64_t> bytes_sent {0};
  std::atomic<std::uint64_t> bytes_received {0};
  std::atomic<std::uint64_t> send_ops {0};
  std::atomic<std::uint64_t> recv_ops {0};

  ~Impl() { ShutdownAll(); }

  Command* AcquireCommand() {
    Command* head = command_free_head.load(std::memory_order_acquire);
    while (head != nullptr) {
      Command* next = head->next.load(std::memory_order_relaxed);
      if (command_free_head.compare_exchange_weak(
              head, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        head->next.store(nullptr, std::memory_order_relaxed);
        return head;
      }
    }
    return new (std::nothrow) Command();
  }

  void ReleaseCommand(Command* cmd) {
    if (cmd == nullptr) {
      return;
    }
    cmd->type = CommandType::kClose;
    cmd->conn_id = 0;
    cmd->endpoint = Endpoint {};
    cmd->connect_timeout_ms = 0;
    cmd->payload.clear();  // keep capacity for reuse.
    Command* old_head = command_free_head.load(std::memory_order_relaxed);
    do {
      cmd->next.store(old_head, std::memory_order_relaxed);
    } while (!command_free_head.compare_exchange_weak(
        old_head, cmd, std::memory_order_release, std::memory_order_relaxed));
  }

  std::error_code Start() {
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
      return MakeNetError(NetErrc::kAlreadyStarted);
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
      started.store(false);
      return MakeNetError(NetErrc::kEpollCreateFailed);
    }

    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) {
      ShutdownAll();
      started.store(false);
      return MakeNetError(NetErrc::kEventFdCreateFailed);
    }

    epoll_event ev {};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = wake_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &ev) < 0) {
      ShutdownAll();
      started.store(false);
      return MakeNetError(NetErrc::kEpollCtlFailed);
    }
    return {};
  }

  std::error_code Stop() {
    if (!started.exchange(false)) {
      return {};
    }
    ShutdownAll();
    return {};
  }

  void ShutdownAll() {
    for (auto& it : connections) {
      if (it.second.fd >= 0) {
        close(it.second.fd);
      }
    }
    connections.clear();
    fd_to_conn.clear();

    if (wake_fd >= 0) {
      close(wake_fd);
      wake_fd = -1;
    }
    if (epoll_fd >= 0) {
      close(epoll_fd);
      epoll_fd = -1;
    }

    Command* node = command_head.exchange(nullptr);
    while (node != nullptr) {
      Command* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
    node = command_free_head.exchange(nullptr);
    while (node != nullptr) {
      Command* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
  }

  std::error_code EnqueueCommand(Command* command) {
    if (command == nullptr) {
      return MakeNetError(NetErrc::kQueueFull);
    }
    Command* old_head = command_head.load(std::memory_order_relaxed);
    do {
      command->next.store(old_head, std::memory_order_relaxed);
    } while (!command_head.compare_exchange_weak(
        old_head, command, std::memory_order_release, std::memory_order_relaxed));

    if (wake_fd >= 0) {
      const std::uint64_t wake_value = kCommandWakeValue;
      const ssize_t n = write(wake_fd, &wake_value, sizeof(wake_value));
      if (n < 0 && errno != EAGAIN) {
        return MakeNetError(NetErrc::kIoFailed);
      }
    }
    return {};
  }

  std::vector<Command*> DrainCommands() {
    Command* stack = command_head.exchange(nullptr, std::memory_order_acquire);

    // 反转为 FIFO 以保持提交顺序。
    Command* prev = nullptr;
    while (stack != nullptr) {
      Command* next = stack->next.load(std::memory_order_relaxed);
      stack->next.store(prev, std::memory_order_relaxed);
      prev = stack;
      stack = next;
    }

    std::vector<Command*> cmds;
    cmds.reserve(kPreallocDrainCommands);
    Command* cursor = prev;
    while (cursor != nullptr) {
      cmds.push_back(cursor);
      cursor = cursor->next.load(std::memory_order_relaxed);
    }
    return cmds;
  }

  void DrainWakeFd() {
    if (wake_fd < 0) {
      return;
    }
    std::uint64_t value = 0;
    while (read(wake_fd, &value, sizeof(value)) > 0) {
    }
  }

  std::error_code AddFdToEpoll(int fd, std::uint32_t events_mask) {
    epoll_event ev {};
    ev.events = events_mask | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      return MakeNetError(NetErrc::kEpollCtlFailed);
    }
    return {};
  }

  std::error_code ModFdInEpoll(int fd, std::uint32_t events_mask) {
    epoll_event ev {};
    ev.events = events_mask | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
      return MakeNetError(NetErrc::kEpollCtlFailed);
    }
    return {};
  }

  void RemoveConnection(ConnectionId id, std::error_code err,
                        std::vector<RecvEvent>* recv_events) {
    auto it = connections.find(id);
    if (it == connections.end()) {
      return;
    }
    Connection& conn = it->second;
    if (conn.fd >= 0) {
      epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn.fd, nullptr);
      close(conn.fd);
      fd_to_conn.erase(conn.fd);
    }

    if (recv_events != nullptr && err) {
      RecvEvent event;
      event.conn_id = conn.id;
      event.is_error = true;
      event.error = err;
      recv_events->push_back(std::move(event));
    }
    connections.erase(it);
  }

  void HandleCreateTcp(const Command& cmd, std::vector<RecvEvent>* recv_events) {
    Connection initial_conn;
    initial_conn.id = cmd.conn_id;
    initial_conn.type = ConnType::kTcp;
    initial_conn.state = ConnState::kConnecting;
    initial_conn.connect_timeout_ms = cmd.connect_timeout_ms;
    initial_conn.connect_begin = Clock::now();
    auto [conn_it, inserted] =
        connections.emplace(cmd.conn_id, std::move(initial_conn));
    if (!inserted) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }

    sockaddr_storage addr {};
    socklen_t addr_len = 0;
    if (!BuildSockAddr(cmd.endpoint, &addr, &addr_len, SOCK_STREAM)) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kResolveFailed),
                       recv_events);
      return;
    }

    const int family = reinterpret_cast<const sockaddr*>(&addr)->sa_family;
    const int fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kSocketCreateFailed),
                       recv_events);
      return;
    }
    if (SetNonBlocking(fd) < 0) {
      close(fd);
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kSetNonBlockingFailed),
                       recv_events);
      return;
    }
    ApplySocketOptimizations(fd, true);

    Connection& conn = conn_it->second;
    conn.fd = fd;
    conn.state = ConnState::kConnecting;
    conn.connect_begin = Clock::now();

    std::error_code ep_rc = AddFdToEpoll(fd, EPOLLIN | EPOLLOUT);
    if (ep_rc) {
      close(fd);
      RemoveConnection(cmd.conn_id, ep_rc, recv_events);
      return;
    }
    fd_to_conn[fd] = cmd.conn_id;

    const int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), addr_len);
    if (rc == 0) {
      conn.state = ConnState::kConnected;
      (void)ModFdInEpoll(fd, EPOLLIN);
      return;
    }

    if (errno == EINPROGRESS) {
      return;
    }

    RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kIoFailed), recv_events);
  }

  void HandleCreateUdp(const Command& cmd, std::vector<RecvEvent>* recv_events) {
    Connection initial_conn;
    initial_conn.id = cmd.conn_id;
    initial_conn.type = ConnType::kUdp;
    initial_conn.state = ConnState::kConnecting;
    auto [conn_it, inserted] =
        connections.emplace(cmd.conn_id, std::move(initial_conn));
    if (!inserted) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }

    sockaddr_storage addr {};
    socklen_t addr_len = 0;
    if (!BuildSockAddr(cmd.endpoint, &addr, &addr_len, SOCK_DGRAM)) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kResolveFailed),
                       recv_events);
      return;
    }

    const int family = reinterpret_cast<const sockaddr*>(&addr)->sa_family;
    const int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kSocketCreateFailed),
                       recv_events);
      return;
    }
    if (SetNonBlocking(fd) < 0) {
      close(fd);
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kSetNonBlockingFailed),
                       recv_events);
      return;
    }
    ApplySocketOptimizations(fd, false);

    Connection& conn = conn_it->second;
    conn.fd = fd;
    conn.state = ConnState::kConnected;

    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), addr_len) < 0) {
      close(fd);
      RemoveConnection(cmd.conn_id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }

    std::error_code ep_rc = AddFdToEpoll(fd, EPOLLIN);
    if (ep_rc) {
      close(fd);
      RemoveConnection(cmd.conn_id, ep_rc, recv_events);
      return;
    }
    fd_to_conn[fd] = cmd.conn_id;
  }

  void HandleSend(const Command& cmd, std::vector<RecvEvent>* recv_events) {
    auto it = connections.find(cmd.conn_id);
    if (it == connections.end()) {
      RecvEvent event;
      event.conn_id = cmd.conn_id;
      event.is_error = true;
      event.error = MakeNetError(NetErrc::kConnectionNotFound);
      recv_events->push_back(std::move(event));
      return;
    }

    Connection& conn = it->second;
    if (conn.state == ConnState::kClosed) {
      return;
    }
    conn.pending_send.push_back(OutboundChunk {cmd.payload, 0});

    if (conn.fd >= 0) {
      (void)ModFdInEpoll(conn.fd, EPOLLIN | EPOLLOUT);
    }
  }

  void HandleClose(const Command& cmd, std::vector<RecvEvent>* recv_events) {
    RemoveConnection(cmd.conn_id, {}, recv_events);
  }

  void ProcessCommands(std::vector<RecvEvent>* recv_events) {
    DrainWakeFd();
    std::vector<Command*> cmds = DrainCommands();
    for (Command* cmd : cmds) {
      switch (cmd->type) {
        case CommandType::kCreateTcp:
          HandleCreateTcp(*cmd, recv_events);
          break;
        case CommandType::kCreateUdp:
          HandleCreateUdp(*cmd, recv_events);
          break;
        case CommandType::kSend:
          HandleSend(*cmd, recv_events);
          break;
        case CommandType::kClose:
          HandleClose(*cmd, recv_events);
          break;
      }
      ReleaseCommand(cmd);
    }
  }

  void FlushWrite(Connection* conn, std::vector<RecvEvent>* recv_events) {
    while (!conn->pending_send.empty()) {
      OutboundChunk& chunk = conn->pending_send.front();
      const std::size_t remaining = chunk.data.size() - chunk.sent_offset;
      if (remaining == 0) {
        conn->pending_send.pop_front();
        continue;
      }

      const void* buf = chunk.data.data() + chunk.sent_offset;
      const ssize_t n = send(conn->fd, buf, remaining, MSG_NOSIGNAL);
      if (n > 0) {
        chunk.sent_offset += static_cast<std::size_t>(n);
        bytes_sent.fetch_add(static_cast<std::uint64_t>(n),
                             std::memory_order_relaxed);
        send_ops.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }

    if (connections.find(conn->id) == connections.end()) {
      return;
    }

    if (conn->pending_send.empty()) {
      (void)ModFdInEpoll(conn->fd, EPOLLIN);
    } else {
      (void)ModFdInEpoll(conn->fd, EPOLLIN | EPOLLOUT);
    }
  }

  void DrainRead(Connection* conn, std::vector<RecvEvent>* recv_events) {
    while (true) {
      const ssize_t n = recv(conn->fd, read_buffer.data(), read_buffer.size(), 0);
      if (n > 0) {
        RecvEvent event;
        event.conn_id = conn->id;
        event.data.assign(read_buffer.begin(), read_buffer.begin() + n);
        recv_events->push_back(std::move(event));
        bytes_received.fetch_add(static_cast<std::uint64_t>(n),
                                 std::memory_order_relaxed);
        recv_ops.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (n == 0) {
        RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }
  }

  void CompleteTcpConnect(Connection* conn, std::vector<RecvEvent>* recv_events) {
    if (conn->type != ConnType::kTcp || conn->state != ConnState::kConnecting) {
      return;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
      RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
      return;
    }

    if (so_error == 0) {
      conn->state = ConnState::kConnected;
      if (conn->pending_send.empty()) {
        (void)ModFdInEpoll(conn->fd, EPOLLIN);
      } else {
        (void)ModFdInEpoll(conn->fd, EPOLLIN | EPOLLOUT);
      }
      return;
    }

    RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
  }

  void CheckConnectTimeouts(std::vector<RecvEvent>* recv_events) {
    const Clock::time_point now = Clock::now();
    std::vector<ConnectionId> timed_out;
    timed_out.reserve(connections.size());

    for (const auto& item : connections) {
      const Connection& conn = item.second;
      if (conn.type == ConnType::kTcp && conn.state == ConnState::kConnecting &&
          conn.connect_timeout_ms > 0) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                  conn.connect_begin)
                .count();
        if (elapsed > conn.connect_timeout_ms) {
          timed_out.push_back(conn.id);
        }
      }
    }

    for (ConnectionId id : timed_out) {
      RemoveConnection(id, MakeNetError(NetErrc::kTimeout), recv_events);
    }
  }

  std::error_code PollOnce(int timeout_ms, std::vector<RecvEvent>* recv_events) {
    if (!started.load(std::memory_order_acquire)) {
      return MakeNetError(NetErrc::kNotStarted);
    }
    if (recv_events == nullptr) {
      return MakeNetError(NetErrc::kInvalidArgument);
    }

    ProcessCommands(recv_events);
    CheckConnectTimeouts(recv_events);

    const int ready = epoll_wait(epoll_fd, events.data(),
                                 static_cast<int>(events.size()), timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        return {};
      }
      return MakeNetError(NetErrc::kIoFailed);
    }

    for (int i = 0; i < ready; ++i) {
      const epoll_event& ev = events[static_cast<std::size_t>(i)];
      const int fd = ev.data.fd;
      if (fd == wake_fd) {
        ProcessCommands(recv_events);
        continue;
      }

      auto id_it = fd_to_conn.find(fd);
      if (id_it == fd_to_conn.end()) {
        continue;
      }
      auto conn_it = connections.find(id_it->second);
      if (conn_it == connections.end()) {
        continue;
      }
      Connection* conn = &conn_it->second;

      if ((ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        RemoveConnection(conn->id, MakeNetError(NetErrc::kIoFailed), recv_events);
        continue;
      }

      if ((ev.events & EPOLLOUT) != 0) {
        if (conn->type == ConnType::kTcp &&
            conn->state == ConnState::kConnecting) {
          CompleteTcpConnect(conn, recv_events);
          if (connections.find(id_it->second) == connections.end()) {
            continue;
          }
        }
        FlushWrite(conn, recv_events);
        if (connections.find(id_it->second) == connections.end()) {
          continue;
        }
      }

      if ((ev.events & EPOLLIN) != 0) {
        DrainRead(conn, recv_events);
      }
    }

    CheckConnectTimeouts(recv_events);
    return {};
  }

  std::error_code CreateTcpConnection(const Endpoint& endpoint,
                                      int connect_timeout_ms,
                                      ConnectionId* conn_id) {
    if (conn_id == nullptr || endpoint.host.empty() || endpoint.port == 0 ||
        connect_timeout_ms <= 0) {
      return MakeNetError(NetErrc::kInvalidArgument);
    }
    if (!started.load(std::memory_order_acquire)) {
      return MakeNetError(NetErrc::kNotStarted);
    }

    const ConnectionId id = next_conn_id.fetch_add(1, std::memory_order_relaxed);
    Command* cmd = AcquireCommand();
    if (cmd == nullptr) {
      return MakeNetError(NetErrc::kQueueFull);
    }
    cmd->type = CommandType::kCreateTcp;
    cmd->conn_id = id;
    cmd->endpoint = endpoint;
    cmd->connect_timeout_ms = connect_timeout_ms;

    *conn_id = id;
    return EnqueueCommand(cmd);
  }

  std::error_code CreateUdpConnection(const Endpoint& endpoint,
                                      ConnectionId* conn_id) {
    if (conn_id == nullptr || endpoint.host.empty() || endpoint.port == 0) {
      return MakeNetError(NetErrc::kInvalidArgument);
    }
    if (!started.load(std::memory_order_acquire)) {
      return MakeNetError(NetErrc::kNotStarted);
    }

    const ConnectionId id = next_conn_id.fetch_add(1, std::memory_order_relaxed);
    Command* cmd = AcquireCommand();
    if (cmd == nullptr) {
      return MakeNetError(NetErrc::kQueueFull);
    }
    cmd->type = CommandType::kCreateUdp;
    cmd->conn_id = id;
    cmd->endpoint = endpoint;

    *conn_id = id;
    return EnqueueCommand(cmd);
  }

  std::error_code Send(ConnectionId conn_id, std::vector<std::uint8_t> payload) {
    if (conn_id == 0 || payload.empty()) {
      return MakeNetError(NetErrc::kInvalidArgument);
    }
    if (!started.load(std::memory_order_acquire)) {
      return MakeNetError(NetErrc::kNotStarted);
    }

    Command* cmd = AcquireCommand();
    if (cmd == nullptr) {
      return MakeNetError(NetErrc::kQueueFull);
    }
    cmd->type = CommandType::kSend;
    cmd->conn_id = conn_id;
    cmd->payload = std::move(payload);  // move，避免额外用户态拷贝
    return EnqueueCommand(cmd);
  }

  std::error_code Close(ConnectionId conn_id) {
    if (conn_id == 0) {
      return MakeNetError(NetErrc::kInvalidArgument);
    }
    if (!started.load(std::memory_order_acquire)) {
      return MakeNetError(NetErrc::kNotStarted);
    }

    Command* cmd = AcquireCommand();
    if (cmd == nullptr) {
      return MakeNetError(NetErrc::kQueueFull);
    }
    cmd->type = CommandType::kClose;
    cmd->conn_id = conn_id;
    return EnqueueCommand(cmd);
  }

  NetStats GetStats() const {
    NetStats stats;
    stats.bytes_sent = bytes_sent.load(std::memory_order_relaxed);
    stats.bytes_received = bytes_received.load(std::memory_order_relaxed);
    stats.send_ops = send_ops.load(std::memory_order_relaxed);
    stats.recv_ops = recv_ops.load(std::memory_order_relaxed);
    return stats;
  }
};

NetWorker::NetWorker() : impl_(new Impl()) {}
NetWorker::~NetWorker() = default;

std::error_code NetWorker::Start() { return impl_->Start(); }
std::error_code NetWorker::Stop() { return impl_->Stop(); }

std::error_code NetWorker::PollOnce(int timeout_ms,
                                    std::vector<RecvEvent>* recv_events) {
  return impl_->PollOnce(timeout_ms, recv_events);
}

std::error_code NetWorker::CreateTcpConnection(const Endpoint& endpoint,
                                               int connect_timeout_ms,
                                               ConnectionId* conn_id) {
  return impl_->CreateTcpConnection(endpoint, connect_timeout_ms, conn_id);
}

std::error_code NetWorker::CreateUdpConnection(const Endpoint& endpoint,
                                               ConnectionId* conn_id) {
  return impl_->CreateUdpConnection(endpoint, conn_id);
}

std::error_code NetWorker::Send(ConnectionId conn_id,
                                std::vector<std::uint8_t> payload) {
  return impl_->Send(conn_id, std::move(payload));
}

std::error_code NetWorker::Close(ConnectionId conn_id) {
  return impl_->Close(conn_id);
}

NetStats NetWorker::GetStats() const { return impl_->GetStats(); }

}  // namespace titanbench
