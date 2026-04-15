#include "core/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/stats.h"
#include "net/network.h"
#include "net/ssl.h"
#include "protocol/http.h"

namespace titanbench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPollTimeoutMs = 20;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kMaxRetryPerSession = 3;
constexpr std::uint64_t kRetryBackoffMsBase = 50;

std::uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

void BindCurrentThreadToCpu(std::size_t logical_index) {
#if defined(__linux__)
  const unsigned int cpu_count = std::thread::hardware_concurrency();
  if (cpu_count == 0) {
    return;
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<int>(logical_index % cpu_count), &cpuset);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
  (void)logical_index;
#endif
}

std::vector<std::uint8_t> BuildPayload(const Config& config) {
  if (config.protocol == Protocol::kHttp) {
    http::Request req;
    req.host = config.host;
    req.path = config.path;
    req.keep_alive = true;
    const std::string packet = http::BuildGetRequest(req);
    return std::vector<std::uint8_t>(packet.begin(), packet.end());
  }
  if (config.protocol == Protocol::kTcp) {
    static const char kTcpPayload[] = "titanbench tcp payload";
    return std::vector<std::uint8_t>(kTcpPayload, kTcpPayload + sizeof(kTcpPayload) - 1);
  }
  if (config.protocol == Protocol::kUdp) {
    static const char kUdpPayload[] = "titanbench udp payload";
    return std::vector<std::uint8_t>(kUdpPayload, kUdpPayload + sizeof(kUdpPayload) - 1);
  }
  return {};
}

bool AcquireRequestQuota(const Config& config, std::atomic<std::uint64_t>* started) {
  if (!config.has_requests) {
    return true;
  }
  while (true) {
    std::uint64_t old_value = started->load(std::memory_order_relaxed);
    if (old_value >= static_cast<std::uint64_t>(config.requests)) {
      return false;
    }
    if (started->compare_exchange_weak(old_value, old_value + 1,
                                       std::memory_order_relaxed)) {
      return true;
    }
  }
}

void WaitRateToken(std::atomic<std::int64_t>* next_token_ns, std::int64_t interval_ns) {
  if (interval_ns <= 0) {
    return;
  }
  while (true) {
    const std::int64_t now_ns = static_cast<std::int64_t>(NowNs());
    std::int64_t old_ns = next_token_ns->load(std::memory_order_relaxed);
    const std::int64_t slot_ns = std::max(old_ns, now_ns);
    if (next_token_ns->compare_exchange_weak(old_ns, slot_ns + interval_ns,
                                             std::memory_order_relaxed)) {
      if (slot_ns > now_ns) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(slot_ns - now_ns));
      }
      return;
    }
  }
}

ErrorClass ToErrorClass(const std::error_code& ec) {
  if (ec.category() == GetNetErrorCategory() &&
      ec.value() == static_cast<int>(NetErrc::kTimeout)) {
    return ErrorClass::kTimeout;
  }
  if (ec.category() == GetNetErrorCategory() &&
      ec.value() == static_cast<int>(NetErrc::kResolveFailed)) {
    return ErrorClass::kConnectFailed;
  }
  return ErrorClass::kNetworkIo;
}

void RecordSample(StatsWriter* writer, const RequestSample& sample) {
  if (writer != nullptr) {
    writer->Record(sample);
  }
}

struct SharedState {
  explicit SharedState(const Config& cfg)
      : config(cfg),
        started_requests(0),
        completed_requests(0),
        succeeded_requests(0),
        failed_requests(0),
        retried_requests(0),
        bytes_sent(0),
        bytes_received(0),
        stop(false) {
    const int worker_count = std::max(1, std::min(config.threads, config.concurrency));
    const std::uint64_t smooth_rps =
        static_cast<std::uint64_t>(std::max(1, config.concurrency * 200));
    const std::int64_t interval = static_cast<std::int64_t>(1000000000LL / smooth_rps);
    rate_interval_ns = std::max<std::int64_t>(1, interval);
    next_token_ns.store(static_cast<std::int64_t>(NowNs()), std::memory_order_relaxed);
    workers = worker_count;
  }

  Config config;
  int workers = 1;
  std::atomic<std::uint64_t> started_requests;
  std::atomic<std::uint64_t> completed_requests;
  std::atomic<std::uint64_t> succeeded_requests;
  std::atomic<std::uint64_t> failed_requests;
  std::atomic<std::uint64_t> retried_requests;
  std::atomic<std::uint64_t> bytes_sent;
  std::atomic<std::uint64_t> bytes_received;
  std::atomic<std::int64_t> next_token_ns;
  std::int64_t rate_interval_ns = 0;
  std::atomic<bool> stop;
};

