#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#include "asyncio/backend/selector_epoll.h"
#include "asyncio/backend/selector_select.hh"

namespace {

struct Row {
  std::string backend;
  int fds{};
  int iterations{};
  double mean_us{};
  double median_us{};
  double p95_us{};
};

struct Options {
  int iterations{5000};
  double timeout_ms{100.0};
  std::vector<int> fds{32, 64, 128, 256, 512};
  std::string csv_out{"benchmarks/select_vs_epoll_results.csv"};
  uint32_t seed{42};
};

struct PipeSet {
  int read_fd{-1};
  int write_fd{-1};
};

void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    throw std::system_error(errno, std::system_category(), "fcntl(F_GETFL) failed");
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    throw std::system_error(errno, std::system_category(), "fcntl(F_SETFL) failed");
  }
}

std::vector<PipeSet> CreatePipes(int n) {
  std::vector<PipeSet> pipes;
  pipes.reserve(static_cast<size_t>(n));

  for (int i = 0; i < n; ++i) {
    std::array<int, 2> fds{-1, -1};
#if defined(O_CLOEXEC) && defined(O_NONBLOCK)
    if (::pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
      if (errno != ENOSYS) {
        throw std::system_error(errno, std::system_category(), "pipe2 failed");
      }
      if (::pipe(fds.data()) != 0) {
        throw std::system_error(errno, std::system_category(), "pipe failed");
      }
      SetNonBlocking(fds[0]);
      SetNonBlocking(fds[1]);
    }
#else
    if (::pipe(fds.data()) != 0) {
      throw std::system_error(errno, std::system_category(), "pipe failed");
    }
    SetNonBlocking(fds[0]);
    SetNonBlocking(fds[1]);
#endif
    pipes.push_back(PipeSet{fds[0], fds[1]});
  }

  return pipes;
}

void ClosePipes(const std::vector<PipeSet>& pipes) {
  for (const PipeSet& p : pipes) {
    if (p.read_fd >= 0) {
      ::close(p.read_fd);
    }
    if (p.write_fd >= 0) {
      ::close(p.write_fd);
    }
  }
}

double Percentile(const std::vector<double>& sorted_values, double p) {
  if (sorted_values.empty()) return 0.0;
  if (p <= 0.0) return sorted_values.front();
  if (p >= 100.0) return sorted_values.back();

  const double k = (static_cast<double>(sorted_values.size()) - 1.0) * (p / 100.0);
  const size_t lower = static_cast<size_t>(k);
  const size_t upper = std::min(lower + 1, sorted_values.size() - 1);
  const double frac = k - static_cast<double>(lower);
  return sorted_values[lower] * (1.0 - frac) + sorted_values[upper] * frac;
}

Row Summarize(std::string backend, int nfds, const std::vector<double>& samples) {
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  const double mean = samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
  const double median = sorted.empty() ? 0.0 : Percentile(sorted, 50.0);
  const double p95 = sorted.empty() ? 0.0 : Percentile(sorted, 95.0);

  return Row{
      .backend = std::move(backend),
      .fds = nfds,
      .iterations = static_cast<int>(samples.size()),
      .mean_us = mean,
      .median_us = median,
      .p95_us = p95,
  };
}

template <typename SelectorT>
std::vector<double> RunBackend(SelectorT& selector, int nfds, int iterations, double timeout_ms,
                               std::mt19937& rng) {
  std::vector<double> latencies_us;
  latencies_us.reserve(static_cast<size_t>(iterations));

  auto pipes = CreatePipes(nfds);
  std::vector<asyncio::IoEvent> events(static_cast<size_t>(nfds));

  for (const PipeSet& p : pipes) {
    selector.Register(p.read_fd, asyncio::IoEventFlags::kReadable, nullptr);
  }

  std::uniform_int_distribution<int> dist(0, nfds - 1);
  const auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double, std::milli>(timeout_ms));

  try {
    for (int i = 0; i < iterations; ++i) {
      const int idx = dist(rng);
      const PipeSet& target = pipes[static_cast<size_t>(idx)];

      const ssize_t wrote = ::write(target.write_fd, "x", 1);
      if (wrote != 1) {
        throw std::system_error(errno, std::system_category(), "write to pipe failed");
      }

      const auto start = std::chrono::steady_clock::now();
      const int ready = selector.Select(events, timeout);
      const auto elapsed = std::chrono::steady_clock::now() - start;

      if (ready <= 0) {
        throw std::runtime_error("selector timed out waiting for readiness event");
      }

      for (int r = 0; r < ready; ++r) {
        const int fd = events[static_cast<size_t>(r)].handle;
        char byte = 0;
        for (;;) {
          const ssize_t n = ::read(fd, &byte, 1);
          if (n == 1) {
            break;
          }
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
          }
          if (n < 0 && errno == EINTR) {
            continue;
          }
          if (n == 0) {
            break;
          }
          throw std::system_error(errno, std::system_category(), "read from pipe failed");
        }
      }

      latencies_us.push_back(
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
          1000.0);
    }
  } catch (...) {
    for (const PipeSet& p : pipes) {
      try {
        selector.Unregister(p.read_fd);
      } catch (...) {
      }
    }
    ClosePipes(pipes);
    throw;
  }

  for (const PipeSet& p : pipes) {
    selector.Unregister(p.read_fd);
  }
  ClosePipes(pipes);
  return latencies_us;
}

