#include "core/stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace titanbench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kLatencyBucketCount = 11891;
constexpr std::uint64_t kNsPerUs = 1000;

std::uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

std::size_t NormalizePowerOfTwo(std::size_t n, std::size_t min_value) {
  std::size_t value = std::max(n, min_value);
  std::size_t out = 1;
  while (out < value) {
    out <<= 1U;
  }
  return out;
}

std::size_t LatencyToBucket(std::uint64_t latency_ns) {
  const std::uint64_t latency_us = latency_ns / kNsPerUs;
  if (latency_us <= 1000) {
    return static_cast<std::size_t>(latency_us);
  }
  if (latency_us <= 100000) {
    return static_cast<std::size_t>(1000 + (latency_us - 1000) / 100);
  }
  if (latency_us <= 10000000) {
    return static_cast<std::size_t>(1990 + (latency_us - 100000) / 1000);
  }
  return kLatencyBucketCount - 1;
}

std::uint64_t BucketToLatencyUpperNs(std::size_t bucket) {
  if (bucket <= 1000) {
    return static_cast<std::uint64_t>(bucket) * kNsPerUs;
  }
  if (bucket <= 1990) {
    return (1000ULL + static_cast<std::uint64_t>(bucket - 1000) * 100ULL) * kNsPerUs;
  }
  return (100000ULL + static_cast<std::uint64_t>(bucket - 1990) * 1000ULL) * kNsPerUs;
}

std::size_t ErrorIndexFromSample(const RequestSample& sample) {
  if (sample.http_status >= 500 && sample.http_status <= 599) {
    return static_cast<std::size_t>(ErrorClass::kHttp5xx);
  }
  if (sample.http_status >= 400 && sample.http_status <= 499) {
    return static_cast<std::size_t>(ErrorClass::kHttp4xx);
  }
  return static_cast<std::size_t>(sample.error);
}

}  // namespace

struct WriterState {
  struct RingEntry {
    RequestSample sample {};
  };

  explicit WriterState(std::size_t capacity)
      : ring(capacity), ring_mask(capacity - 1) {}

  std::vector<RingEntry> ring;
  const std::size_t ring_mask;
  std::atomic<std::uint64_t> write_index {0};
  std::atomic<std::uint64_t> read_index {0};
  std::atomic<std::uint64_t> dropped {0};
};

struct StatsCollector::Impl {
  struct QpsSlot {
    std::atomic<std::uint64_t> second {0};
    std::atomic<std::uint64_t> count {0};
  };

  explicit Impl(const StatsConfig& cfg)
      : ring_capacity(NormalizePowerOfTwo(cfg.ring_capacity, 1024)),
        qps_slot_count(NormalizePowerOfTwo(cfg.qps_slot_count, 16)),
        qps_mask(qps_slot_count - 1),
        start_ns(NowNs()),
        latency_histogram(kLatencyBucketCount),
        qps_slots(qps_slot_count) {
    for (auto& bucket : latency_histogram) {
      bucket.store(0, std::memory_order_relaxed);
    }
  }

  std::shared_ptr<::titanbench::WriterState> RegisterWriter() {
    auto writer = std::make_shared<WriterState>(ring_capacity);
    std::lock_guard<std::mutex> guard(writers_mu);
    writers.push_back(writer);
    return writer;
  }

  void RecordFastPath(const RequestSample& sample) {
    total_requests.fetch_add(1, std::memory_order_relaxed);
    if (sample.success) {
      success_requests.fetch_add(1, std::memory_order_relaxed);
      error_counts[static_cast<std::size_t>(ErrorClass::kNone)].fetch_add(
          1, std::memory_order_relaxed);
    } else {
      failed_requests.fetch_add(1, std::memory_order_relaxed);
      const std::size_t err_idx = ErrorIndexFromSample(sample);
      error_counts[err_idx].fetch_add(1, std::memory_order_relaxed);
    }

    bytes_sent.fetch_add(sample.bytes_sent, std::memory_order_relaxed);
    bytes_received.fetch_add(sample.bytes_received, std::memory_order_relaxed);
    latency_sum_ns.fetch_add(sample.latency_ns, std::memory_order_relaxed);
    const std::size_t bucket = LatencyToBucket(sample.latency_ns);
    latency_histogram[bucket].fetch_add(1, std::memory_order_relaxed);

    UpdateMin(sample.latency_ns);
    UpdateMax(sample.latency_ns);
    UpdateQps(sample.timestamp_ns == 0 ? NowNs() : sample.timestamp_ns);
  }