struct Session {
  ConnectionId conn_id = 0;
  http::ResponseParser parser;
  bool in_flight = false;
  int retries = 0;
  std::uint64_t request_begin_ns = 0;
  Clock::time_point retry_at = Clock::now();
};

void WorkerLoopEventDriven(std::size_t worker_index, int worker_concurrency, SharedState* shared,
                           StatsCollector* stats) {
  BindCurrentThreadToCpu(worker_index);
  NetWorker worker;
  if (worker.Start()) {
    shared->stop.store(true, std::memory_order_release);
    return;
  }
  std::unique_ptr<StatsWriter> writer = stats->CreateWriter();

  Endpoint endpoint;
  endpoint.host = shared->config.host;
  endpoint.port = static_cast<std::uint16_t>(shared->config.port);
  const std::vector<std::uint8_t> payload = BuildPayload(shared->config);
  std::unordered_map<ConnectionId, Session> sessions;
  sessions.reserve(static_cast<std::size_t>(worker_concurrency * 2));

  auto create_connection = [&](Session* session) -> bool {
    ConnectionId conn_id = 0;
    std::error_code ec;
    if (shared->config.protocol == Protocol::kUdp) {
      ec = worker.CreateUdpConnection(endpoint, &conn_id);
    } else {
      ec = worker.CreateTcpConnection(endpoint, kConnectTimeoutMs, &conn_id);
    }
    if (ec) {
      return false;
    }
    session->conn_id = conn_id;
    session->in_flight = false;
    session->parser.Reset();
    sessions[conn_id] = *session;
    return true;
  };

  for (int i = 0; i < worker_concurrency; ++i) {
    Session session;
    if (!create_connection(&session)) {
      shared->stop.store(true, std::memory_order_release);
      break;
    }
  }

  std::vector<RecvEvent> recv_events;
  recv_events.reserve(1024);

  while (!shared->stop.load(std::memory_order_acquire)) {
    const auto now = Clock::now();
    for (auto it = sessions.begin(); it != sessions.end();) {
      Session& s = it->second;
      if (s.conn_id == 0 || s.in_flight || now < s.retry_at) {
        ++it;
        continue;
      }
      if (!AcquireRequestQuota(shared->config, &shared->started_requests)) {
        ++it;
        continue;
      }
      WaitRateToken(&shared->next_token_ns, shared->rate_interval_ns);

      const std::uint64_t begin_ns = NowNs();
      const std::error_code send_ec = worker.Send(s.conn_id, payload);
      if (send_ec) {
        shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
        shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
        RecordSample(writer.get(), RequestSample {
                                       begin_ns,
                                       0,
                                       0,
                                       0,
                                       0,
                                       false,
                                       ToErrorClass(send_ec),
                                   });
      } else {
        shared->bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
        if (shared->config.protocol == Protocol::kTcp ||
            shared->config.protocol == Protocol::kUdp) {
          const std::uint64_t end_ns = NowNs();
          shared->succeeded_requests.fetch_add(1, std::memory_order_relaxed);
          shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
          RecordSample(writer.get(), RequestSample {
                                         end_ns,
                                         end_ns - begin_ns,
                                         static_cast<std::uint32_t>(payload.size()),
                                         0,
                                         0,
                                         true,
                                         ErrorClass::kNone,
                                     });
        } else {
          s.in_flight = true;
          s.request_begin_ns = begin_ns;
        }
      }
      ++it;
    }

    recv_events.clear();
    const std::error_code poll_ec = worker.PollOnce(kPollTimeoutMs, &recv_events);
    if (poll_ec) {
      shared->stop.store(true, std::memory_order_release);
      break;
    }

    for (const RecvEvent& ev : recv_events) {
      auto it = sessions.find(ev.conn_id);
      if (it == sessions.end()) {
        continue;
      }
      Session old = it->second;
      sessions.erase(it);

      if (ev.is_error) {
        if (old.in_flight) {
          shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
          shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
          const std::uint64_t now_ns = NowNs();
          RecordSample(writer.get(), RequestSample {
                                         now_ns,
                                         old.request_begin_ns == 0 ? 0 : now_ns - old.request_begin_ns,
                                         static_cast<std::uint32_t>(payload.size()),
                                         0,
                                         0,
                                         false,
                                         ToErrorClass(ev.error),
                                     });
        }
        if (old.retries < kMaxRetryPerSession &&
            !shared->stop.load(std::memory_order_acquire)) {
          old.retries++;
          shared->retried_requests.fetch_add(1, std::memory_order_relaxed);
          const std::uint64_t wait_ms =
              kRetryBackoffMsBase * static_cast<std::uint64_t>(1U << (old.retries - 1));
          old.retry_at = Clock::now() + std::chrono::milliseconds(wait_ms);
          if (!create_connection(&old)) {
            shared->stop.store(true, std::memory_order_release);
            break;
          }
        }
        continue;
      }

      shared->bytes_received.fetch_add(ev.data.size(), std::memory_order_relaxed);
      if (shared->config.protocol == Protocol::kHttp) {
        Session updated = old;
        if (!updated.parser.Feed(reinterpret_cast<const char*>(ev.data.data()),
                                 ev.data.size())) {
          updated.in_flight = false;
          shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
          shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
          const std::uint64_t now_ns = NowNs();
          RecordSample(writer.get(), RequestSample {
                                         now_ns,
                                         updated.request_begin_ns == 0 ? 0 : now_ns - updated.request_begin_ns,
                                         static_cast<std::uint32_t>(payload.size()),
                                         static_cast<std::uint32_t>(ev.data.size()),
                                         0,
                                         false,
                                         ErrorClass::kParseError,
                                     });
        } else if (updated.parser.Done()) {
          updated.in_flight = false;
          updated.retries = 0;
          const int http_status = updated.parser.response().status_code;
          const bool ok = http_status > 0 && http_status < 400;
          shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
          if (ok) {
            shared->succeeded_requests.fetch_add(1, std::memory_order_relaxed);
          } else {
            shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
          }
          const std::uint64_t now_ns = NowNs();
          RecordSample(writer.get(), RequestSample {
                                         now_ns,
                                         updated.request_begin_ns == 0 ? 0 : now_ns - updated.request_begin_ns,
                                         static_cast<std::uint32_t>(payload.size()),
                                         static_cast<std::uint32_t>(ev.data.size()),
                                         http_status,
                                         ok,
                                         ok ? ErrorClass::kNone
                                            : (http_status >= 500 ? ErrorClass::kHttp5xx
                                                                  : ErrorClass::kHttp4xx),
                                     });
          updated.parser.Reset();
        }
        sessions[updated.conn_id] = std::move(updated);
      } else {
        sessions[old.conn_id] = std::move(old);
      }
    }
  }

  for (const auto& item : sessions) {
    (void)worker.Close(item.first);
  }
  (void)worker.Stop();
}

