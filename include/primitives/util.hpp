#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  inline void cpu_relax() noexcept { _mm_pause(); }
#elif defined(__aarch64__)
  inline void cpu_relax() noexcept { asm volatile("yield" ::: "memory"); }
#else
  inline void cpu_relax() noexcept { std::this_thread::yield(); }
#endif

// bias threads toward performance cores on macOS (QoS hint)
#if defined(__APPLE__)
  #include <pthread.h>
  inline void set_thread_high_priority() noexcept {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  }
#else
  inline void set_thread_high_priority() noexcept {}
#endif

// pin calling thread to a specific core (Linux only)
#if defined(__linux__)
  #include <sched.h>
  inline bool set_thread_affinity(int core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
  }
  // Re-allow scheduling on every online CPU. Use after a temporary pin
  // (e.g. NUMA-local prefill) so the kernel can again place subsequently
  // spawned threads anywhere. The kernel intersects this mask with the
  // currently-online CPUs, so setting every bit is safe.
  inline bool clear_thread_affinity() noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (std::size_t i = 0; i < CPU_SETSIZE; ++i) CPU_SET(i, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
  }
#else
  inline bool set_thread_affinity(int) noexcept { return false; }
  inline bool clear_thread_affinity() noexcept { return false; }
#endif

// === Topology-aware pinning ===
// Reads sysfs to discover socket / physical-core layout, then maps worker
// indices to logical CPU IDs under one of three policies:
//   linear        — worker_idx i → logical CPU i (legacy)
//   compact_phys  — fill physical cores socket-0-first; SMT siblings only after
//                   physical cores are exhausted. Eliminates SMT contention.
//   compact_socket— fill socket 0 fully (incl. SMT) before moving to socket 1.
// On macOS / containers without sysfs, falls back to linear.
enum class pin_policy { off, linear, compact_phys, compact_socket };

inline pin_policy parse_pin_policy(const std::string& s) noexcept {
  if (s == "off" || s == "none") return pin_policy::off;
  if (s == "linear" || s == "lin") return pin_policy::linear;
  if (s == "phys" || s == "compact_phys") return pin_policy::compact_phys;
  if (s == "socket" || s == "compact_socket") return pin_policy::compact_socket;
  return pin_policy::compact_phys;  // sensible default for unknown input
}

inline const char* pin_policy_name(pin_policy p) noexcept {
  switch (p) {
    case pin_policy::off: return "off";
    case pin_policy::linear: return "linear";
    case pin_policy::compact_phys: return "compact_phys";
    case pin_policy::compact_socket: return "compact_socket";
  }
  return "unknown";
}

struct cpu_topology {
  // For each logical CPU id, which socket it lives on (-1 = unknown)
  std::vector<int> socket_of;
  // For each logical CPU id, the (socket, core_id) it belongs to
  std::vector<std::pair<int,int>> core_of;
  // List of online logical CPU ids
  std::vector<int> online;
  // Pre-computed pin orders, indexed by worker_idx → logical CPU id
  std::vector<int> order_compact_phys;
  std::vector<int> order_compact_socket;
  int n_logical = 1;
  int n_physical = 1;
  int n_sockets = 1;
};

#if defined(__linux__)
namespace detail {
  // Parse a sysfs cpu-list like "0-3,5,7-11" into a vector of ints.
  inline std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string range;
    while (std::getline(ss, range, ',')) {
      auto dash = range.find('-');
      if (dash == std::string::npos) {
        try { out.push_back(std::stoi(range)); } catch (...) {}
      } else {
        try {
          int a = std::stoi(range.substr(0, dash));
          int b = std::stoi(range.substr(dash + 1));
          for (int x = a; x <= b; ++x) out.push_back(x);
        } catch (...) {}
      }
    }
    return out;
  }

  inline int read_int_file(const std::string& path, int fallback = -1) {
    std::ifstream f(path);
    int v = fallback;
    if (f) f >> v;
    return v;
  }

  inline std::string read_string_file(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return s;
  }
}  // namespace detail
#endif