  void Drain() {
    std::vector<std::shared_ptr<::titanbench::WriterState>> local_writers;
    {
      std::lock_guard<std::mutex> guard(writers_mu);
      local_writers = writers;
    }

    for (const auto& writer : local_writers) {
      if (!writer) {
        continue;
      }
      std::uint64_t r = writer->read_index.load(std::memory_order_relaxed);
      const std::uint64_t w = writer->write_index.load(std::memory_order_acquire);
      while (r < w) {
        WriterState::RingEntry& entry =
            writer->ring[static_cast<std::size_t>(r & writer->ring_mask)];
        RecordFastPath(entry.sample);
        ++r;
      }
      writer->read_index.store(r, std::memory_order_release);
      dropped_samples.fetch_add(writer->dropped.exchange(0, std::memory_order_acq_rel),
                                std::memory_order_relaxed);
    }
  }

  StatsSnapshot Snapshot(bool drain_before_snapshot) {
    if (drain_before_snapshot) {
      Drain();
    }

    StatsSnapshot out;
    out.total_requests = total_requests.load(std::memory_order_relaxed);
    out.success_requests = success_requests.load(std::memory_order_relaxed);
    out.failed_requests = failed_requests.load(std::memory_order_relaxed);
    out.bytes_sent = bytes_sent.load(std::memory_order_relaxed);
    out.bytes_received = bytes_received.load(std::memory_order_relaxed);
    out.dropped_samples = dropped_samples.load(std::memory_order_relaxed);
    out.elapsed_ns = std::max<std::uint64_t>(1, NowNs() - start_ns);

    out.qps = static_cast<double>(out.total_requests) * 1000000000.0 /
              static_cast<double>(out.elapsed_ns);
    out.throughput_bytes_per_sec =
        static_cast<double>(out.bytes_sent + out.bytes_received) * 1000000000.0 /
        static_cast<double>(out.elapsed_ns);

    if (out.total_requests > 0) {
      out.avg_latency_ns =
          latency_sum_ns.load(std::memory_order_relaxed) / out.total_requests;
      std::uint64_t min_v = min_latency_ns.load(std::memory_order_relaxed);
      if (min_v == std::numeric_limits<std::uint64_t>::max()) {
        min_v = 0;
      }
      out.min_latency_ns = min_v;
      out.max_latency_ns = max_latency_ns.load(std::memory_order_relaxed);
      out.p50_latency_ns = GetPercentileNs(0.50, out.total_requests);
      out.p95_latency_ns = GetPercentileNs(0.95, out.total_requests);
      out.p99_latency_ns = GetPercentileNs(0.99, out.total_requests);
    }

    for (std::size_t i = 0; i < out.error_counts.size(); ++i) {
      out.error_counts[i] = error_counts[i].load(std::memory_order_relaxed);
    }

    const std::uint64_t now_sec = NowNs() / 1000000000ULL;
    QpsSlot& slot = qps_slots[static_cast<std::size_t>(now_sec & qps_mask)];
    if (slot.second.load(std::memory_order_acquire) == now_sec) {
      out.qps_current_second = slot.count.load(std::memory_order_relaxed);
    }

    return out;
  }

  void UpdateMin(std::uint64_t value) {
    std::uint64_t old = min_latency_ns.load(std::memory_order_relaxed);
    while (value < old &&
           !min_latency_ns.compare_exchange_weak(old, value,
                                                 std::memory_order_relaxed)) {
    }
  }

  void UpdateMax(std::uint64_t value) {
    std::uint64_t old = max_latency_ns.load(std::memory_order_relaxed);
    while (value > old &&
           !max_latency_ns.compare_exchange_weak(old, value,
                                                 std::memory_order_relaxed)) {
    }
  }