void WorkerLoopHttps(std::size_t worker_index, int worker_concurrency, SharedState* shared,
                     StatsCollector* stats) {
  BindCurrentThreadToCpu(worker_index);
  http::Request req;
  req.host = shared->config.host;
  req.path = shared->config.path;
  req.keep_alive = true;

  auto slot_run = [shared, req, stats, worker_index](std::size_t slot_index) {
    BindCurrentThreadToCpu(worker_index + slot_index + 1);
    std::unique_ptr<StatsWriter> writer = stats->CreateWriter();
    tls::SslClient client;
    std::string err;
    if (!client.Init(&err)) {
      shared->stop.store(true, std::memory_order_release);
      return;
    }
    tls::SslOptions options;
    options.endpoint.host = shared->config.host;
    options.endpoint.port = static_cast<std::uint16_t>(shared->config.port);
    options.connect_timeout_ms = kConnectTimeoutMs;
    options.handshake_timeout_ms = kConnectTimeoutMs;
    options.verify_peer = false;
    options.keep_alive = true;

    int retry = 0;
    while (!shared->stop.load(std::memory_order_acquire)) {
      if (!client.connected() && !client.Connect(options, &err)) {
        if (retry < kMaxRetryPerSession) {
          retry++;
          shared->retried_requests.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(
              kRetryBackoffMsBase * static_cast<std::uint64_t>(1U << (retry - 1))));
          continue;
        }
        shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
        shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
        RecordSample(writer.get(), RequestSample {
                                       NowNs(), 0, 0, 0, 0, false, ErrorClass::kConnectFailed,
                                   });
        shared->stop.store(true, std::memory_order_release);
        return;
      }

      if (!AcquireRequestQuota(shared->config, &shared->started_requests)) {
        return;
      }
      WaitRateToken(&shared->next_token_ns, shared->rate_interval_ns);

      http::Response resp;
      std::string body;
      http::Result result = client.PerformHttpGet(req, &resp, &body);
      shared->bytes_sent.fetch_add(result.bytes_sent, std::memory_order_relaxed);
      shared->bytes_received.fetch_add(result.bytes_received, std::memory_order_relaxed);
      shared->completed_requests.fetch_add(1, std::memory_order_relaxed);
      const bool ok = result.status == http::ResultStatus::kOk && resp.status_code < 400;
      if (ok) {
        retry = 0;
        shared->succeeded_requests.fetch_add(1, std::memory_order_relaxed);
      } else {
        shared->failed_requests.fetch_add(1, std::memory_order_relaxed);
      }
      RecordSample(writer.get(), RequestSample {
                                     NowNs(),
                                     static_cast<std::uint64_t>(std::max<std::int64_t>(0, result.latency_ns)),
                                     static_cast<std::uint32_t>(result.bytes_sent),
                                     static_cast<std::uint32_t>(result.bytes_received),
                                     resp.status_code,
                                     ok,
                                     ok ? ErrorClass::kNone
                                        : (result.status == http::ResultStatus::kParseError
                                               ? ErrorClass::kParseError
                                               : (resp.status_code >= 500 ? ErrorClass::kHttp5xx
                                                                          : ErrorClass::kOther)),
                                 });
      if (!ok && retry < kMaxRetryPerSession) {
        retry++;
        shared->retried_requests.fetch_add(1, std::memory_order_relaxed);
        client.Close();
        std::this_thread::sleep_for(std::chrono::milliseconds(
            kRetryBackoffMsBase * static_cast<std::uint64_t>(1U << (retry - 1))));
      }
    }
  };

  std::vector<std::thread> slots;
  slots.reserve(static_cast<std::size_t>(worker_concurrency));
  for (int i = 0; i < worker_concurrency; ++i) {
    slots.emplace_back(slot_run, static_cast<std::size_t>(i));
  }
  for (std::thread& t : slots) {
    if (t.joinable()) {
      t.join();
    }
  }
}

