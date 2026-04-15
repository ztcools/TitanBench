#include "cli/args.h"

#include <getopt.h>
#include <limits>
#include <sstream>
#include <string>

namespace titanbench {
namespace {

bool ParsePositiveInt(const std::string& input, int* value) {
  if (input.empty()) {
    return false;
  }

  std::size_t index = 0;
  long long parsed = 0;
  try {
    parsed = std::stoll(input, &index, 10);
  } catch (...) {
    return false;
  }

  if (index != input.size()) {
    return false;
  }
  if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

bool ParsePositiveInt64(const std::string& input, std::int64_t* value) {
  if (input.empty()) {
    return false;
  }

  std::size_t index = 0;
  long long parsed = 0;
  try {
    parsed = std::stoll(input, &index, 10);
  } catch (...) {
    return false;
  }

  if (index != input.size() || parsed <= 0) {
    return false;
  }

  *value = static_cast<std::int64_t>(parsed);
  return true;
}

bool ParsePort(const std::string& input, int* value) {
  int port = 0;
  if (!ParsePositiveInt(input, &port)) {
    return false;
  }
  if (port > 65535) {
    return false;
  }

  *value = port;
  return true;
}

}  // namespace

bool ParseProtocol(const std::string& protocol_text, Protocol* protocol) {
  if (protocol_text == "http") {
    *protocol = Protocol::kHttp;
    return true;
  }
  if (protocol_text == "https") {
    *protocol = Protocol::kHttps;
    return true;
  }
  if (protocol_text == "tcp") {
    *protocol = Protocol::kTcp;
    return true;
  }
  if (protocol_text == "udp") {
    *protocol = Protocol::kUdp;
    return true;
  }
  return false;
}

std::string ProtocolToString(Protocol protocol) {
  switch (protocol) {
    case Protocol::kHttp:
      return "http";
    case Protocol::kHttps:
      return "https";
    case Protocol::kTcp:
      return "tcp";
    case Protocol::kUdp:
      return "udp";
  }
  return "unknown";
}

std::string BuildHelpMessage(const char* program_name) {
  std::ostringstream oss;
  oss << "Usage: " << program_name
      << " -c <concurrency> (-n <requests> | -t <seconds>) [options]\n\n"
      << "Options:\n"
      << "  -c, --concurrency <num>   Number of concurrent connections "
         "(required)\n"
      << "  -n, --requests <num>      Total number of requests "
         "(mutually exclusive with -t)\n"
      << "  -t, --time <seconds>      Benchmark duration in seconds "
         "(mutually exclusive with -n)\n"
      << "  -T, --threads <num>       Worker threads (default: 1)\n"
      << "  -p, --protocol <proto>    Protocol type: http/https/tcp/udp "
         "(default: http)\n"
      << "  -h, --host <host>         Target host (required)\n"
      << "  -P, --port <port>         Target port (required)\n"
      << "      --path <path>         HTTP path (default: /)\n"
      << "      --help                Show this help message\n"
      << "      --version             Show version information\n";
  return oss.str();
}

std::string BuildVersionMessage() { return kVersion; }

bool ParseArgs(int argc, char* argv[], Config* config, std::string* error_message) {
  if (config == nullptr || error_message == nullptr) {
    return false;
  }

  opterr = 0;

  const option long_options[] = {
      {"concurrency", required_argument, nullptr, 'c'},
      {"requests", required_argument, nullptr, 'n'},
      {"time", required_argument, nullptr, 't'},
      {"threads", required_argument, nullptr, 'T'},
      {"protocol", required_argument, nullptr, 'p'},
      {"host", required_argument, nullptr, 'h'},
      {"port", required_argument, nullptr, 'P'},
      {"path", required_argument, nullptr, 1000},
      {"help", no_argument, nullptr, 1001},
      {"version", no_argument, nullptr, 1002},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  int c = 0;
  while ((c = getopt_long(argc, argv, "c:n:t:T:p:h:P:", long_options,
                          &option_index)) != -1) {
    switch (c) {
      case 'c': {
        if (!ParsePositiveInt(optarg, &config->concurrency)) {
          *error_message = "invalid value for --concurrency";
          return false;
        }
        break;
      }
      case 'n': {
        if (!ParsePositiveInt64(optarg, &config->requests)) {
          *error_message = "invalid value for --requests";
          return false;
        }
        config->has_requests = true;
        break;
      }
      case 't': {
        if (!ParsePositiveInt(optarg, &config->duration_seconds)) {
          *error_message = "invalid value for --time";
          return false;
        }
        config->has_duration = true;
        break;
      }
      case 'T': {
        if (!ParsePositiveInt(optarg, &config->threads)) {
          *error_message = "invalid value for --threads";
          return false;
        }
        break;
      }
      case 'p': {
        if (!ParseProtocol(optarg, &config->protocol)) {
          *error_message = "invalid value for --protocol, expected "
                           "http/https/tcp/udp";
          return false;
        }
        break;
      }
      case 'h': {
        config->host = optarg;
        break;
      }
      case 'P': {
        if (!ParsePort(optarg, &config->port)) {
          *error_message = "invalid value for --port";
          return false;
        }
        break;
      }
      case 1000: {
        config->path = optarg;
        break;
      }
      case 1001: {
        config->show_help = true;
        return true;
      }
      case 1002: {
        config->show_version = true;
        return true;
      }
      case '?': {
        *error_message = "unknown or malformed option";
        return false;
      }
      default: {
        *error_message = "unexpected argument parsing failure";
        return false;
      }
    }
  }

  if (optind < argc) {
    *error_message = "unexpected positional arguments";
    return false;
  }

  return true;
}

bool ValidateConfig(Config* config, std::string* error_message) {
  if (config == nullptr || error_message == nullptr) {
    return false;
  }

  if (config->show_help || config->show_version) {
    return true;
  }

  if (config->concurrency <= 0) {
    *error_message = "missing required option: --concurrency";
    return false;
  }
  if (config->host.empty()) {
    *error_message = "missing required option: --host";
    return false;
  }
  if (config->port <= 0) {
    *error_message = "missing required option: --port";
    return false;
  }
  if (config->has_requests == config->has_duration) {
    *error_message = "either --requests or --time must be set, but not both";
    return false;
  }
  if (config->path.empty()) {
    config->path = "/";
  }

  if ((config->protocol == Protocol::kHttp ||
       config->protocol == Protocol::kHttps) &&
      config->path.front() != '/') {
    config->path = "/" + config->path;
  }

  return true;
}

}  // namespace titanbench
