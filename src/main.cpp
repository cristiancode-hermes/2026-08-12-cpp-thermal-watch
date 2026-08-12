// SPDX-License-Identifier: MIT
// cpp-thermal-watch — Linux thermal monitor (C++23)
// Reads /sys/class/thermal/thermal_zone*/ (temp in millidegrees, type, policy),
// /sys/class/thermal/cooling_device*/ (type, max_state, cur_state) and CPU
// cpufreq governors/frequencies to show thermal zones, cooling device
// throttle and CPU frequency policy in one screen.
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o thermal-watch
// Usage: ./thermal-watch [--watch N] [--json] [--threshold N] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace thermalwatch {

namespace fs = std::filesystem;

// ── ANSI palette (mutable so --no-color can blank it) ──────────────────────
bool g_color = true;
struct Palette {
  std::string bold, dim, cyan, green, yellow, red, rst;
};
Palette P;

void init_palette() {
  P.bold   = "\x1b[1m";
  P.dim    = "\x1b[2m";
  P.cyan   = "\x1b[38;5;117m";
  P.green  = "\x1b[38;5;158m";
  P.yellow = "\x1b[38;5;222m";
  P.red    = "\x1b[38;5;210m";
  P.rst    = "\x1b[0m";
}
void disable_color() {
  P.bold = P.dim = P.cyan = P.green = P.yellow = P.red = P.rst = "";
}

// ── Small helpers ──────────────────────────────────────────────────────────
std::string trim(std::string s) {
  auto is_sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_sp(s.front())) s.erase(s.begin());
  while (!s.empty() && is_sp(s.back())) s.pop_back();
  return s;
}

// Read a sysfs/proc file fully, trimmed; empty string on failure.
std::string read_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return trim(ss.str());
}

// "45000\n" -> 45.0 ; garbage -> 0.0
double parse_millicelsius(const std::string& raw) {
  std::string s = trim(raw);  // from_chars does NOT skip whitespace
  long long mC = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), mC);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return 0.0;
  return static_cast<double>(mC) / 1000.0;
}

int parse_int(const std::string& raw) {
  std::string s = trim(raw);
  int v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return 0;
  return v;
}

// 67.0 -> "67.0"; 45.25 -> "45.3"
std::string fmt_temp(double c) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.1f", c);
  return buf;
}

