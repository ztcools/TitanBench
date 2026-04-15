#include "cli/report.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "cli/args.h"

namespace titanbench {
namespace {

std::string FormatNs(std::uint64_t ns) {
  std::ostringstream oss;
  if (ns >= 1000000000ULL) {
    oss << std::fixed << std::setprecision(2)
        << static_cast<double>(ns) / 1000000000.0 << " s";
  } else if (ns >= 1000000ULL) {
    oss << std::fixed << std::setprecision(2)
        << static_cast<double>(ns) / 1000000.0 << " ms";
  } else if (ns >= 1000ULL) {
    oss << std::fixed << std::setprecision(2)
        << static_cast<double>(ns) / 1000.0 << " us";
  } else {
    oss << ns << " ns";
  }
  return oss.str();
}

std::string FormatRate(double value) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << value;
  return oss.str();
}

double SuccessRate(const EngineReport& report) {
  if (report.completed_requests == 0) {
    return 0.0;
  }
  return static_cast<double>(report.succeeded_requests) * 100.0 /
         static_cast<double>(report.completed_requests);
}

}  // namespace

CliReportPrinter::CliReportPrinter(const Config& config, std::ostream* out)
    : config_(config), out_(out == nullptr ? &std::cout : out) {}

void CliReportPrinter::PrintProgress(const EngineReport& report) {
  *out_ << "\r" << BuildProgressLine(report) << std::flush;
  printed_progress_ = true;
}

void CliReportPrinter::PrintFinal(const EngineReport& report) {
  if (printed_progress_) {
    *out_ << "\n";
  }
  *out_ << BuildFinalReport(report);
}

std::string CliReportPrinter::BuildProgressLine(const EngineReport& report) const {
  std::ostringstream oss;
  oss << "[running] "
      << "elapsed=" << std::fixed << std::setprecision(1)
      << static_cast<double>(report.elapsed_ms) / 1000.0 << "s"
      << " qps=" << FormatRate(report.stats.qps_current_second)
      << " done=" << report.completed_requests
      << " ok=" << report.succeeded_requests
      << " fail=" << report.failed_requests
      << " throughput=" << FormatRate(report.stats.throughput_bytes_per_sec) << " B/s";
  return oss.str();
}

std::string CliReportPrinter::BuildFinalReport(const EngineReport& report) const {
  const StatsSnapshot& s = report.stats;
  std::ostringstream oss;
  oss << "==================== titanbench report ====================\n";
  oss << "[target]\n";
  oss << "  protocol       : " << ProtocolToString(config_.protocol) << "\n";
  oss << "  host           : " << config_.host << "\n";
  oss << "  port           : " << config_.port << "\n";
  if (config_.protocol == Protocol::kHttp || config_.protocol == Protocol::kHttps) {
    oss << "  path           : " << config_.path << "\n";
  }

  oss << "\n[benchmark params]\n";
  oss << "  concurrency    : " << config_.concurrency << "\n";
  oss << "  threads        : " << config_.threads << "\n";
  oss << "  mode           : "
      << (config_.has_requests ? "-n total requests" : "-t duration") << "\n";
  oss << "  requests       : "
      << (config_.has_requests ? std::to_string(config_.requests) : "-") << "\n";
  oss << "  duration       : "
      << (config_.has_duration ? std::to_string(config_.duration_seconds) + " s" : "-")
      << "\n";

  oss << "\n[overall]\n";
  oss << "  elapsed        : " << std::fixed << std::setprecision(3)
      << static_cast<double>(report.elapsed_ms) / 1000.0 << " s\n";
  oss << "  started        : " << report.started_requests << "\n";
  oss << "  completed      : " << report.completed_requests << "\n";
  oss << "  success        : " << report.succeeded_requests << "\n";
  oss << "  failed         : " << report.failed_requests << "\n";
  oss << "  retried        : " << report.retried_requests << "\n";

  oss << "\n[latency]\n";
  oss << "  avg            : " << FormatNs(s.avg_latency_ns) << "\n";
  oss << "  min            : " << FormatNs(s.min_latency_ns) << "\n";
  oss << "  max            : " << FormatNs(s.max_latency_ns) << "\n";
  oss << "  p50            : " << FormatNs(s.p50_latency_ns) << "\n";
  oss << "  p95            : " << FormatNs(s.p95_latency_ns) << "\n";
  oss << "  p99            : " << FormatNs(s.p99_latency_ns) << "\n";

  oss << "\n[qps]\n";
  oss << "  avg qps        : " << FormatRate(s.qps) << "\n";
  oss << "  current qps    : " << s.qps_current_second << "\n";

  oss << "\n[success ratio]\n";
  oss << "  success rate   : " << std::fixed << std::setprecision(2)
      << SuccessRate(report) << " %\n";

  oss << "\n[errors]\n";
  oss << "  connect failed : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kConnectFailed)] << "\n";
  oss << "  timeout        : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kTimeout)] << "\n";
  oss << "  http 4xx       : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kHttp4xx)] << "\n";
  oss << "  http 5xx       : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kHttp5xx)] << "\n";
  oss << "  network io     : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kNetworkIo)] << "\n";
  oss << "  tls handshake  : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kTlsHandshake)] << "\n";
  oss << "  parse error    : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kParseError)] << "\n";
  oss << "  other          : "
      << s.error_counts[static_cast<std::size_t>(ErrorClass::kOther)] << "\n";

  oss << "\n[network]\n";
  oss << "  bytes sent     : " << s.bytes_sent << "\n";
  oss << "  bytes recv     : " << s.bytes_received << "\n";
  oss << "  throughput     : " << FormatRate(s.throughput_bytes_per_sec) << " B/s\n";
  oss << "  dropped sample : " << s.dropped_samples << "\n";
  oss << "============================================================\n";
  return oss.str();
}

}  // namespace titanbench