EngineReport BuildProgressReport(const SharedState& shared, StatsCollector* stats,
                                 const Clock::time_point& begin) {
  EngineReport out;
  out.started_requests = shared.started_requests.load(std::memory_order_relaxed);
  out.completed_requests = shared.completed_requests.load(std::memory_order_relaxed);
  out.succeeded_requests = shared.succeeded_requests.load(std::memory_order_relaxed);
  out.failed_requests = shared.failed_requests.load(std::memory_order_relaxed);
  out.retried_requests = shared.retried_requests.load(std::memory_order_relaxed);
  out.bytes_sent = shared.bytes_sent.load(std::memory_order_relaxed);
  out.bytes_received = shared.bytes_received.load(std::memory_order_relaxed);
  out.elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
          .count();
  out.stats = stats->Snapshot(true);
  return out;
}

}  // namespace

BenchmarkEngine::BenchmarkEngine(Config config) : config_(std::move(config)) {}

bool BenchmarkEngine::Run(std::string* error_message,
                          const ProgressCallback& progress_callback) {
  if (error_message == nullptr) {
    return false;
  }
  *error_message = "";
  if (config_.concurrency <= 0 || config_.threads <= 0) {
    *error_message = "invalid engine config";
    return false;
  }

  StatsCollector stats;
  SharedState shared(config_);
  const Clock::time_point begin = Clock::now();
  const Clock::time_point end_time =
      begin + std::chrono::seconds(std::max(1, config_.duration_seconds));

  const int worker_count = shared.workers;
  std::vector<int> worker_concurrency(static_cast<std::size_t>(worker_count), 0);
  for (int i = 0; i < config_.concurrency; ++i) {
    worker_concurrency[static_cast<std::size_t>(i % worker_count)]++;
  }

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(worker_count));
  for (int i = 0; i < worker_count; ++i) {
    const int per_worker = worker_concurrency[static_cast<std::size_t>(i)];
    if (per_worker <= 0) {
      continue;
    }
    if (config_.protocol == Protocol::kHttps) {
      workers.emplace_back([i, &shared, per_worker, &stats]() {
        WorkerLoopHttps(static_cast<std::size_t>(i), per_worker, &shared, &stats);
      });
    } else {
      workers.emplace_back([i, per_worker, &shared, &stats]() {
        WorkerLoopEventDriven(static_cast<std::size_t>(i), per_worker, &shared, &stats);
      });
    }
  }

  Clock::time_point last_progress = begin;
  while (!shared.stop.load(std::memory_order_acquire)) {
    if (config_.has_duration) {
      if (Clock::now() >= end_time) {
        shared.stop.store(true, std::memory_order_release);
        break;
      }
    } else if (config_.has_requests) {
      const std::uint64_t completed =
          shared.completed_requests.load(std::memory_order_relaxed);
      if (completed >= static_cast<std::uint64_t>(config_.requests)) {
        shared.stop.store(true, std::memory_order_release);
        break;
      }
    }

    if (progress_callback &&
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - last_progress)
                .count() >= 1) {
      progress_callback(BuildProgressReport(shared, &stats, begin));
      last_progress = Clock::now();
    } else {
      stats.Drain();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  for (std::thread& t : workers) {
    if (t.joinable()) {
      t.join();
    }
  }

  report_ = BuildProgressReport(shared, &stats, begin);
  if (progress_callback) {
    progress_callback(report_);
  }

  if (report_.completed_requests == 0) {
    *error_message = "benchmark finished with zero completed requests";
    return false;
  }
  return true;
}

}  // namespace titanbench