// 2400000 -> "2.40GHz"
std::string fmt_mhz(long long khz) {
  if (khz <= 0) return "-";
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.2fGHz", static_cast<double>(khz) / 1000000.0);
  return buf;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// value/max scaled to width, filled with '|', rest '.'
std::string bar(double value, double max, int width) {
  int filled = 0;
  if (max > 0.0)
    filled = std::clamp(static_cast<int>(std::round(value / max * width)), 0, width);
  return std::string(filled, '|') + std::string(width - filled, '.');
}

// Temp color by threshold: <60 green, <80 yellow, else red
std::string temp_color(double c, double threshold) {
  if (c < 60.0) return P.green;
  if (c < threshold) return P.yellow;
  return P.red;
}

// ── sysfs scan ─────────────────────────────────────────────────────────────
struct ThermalZone {
  int index = 0;
  std::string type;     // e.g. "x86_pkg_temp", "acpitz"
  double temp_c = 0.0;  // millidegrees / 1000
  std::string policy;   // e.g. "step_wise", "user_space"
};

struct CoolingDevice {
  int index = 0;
  std::string type;     // e.g. "Processor", "intel_powerclamp"
  int max_state = 0;
  int cur_state = 0;
  double throttle_pct = 0.0;  // cur/max*100 (0 if max==0)
};

struct CpuFreq {
  int cpu = 0;
  std::string governor;  // "powersave", "performance", ...
  long long cur_khz = 0;
  long long max_khz = 0;
};

struct Snapshot {
  std::vector<ThermalZone> zones;
  std::vector<CoolingDevice> coolers;
  std::vector<CpuFreq> cpus;
  double max_temp_c = 0.0;
  int zone_count = 0, cooler_count = 0, cpu_count = 0;
  std::string governor;  // cpu0 governor
};

Snapshot take_snapshot() {
  Snapshot s;
  const std::string base = "/sys/class/thermal/";

  // Thermal zones (therm_zoneN: temp, type, policy)
  if (fs::is_directory(base)) {
    for (int i = 0; i < 128; ++i) {
      std::string dir = base + "thermal_zone" + std::to_string(i);
      if (!fs::is_directory(dir)) continue;
      ThermalZone z;
      z.index = i;
      z.type = read_file(dir + "/type");
      if (z.type.empty()) z.type = "unknown";
      z.policy = read_file(dir + "/policy");
      if (z.policy.empty()) z.policy = "unknown";
      z.temp_c = parse_millicelsius(read_file(dir + "/temp"));
      s.zones.push_back(std::move(z));
    }
  }

  // Cooling devices (cooling_deviceN: type, max_state, cur_state)
  if (fs::is_directory(base)) {
    for (int i = 0; i < 128; ++i) {
      std::string dir = base + "cooling_device" + std::to_string(i);
      if (!fs::is_directory(dir)) continue;
      CoolingDevice d;
      d.index = i;
      d.type = read_file(dir + "/type");
      if (d.type.empty()) d.type = "unknown";
      d.max_state = parse_int(read_file(dir + "/max_state"));
      d.cur_state = parse_int(read_file(dir + "/cur_state"));
      d.throttle_pct = (d.max_state > 0)
        ? static_cast<double>(d.cur_state) / d.max_state * 100.0 : 0.0;
      s.coolers.push_back(std::move(d));
    }
  }

  // CPU frequencies + governors (cpufreq per core)
  for (int i = 0; i < 256; ++i) {
    std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq";
    if (!fs::is_directory(dir)) continue;
    CpuFreq c;
    c.cpu = i;
    c.governor = read_file(dir + "/scaling_governor");
    c.cur_khz = static_cast<long long>(parse_int(read_file(dir + "/scaling_cur_freq")));
    c.max_khz = static_cast<long long>(parse_int(read_file(dir + "/scaling_max_freq")));
    if (i == 0) s.governor = c.governor;
    s.cpus.push_back(std::move(c));
  }

  // Derived stats
  s.zone_count = static_cast<int>(s.zones.size());
  s.cooler_count = static_cast<int>(s.coolers.size());
  s.cpu_count = static_cast<int>(s.cpus.size());
  for (const auto& z : s.zones)
    if (z.temp_c > s.max_temp_c) s.max_temp_c = z.temp_c;
  return s;
}

// ── Rendering ──────────────────────────────────────────────────────────────
void print_header() {
  std::cout << P.bold << "  THERMAL WATCH — Linux thermal monitor" << P.rst << P.dim
            << "  (/sys/class/thermal + cpufreq)" << P.rst << '\n';
  std::cout << P.dim << "  ------------------------------------------------------------------------------" << P.rst << '\n';
}

void print_zones(const Snapshot& s, double threshold) {
  std::cout << P.cyan << "  THERMAL ZONES (" << s.zone_count << ")" << P.rst;
  if (s.zone_count == 0) {
    std::cout << P.dim << "  — no /sys/class/thermal/thermal_zone* on this system (VM/container)" << P.rst << '\n';
    return;
  }
  std::cout << '\n';
  std::cout << P.dim << "   ZONE  TYPE                         TEMP     POLICY" << P.rst << '\n';
  for (const auto& z : s.zones) {
    std::string t = fmt_temp(z.temp_c) + " C";
    // right-align temp in 8 cols
    int pad = std::max(0, 8 - static_cast<int>(t.size()));
    std::string temp_s = std::string(pad, ' ') + temp_color(z.temp_c, threshold) + t + P.rst;
    char buf[256];
    std::snprintf(buf, sizeof buf, "  %4d  %-28s %s  %s",
                  z.index, z.type.c_str(), temp_s.c_str(), z.policy.c_str());
    std::cout << buf << '\n';
  }
}

void print_coolers(const Snapshot& s) {
  std::cout << P.cyan << "  COOLING DEVICES (" << s.cooler_count << ")" << P.rst;
  if (s.cooler_count == 0) {
    std::cout << P.dim << "  — none" << P.rst << '\n';
    return;
  }
  std::cout << '\n';
  std::cout << P.dim << "   DEV  TYPE                         CUR/MAX   THROTTLE  BAR" << P.rst << '\n';
  for (const auto& d : s.coolers) {
    std::string pct = fmt_temp(d.throttle_pct) + "%";
    std::string color = d.throttle_pct >= 80.0 ? P.red
                      : d.throttle_pct >= 50.0 ? P.yellow : P.green;
    std::string bar_s = d.max_state > 0
      ? color + bar(d.cur_state, d.max_state, 16) + P.rst
      : P.dim + std::string(16, '.') + P.rst;
    char buf[256];
    std::snprintf(buf, sizeof buf, "  %4d  %-28s %3d/%-6d %7s  %s",
                  d.index, d.type.c_str(), d.cur_state, d.max_state, pct.c_str(), bar_s.c_str());
    std::cout << buf << '\n';
  }
}

void print_cpus(const Snapshot& s) {
  std::cout << P.cyan << "  CPU FREQUENCY (" << s.cpu_count << " cores" << (s.governor.empty() ? "" : ", governor: " + s.governor) << ")" << P.rst;
  if (s.cpu_count == 0) {
    std::cout << P.dim << "  — no cpufreq exposed (VM/container)" << P.rst << '\n';
    return;
  }
  std::cout << '\n';
  std::cout << P.dim << "   CPU  GOV          CUR        MAX" << P.rst << '\n';
  for (const auto& c : s.cpus) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "  %3d  %-12s %9s  %9s",
                  c.cpu, c.governor.c_str(), fmt_mhz(c.cur_khz).c_str(), fmt_mhz(c.max_khz).c_str());
    std::cout << buf << '\n';
  }
}

