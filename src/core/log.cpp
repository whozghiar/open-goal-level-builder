#include "core/log.h"

#include <cstdio>
#include <vector>

namespace ogle {

Log& Log::get() {
  static Log log;
  return log;
}

void Log::add(LogLevel level, const std::string& text) {
  const char* prefix = level == LogLevel::Error ? "[error] " : level == LogLevel::Warn ? "[warn] " : "";
  FILE* out = to_stderr ? stderr : stdout;
  std::fprintf(out, "%s%s\n", prefix, text.c_str());
  std::fflush(out);
  std::lock_guard<std::mutex> lock(m_mutex);
  m_lines.push_back({level, text});
  while (m_lines.size() > 2000) m_lines.pop_front();
}

std::deque<LogLine> Log::lines() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_lines;
}

std::string Log::last() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_lines.empty() ? std::string() : m_lines.back().text;
}

std::string strf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::vector<char> buf(n > 0 ? n + 1 : 1);
  std::vsnprintf(buf.data(), buf.size(), fmt, args);
  va_end(args);
  return std::string(buf.data());
}

}  // namespace ogle
