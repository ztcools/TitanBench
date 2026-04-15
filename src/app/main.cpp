#include <cstdlib>
#include <csignal>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "cli/args.h"
#include "cli/report.h"
#include "core/config.h"
#include "core/engine.h"

namespace titanbench {
namespace {

std::mutex g_signal_mutex;
bool g_signal_triggered = false;

void FatalSignalHandler(int signo) {
  {
    std::lock_guard<std::mutex> guard(g_signal_mutex);
    g_signal_triggered = true;
  }
  std::cerr << "\nFatal signal captured: " << signo
            << ", benchmark aborted safely.\n";
  std::_Exit(EXIT_FAILURE);
}

void InstallCrashGuards() {
  std::signal(SIGSEGV, FatalSignalHandler);
  std::signal(SIGABRT, FatalSignalHandler);
  std::signal(SIGFPE, FatalSignalHandler);
  std::signal(SIGILL, FatalSignalHandler);
  std::signal(SIGTERM, FatalSignalHandler);
  std::set_terminate([]() {
    std::cerr << "\nUnhandled exception, benchmark terminated.\n";
    std::_Exit(EXIT_FAILURE);
  });
}

void PrintConfig(const Config& config) {
  std::cout << "titanbench config:\n"
            << "  concurrency: " << config.concurrency << "\n"
            << "  requests: "
            << (config.has_requests ? std::to_string(config.requests) : "-")
            << "\n"
            << "  duration_seconds: "
            << (config.has_duration ? std::to_string(config.duration_seconds) : "-")
            << "\n"
            << "  threads: " << config.threads << "\n"
            << "  protocol: " << ProtocolToString(config.protocol) << "\n"
            << "  host: " << config.host << "\n"
            << "  port: " << config.port << "\n"
            << "  path: " << config.path << "\n";
}

}  // namespace
}  // namespace titanbench

int main(int argc, char* argv[]) {
  titanbench::InstallCrashGuards();
  titanbench::Config config;
  std::string error_message;

  if (!titanbench::ParseArgs(argc, argv, &config, &error_message)) {
    std::cerr << "Error: " << error_message << "\n\n"
              << titanbench::BuildHelpMessage(argv[0]);
    return EXIT_FAILURE;
  }

  if (config.show_help) {
    std::cout << titanbench::BuildHelpMessage(argv[0]);
    return EXIT_SUCCESS;
  }

  if (config.show_version) {
    std::cout << titanbench::BuildVersionMessage() << "\n";
    return EXIT_SUCCESS;
  }

  if (!titanbench::ValidateConfig(&config, &error_message)) {
    std::cerr << "Error: " << error_message << "\n\n"
              << titanbench::BuildHelpMessage(argv[0]);
    return EXIT_FAILURE;
  }

  titanbench::PrintConfig(config);
  titanbench::CliReportPrinter reporter(config);
  titanbench::BenchmarkEngine engine(config);
  std::atomic<bool> engine_finished {false};
  std::thread watchdog([&config, &engine_finished]() {
    // Hard timeout guard: duration mode + 60s buffer, requests mode 1h upper bound.
    int guard_seconds = config.has_duration ? (config.duration_seconds + 60) : 3600;
    std::this_thread::sleep_for(std::chrono::seconds(guard_seconds));
    if (!engine_finished.load(std::memory_order_acquire)) {
      std::cerr << "\nTimeout guard triggered, forcing exit.\n";
      std::_Exit(EXIT_FAILURE);
    }
  });
  if (!engine.Run(&error_message, [&reporter](const titanbench::EngineReport& progress) {
        reporter.PrintProgress(progress);
      })) {
    engine_finished.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
      watchdog.join();
    }
    std::cerr << "Engine error: " << error_message << "\n";
    return EXIT_FAILURE;
  }
  engine_finished.store(true, std::memory_order_release);
  if (watchdog.joinable()) {
    watchdog.join();
  }

  reporter.PrintFinal(engine.report());
  return EXIT_SUCCESS;
}