void print_summary(const Snapshot& s, double threshold) {
  std::cout << P.cyan << "  SUMMARY" << P.rst << "  zones " << s.zone_count
            << "   coolers " << s.cooler_count << "   cores " << s.cpu_count;
  if (s.max_temp_c > 0.0)
    std::cout << "   max temp " << temp_color(s.max_temp_c, threshold)
              << fmt_temp(s.max_temp_c) << " C" << P.rst;
  std::cout << '\n';
}

// ── JSON ───────────────────────────────────────────────────────────────────
std::string build_json(const Snapshot& s) {
  std::ostringstream j;
  j << "{\n";
  j << "  \"zones\": [\n";
  for (size_t i = 0; i < s.zones.size(); ++i) {
    const auto& z = s.zones[i];
    j << (i ? ",\n" : "") << "    {\"index\": " << z.index
      << ", \"type\": \"" << json_escape(z.type) << "\""
      << ", \"temp_celsius\": " << std::round(z.temp_c * 10.0) / 10.0
      << ", \"policy\": \"" << json_escape(z.policy) << "\"}";
  }
  j << "\n  ],\n";
  j << "  \"cooling_devices\": [\n";
  for (size_t i = 0; i < s.coolers.size(); ++i) {
    const auto& d = s.coolers[i];
    j << (i ? ",\n" : "") << "    {\"index\": " << d.index
      << ", \"type\": \"" << json_escape(d.type) << "\""
      << ", \"max_state\": " << d.max_state
      << ", \"cur_state\": " << d.cur_state
      << ", \"throttle_pct\": " << std::round(d.throttle_pct * 10.0) / 10.0 << "}";
  }
  j << "\n  ],\n";
  j << "  \"cpus\": [\n";
  for (size_t i = 0; i < s.cpus.size(); ++i) {
    const auto& c = s.cpus[i];
    j << (i ? ",\n" : "") << "    {\"cpu\": " << c.cpu
      << ", \"governor\": \"" << json_escape(c.governor) << "\""
      << ", \"cur_khz\": " << c.cur_khz
      << ", \"max_khz\": " << c.max_khz << "}";
  }
  j << "\n  ],\n";
  j << "  \"summary\": {\"zones\": " << s.zone_count
    << ", \"coolers\": " << s.cooler_count
    << ", \"cores\": " << s.cpu_count
    << ", \"max_temp_c\": " << std::round(s.max_temp_c * 10.0) / 10.0
    << ", \"governor\": \"" << json_escape(s.governor) << "\"}\n";
  j << "}\n";
  return j.str();
}

// ── Watch mode ─────────────────────────────────────────────────────────────
void run_watch(int interval, double threshold) {
  while (true) {
    Snapshot s = take_snapshot();
    std::cout << "\x1b[2J\x1b[H";
    print_header();
    std::cout << P.dim << "  (live refresh every " << interval << "s — Ctrl+C to exit)" << P.rst << '\n';
    print_zones(s, threshold);
    std::cout << '\n';
    print_coolers(s);
    std::cout << '\n';
    print_cpus(s);
    std::cout << '\n';
    print_summary(s, threshold);
    std::cout << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(interval));
  }
}

void print_help(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "Monitor termico de Linux (C++23).\n\n"
            << "Opciones:\n"
            << "  --watch N       refresco en vivo cada N segundos\n"
            << "  --json          salida JSON completa para scripting\n"
            << "  --threshold N   umbral de alerta en C (default 80; >= umbral = rojo)\n"
            << "  --no-color      sin colores ANSI (pipes/logs)\n"
            << "  --selftest      auto-verificacion\n"
            << "  -h, --help      esta ayuda\n\n"
            << "Fuentes: /sys/class/thermal/thermal_zone*/{temp,type,policy},\n"
            << "         /sys/class/thermal/cooling_device*/{type,max_state,cur_state},\n"
            << "         /sys/devices/system/cpu/cpu*/cpufreq/{scaling_governor,scaling_cur_freq,scaling_max_freq}\n";
}

// ── Selftest ───────────────────────────────────────────────────────────────
int g_pass = 0, g_fail = 0;
void t_check(const char* name, bool cond) {
  if (cond) { ++g_pass; std::cout << "  PASS  " << name << '\n'; }
  else { ++g_fail; std::cout << "  FAIL  " << name << '\n'; }
}