void WriteCsv(const std::vector<Row>& rows, std::string_view path) {
  std::ofstream out{std::string(path)};
  if (!out.is_open()) {
    throw std::runtime_error("failed to open CSV output path");
  }

  out << "backend,fds,iterations,mean_us,median_us,p95_us\n";
  out << std::fixed << std::setprecision(3);
  for (const Row& row : rows) {
    out << row.backend << ',' << row.fds << ',' << row.iterations << ',' << row.mean_us << ','
        << row.median_us << ',' << row.p95_us << '\n';
  }
}

int ParsePositiveInt(const std::string& value, const char* flag_name) {
  try {
    const int parsed = std::stoi(value);
    if (parsed <= 0) {
      throw std::invalid_argument("non-positive");
    }
    return parsed;
  } catch (const std::exception&) {
    std::ostringstream oss;
    oss << "invalid " << flag_name << ": " << value;
    throw std::invalid_argument(oss.str());
  }
}

double ParsePositiveDouble(const std::string& value, const char* flag_name) {
  try {
    const double parsed = std::stod(value);
    if (parsed <= 0.0) {
      throw std::invalid_argument("non-positive");
    }
    return parsed;
  } catch (const std::exception&) {
    std::ostringstream oss;
    oss << "invalid " << flag_name << ": " << value;
    throw std::invalid_argument(oss.str());
  }
}

Options ParseArgs(int argc, char** argv) {
  Options opts;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--iterations") {
      if (i + 1 >= argc) throw std::invalid_argument("--iterations requires a value");
      opts.iterations = ParsePositiveInt(argv[++i], "--iterations");
      continue;
    }

    if (arg == "--timeout-ms") {
      if (i + 1 >= argc) throw std::invalid_argument("--timeout-ms requires a value");
      opts.timeout_ms = ParsePositiveDouble(argv[++i], "--timeout-ms");
      continue;
    }

    if (arg == "--seed") {
      if (i + 1 >= argc) throw std::invalid_argument("--seed requires a value");
      opts.seed = static_cast<uint32_t>(ParsePositiveInt(argv[++i], "--seed"));
      continue;
    }

    if (arg == "--csv-out") {
      if (i + 1 >= argc) throw std::invalid_argument("--csv-out requires a value");
      opts.csv_out = argv[++i];
      continue;
    }

    if (arg == "--fds") {
      opts.fds.clear();
      while (i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0) {
        opts.fds.push_back(ParsePositiveInt(argv[++i], "--fds value"));
      }
      if (opts.fds.empty()) {
        throw std::invalid_argument("--fds requires at least one value");
      }
      continue;
    }

    if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: select_vs_epoll_benchmark_cpp [options]\n"
          << "  --iterations N        Samples per FD size (default 5000)\n"
          << "  --timeout-ms T        Poll timeout in milliseconds (default 100.0)\n"
          << "  --fds N1 N2 ...       FD sizes to test (default 32 64 128 256 512)\n"
          << "  --seed S              Random seed (default 42)\n"
          << "  --csv-out PATH        Output CSV path (default benchmarks/select_vs_epoll_results.csv)\n";
      std::exit(0);
    }

    throw std::invalid_argument("unknown argument: " + arg);
  }

  std::sort(opts.fds.begin(), opts.fds.end());
  opts.fds.erase(std::unique(opts.fds.begin(), opts.fds.end()), opts.fds.end());
  return opts;
}

int MainImpl(int argc, char** argv) {
  Options opts = ParseArgs(argc, argv);
  std::mt19937 rng(opts.seed);
  std::vector<Row> rows;

  for (int nfds : opts.fds) {
    try {
      asyncio::SelectSelector selector;
      rows.push_back(Summarize("select", nfds,
                               RunBackend(selector, nfds, opts.iterations, opts.timeout_ms, rng)));
    } catch (const std::exception& ex) {
      std::cerr << "Skipping select nfds=" << nfds << ": " << ex.what() << '\n';
    }
  }

#if defined(ASYNCIO_OS_LINUX)
  for (int nfds : opts.fds) {
    try {
      asyncio::EpollSelector selector;
      rows.push_back(Summarize("epoll", nfds,
                               RunBackend(selector, nfds, opts.iterations, opts.timeout_ms, rng)));
    } catch (const std::exception& ex) {
      std::cerr << "Skipping epoll nfds=" << nfds << ": " << ex.what() << '\n';
    }
  }
#else
  std::cerr << "epoll backend is unavailable on this platform\n";
#endif

  if (rows.empty()) {
    throw std::runtime_error("no benchmark rows produced");
  }

  WriteCsv(rows, opts.csv_out);

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.backend != b.backend) return a.backend < b.backend;
    return a.fds < b.fds;
  });

  std::cout << "Benchmark summary (microseconds):\n";
  std::cout << std::fixed << std::setprecision(3);
  for (const Row& row : rows) {
    std::cout << std::setw(6) << row.backend << " nfds=" << std::setw(4) << row.fds
              << " mean=" << std::setw(8) << row.mean_us << " median=" << std::setw(8)
              << row.median_us << " p95=" << std::setw(8) << row.p95_us << '\n';
  }
  std::cout << "CSV:  " << opts.csv_out << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return MainImpl(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return 1;
  }
}