  void UpdateQps(std::uint64_t ts_ns) {
    const std::uint64_t sec = ts_ns / 1000000000ULL;
    QpsSlot& slot = qps_slots[static_cast<std::size_t>(sec & qps_mask)];
    std::uint64_t slot_sec = slot.second.load(std::memory_order_acquire);
    if (slot_sec != sec) {
      if (slot.second.compare_exchange_strong(slot_sec, sec,
                                              std::memory_order_acq_rel)) {
        slot.count.store(0, std::memory_order_release);
      }
    }
    slot.count.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t GetPercentileNs(double pct, std::uint64_t total) const {
    if (total == 0) {
      return 0;
    }
    std::uint64_t target = static_cast<std::uint64_t>(pct * static_cast<double>(total));
    if (target == 0) {
      target = 1;
    }
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < latency_histogram.size(); ++i) {
      cumulative += latency_histogram[i].load(std::memory_order_relaxed);
      if (cumulative >= target) {
        return BucketToLatencyUpperNs(i);
      }
    }
    return BucketToLatencyUpperNs(latency_histogram.size() - 1);
  }

  const std::size_t ring_capacity;
  const std::size_t qps_slot_count;
  const std::size_t qps_mask;
  const std::uint64_t start_ns;

  std::mutex writers_mu;
  std::vector<std::shared_ptr<::titanbench::WriterState>> writers;

  std::atomic<std::uint64_t> total_requests {0};
  std::atomic<std::uint64_t> success_requests {0};
  std::atomic<std::uint64_t> failed_requests {0};
  std::atomic<std::uint64_t> bytes_sent {0};
  std::atomic<std::uint64_t> bytes_received {0};
  std::atomic<std::uint64_t> latency_sum_ns {0};
  std::atomic<std::uint64_t> min_latency_ns {std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> max_latency_ns {0};
  std::atomic<std::uint64_t> dropped_samples {0};

  std::vector<std::atomic<std::uint64_t>> latency_histogram;
  std::array<std::atomic<std::uint64_t>,
             static_cast<std::size_t>(ErrorClass::kCount)>
      error_counts {};
  std::vector<QpsSlot> qps_slots;
};

struct StatsWriter::Impl {
  explicit Impl(std::shared_ptr<::titanbench::WriterState> writer_state)
      : state(std::move(writer_state)) {}

  std::shared_ptr<::titanbench::WriterState> state;
};

StatsWriter::StatsWriter() = default;

StatsWriter::StatsWriter(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

StatsWriter::~StatsWriter() = default;

StatsWriter::StatsWriter(StatsWriter&& other) noexcept : impl_(std::move(other.impl_)) {}

StatsWriter& StatsWriter::operator=(StatsWriter&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void StatsWriter::Record(const RequestSample& sample) {
  if (!impl_ || !impl_->state) {
    return;
  }

  auto& state = *impl_->state;
  const std::uint64_t write = state.write_index.load(std::memory_order_relaxed);
  const std::uint64_t read = state.read_index.load(std::memory_order_acquire);
  if (write - read >= state.ring.size()) {
    state.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  state.ring[static_cast<std::size_t>(write & state.ring_mask)].sample = sample;
  state.write_index.store(write + 1, std::memory_order_release);
}

StatsCollector::StatsCollector(const StatsConfig& config)
    : impl_(new Impl(config)) {}

StatsCollector::~StatsCollector() = default;

std::unique_ptr<StatsWriter> StatsCollector::CreateWriter() {
  std::shared_ptr<::titanbench::WriterState> writer_state = impl_->RegisterWriter();
  auto writer_impl = std::make_shared<StatsWriter::Impl>(writer_state);
  return std::unique_ptr<StatsWriter>(new StatsWriter(writer_impl));
}

void StatsCollector::Drain() { impl_->Drain(); }

StatsSnapshot StatsCollector::Snapshot(bool drain_before_snapshot) {
  return impl_->Snapshot(drain_before_snapshot);
}

}  // namespace titanbench