inline const cpu_topology& probe_topology() noexcept {
  static const cpu_topology t = []{
    cpu_topology r;
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 1) hw = 1;
    r.n_logical = hw;
#if defined(__linux__)
    // Discover online CPUs.
    std::string online_str = detail::read_string_file("/sys/devices/system/cpu/online");
    if (!online_str.empty()) {
      r.online = detail::parse_cpu_list(online_str);
    }
    if (r.online.empty()) {
      r.online.reserve(hw);
      for (int i = 0; i < hw; ++i) r.online.push_back(i);
    }
    int max_id = *std::max_element(r.online.begin(), r.online.end());
    r.socket_of.assign(max_id + 1, -1);
    r.core_of.assign(max_id + 1, {-1, -1});

    // Read per-CPU topology.
    for (int cpu : r.online) {
      std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
      int pkg = detail::read_int_file(base + "physical_package_id", 0);
      int core = detail::read_int_file(base + "core_id", cpu);
      r.socket_of[cpu] = pkg;
      r.core_of[cpu] = {pkg, core};
    }

    // Group logical CPUs by (socket, core) to find SMT siblings.
    // Use a sorted vector of (socket, core, cpu) triples.
    std::vector<std::tuple<int,int,int>> tri;
    tri.reserve(r.online.size());
    for (int cpu : r.online) {
      auto& c = r.core_of[cpu];
      tri.emplace_back(c.first, c.second, cpu);
    }
    std::sort(tri.begin(), tri.end());

    // Build compact_phys: one logical CPU per physical core, socket 0 first.
    // Then SMT siblings (sibling 1, sibling 2, ...) for the same physical-core ordering.
    {
      std::vector<std::vector<int>> rounds;  // rounds[k] = k-th sibling of each phys core
      std::pair<int,int> last_core = {-2, -2};
      size_t round_idx = 0;
      for (auto& [sock, core, cpu] : tri) {
        std::pair<int,int> key = {sock, core};
        if (key != last_core) { last_core = key; round_idx = 0; }
        if (rounds.size() <= round_idx) rounds.emplace_back();
        rounds[round_idx].push_back(cpu);
        ++round_idx;
      }
      r.n_physical = rounds.empty() ? r.n_logical : (int)rounds[0].size();
      for (auto& round : rounds)
        for (int cpu : round) r.order_compact_phys.push_back(cpu);
    }

    // Build compact_socket: all CPUs of socket 0 (in sysfs order), then socket 1, ...
    {
      std::vector<int> sockets;
      for (auto& [s, c, cpu] : tri) sockets.push_back(s);
      std::sort(sockets.begin(), sockets.end());
      sockets.erase(std::unique(sockets.begin(), sockets.end()), sockets.end());
      r.n_sockets = (int)sockets.size();
      for (int sock : sockets)
        for (auto& [s, c, cpu] : tri)
          if (s == sock) r.order_compact_socket.push_back(cpu);
    }
#else
    // Non-Linux fallback: assume linear, single socket, no SMT info.
    r.online.reserve(hw);
    for (int i = 0; i < hw; ++i) r.online.push_back(i);
    r.socket_of.assign(hw, 0);
    r.core_of.assign(hw, {0, 0});
    r.order_compact_phys = r.online;
    r.order_compact_socket = r.online;
    r.n_physical = hw;
    r.n_sockets = 1;
#endif
    return r;
  }();
  return t;
}

// Resolve a worker index (0..N-1) to a logical CPU id under the given policy.
// Returns -1 if no pinning should be performed (pin_policy::off or out-of-range).
inline int resolve_pin_target(int worker_idx, pin_policy policy) noexcept {
  if (policy == pin_policy::off || worker_idx < 0) return -1;
  const cpu_topology& t = probe_topology();
  const std::vector<int>* order = nullptr;
  switch (policy) {
    case pin_policy::off: return -1;
    case pin_policy::linear:        return worker_idx;  // direct logical id
    case pin_policy::compact_phys:  order = &t.order_compact_phys; break;
    case pin_policy::compact_socket:order = &t.order_compact_socket; break;
  }
  if (!order || order->empty()) return worker_idx;
  if (worker_idx < (int)order->size()) return (*order)[worker_idx];
  // More workers than logical CPUs: wrap into the order.
  return (*order)[worker_idx % (int)order->size()];
}

// call at the start of each worker thread before the barrier
inline void setup_worker_thread([[maybe_unused]] int thread_id,
                                [[maybe_unused]] pin_policy policy) noexcept {
  set_thread_high_priority();
#if defined(__linux__)
  int target = resolve_pin_target(thread_id, policy);
  if (target >= 0) set_thread_affinity(target);
#endif
}

// Backward-compat overload: bool true → compact_phys, false → off.
inline void setup_worker_thread(int thread_id, bool pin) noexcept {
  setup_worker_thread(thread_id, pin ? pin_policy::compact_phys : pin_policy::off);
}

// spin barrier - threads wait here until everyone is ready
struct start_barrier {
  std::atomic<int> arrived{0};
  std::atomic<bool> go{false};
  int total;

  explicit start_barrier(int n) : total(n) {}

  void arrive_and_wait() {
    arrived.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
  }

  void wait_all_arrived() {
    while (arrived.load(std::memory_order_acquire) < total) cpu_relax();
  }

  void release() {
    go.store(true, std::memory_order_release);
  }
};

// format a double with comma as decimal separator for CSV output
inline std::string fmt_double(double v) {
  std::string s = std::to_string(v);
  for (auto& c : s) if (c == '.') c = ',';
  return s;
}

// do 'iters' iterations of dummy work to simulate a critical section
// compiler barrier prevents the loop from being optimized away
inline void busy_work(std::uint64_t iters) {
  for (std::uint64_t i = 0; i < iters; ++i) {
    asm volatile("" ::: "memory");
  }
}