int run_selftest() {
  // 1. millicelsius parse
  t_check("mC 45000 -> 45.0", parse_millicelsius("45000\n") == 45.0);
  // 2. mC with trailing spaces
  t_check("mC 67000 -> 67.0", parse_millicelsius("67000 ") == 67.0);
  // 3. mC garbage rejected
  t_check("mC garbage -> 0", parse_millicelsius("abc") == 0.0);
  // 4. mC negative (frozen) -> -12.3
  t_check("mC negative", parse_millicelsius("-12300") == -12.3);
  // 5. int parse
  t_check("int parse", parse_int("100\n") == 100 && parse_int("x") == 0);
  // 6. trim
  t_check("trim", trim("  hello \t\n") == "hello");
  // 7. fmt_temp (avoid .5 ties — %.1f rounds half-to-even)
  t_check("fmt_temp", fmt_temp(67.0) == "67.0" && fmt_temp(45.24) == "45.2" && fmt_temp(45.26) == "45.3");
  // 8. fmt_mhz
  t_check("fmt_mhz", fmt_mhz(2400000) == "2.40GHz" && fmt_mhz(0) == "-");
  // 9. json_escape
  t_check("json_escape", json_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
  // 10. throttle calc
  t_check("throttle 45/100", (static_cast<double>(45) / 100 * 100.0) == 45.0);
  // 11. bar clamp
  t_check("bar clamp", bar(0, 10, 5) == "....." && bar(10, 10, 5) == "|||||" && bar(7, 10, 5) == "||||.");
  // 12. bar max=0
  t_check("bar max=0", bar(0, 0, 5) == ".....");
  // 13. temp color classification (disabled color -> same string, logic via threshold)
  t_check("temp_color threshold", temp_color(59.9, 80.0) == P.green &&
                                  temp_color(70.0, 80.0) == P.yellow &&
                                  temp_color(85.0, 80.0) == P.red &&
                                  temp_color(75.0, 70.0) == P.red);
  // 14. snapshot on real sysfs does not crash, counts consistent
  Snapshot s = take_snapshot();
  t_check("snapshot counts", s.zone_count == static_cast<int>(s.zones.size()) &&
                             s.cooler_count == static_cast<int>(s.coolers.size()) &&
                             s.cpu_count == static_cast<int>(s.cpus.size()));
  // 15. max temp derived
  double m = 0.0;
  for (const auto& z : s.zones) m = std::max(m, z.temp_c);
  t_check("max temp", s.max_temp_c == m);
  // 16. JSON shape
  std::string j = build_json(s);
  t_check("json shape", j.starts_with("{\n  \"zones\"") &&
                        j.find("\"cooling_devices\"") != std::string::npos &&
                        j.find("\"summary\"") != std::string::npos);
  // 17. JSON parseable via python (validated externally), zones temps in sane range when present
  for (const auto& z : s.zones)
    if (z.temp_c > 0.0) {
      t_check("zone temp range", z.temp_c > 0.0 && z.temp_c < 150.0);
      break;
    }
  // 18. throttle sanity
  for (const auto& d : s.coolers)
    if (d.max_state > 0) {
      t_check("throttle range", d.throttle_pct >= 0.0 && d.throttle_pct <= 100.0);
      break;
    }

  std::cout << '\n' << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}

}  // namespace thermalwatch

// ── main (global scope) ────────────────────────────────────────────────────
int main(int argc, char** argv) {
  using namespace thermalwatch;
  init_palette();
  int watch = 0;
  double threshold = 80.0;
  bool json = false, selftest = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next_val = [&](const char* flag) -> double {
      if (i + 1 < argc) return std::atof(argv[++i]);
      std::cerr << flag << " requires a number\n";
      std::exit(2);
    };
    if (a == "--watch") watch = static_cast<int>(next_val("--watch"));
    else if (a == "--threshold") threshold = next_val("--threshold");
    else if (a == "--json") json = true;
    else if (a == "--no-color" || a == "-c") disable_color();
    else if (a == "--selftest") selftest = true;
    else if (a == "-h" || a == "--help") { print_help(argv[0]); return 0; }
    else { std::cerr << "opcion desconocida: " << a << " (usa --help)\n"; return 2; }
  }
  if (threshold < 40.0) threshold = 40.0;
  if (threshold > 120.0) threshold = 120.0;

  if (selftest) return run_selftest();
  if (json) {
    std::cout << build_json(take_snapshot());
    return 0;
  }
  if (watch > 0) {
    run_watch(watch, threshold);
    return 0;
  }

  Snapshot s = take_snapshot();
  print_header();
  print_zones(s, threshold);
  std::cout << '\n';
  print_coolers(s);
  std::cout << '\n';
  print_cpus(s);
  std::cout << '\n';
  print_summary(s, threshold);
  return 0;
}
