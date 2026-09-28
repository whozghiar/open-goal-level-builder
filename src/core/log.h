#pragma once

// Minimal logger: prints to stdout and keeps the last lines for the in-editor log window.

#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>

namespace ogle {

enum class LogLevel { Info, Warn, Error };

struct LogLine {
  LogLevel level;
  std::string text;
};

class Log {
 public:
  static Log& get();
  static inline bool to_stderr = false;  // the MCP server keeps stdout for its protocol
  void add(LogLevel level, const std::string& text);
  std::deque<LogLine> lines();
  // the last message, shown in the status bar
  std::string last();

 private:
  std::mutex m_mutex;
  std::deque<LogLine> m_lines;
};

std::string strf(const char* fmt, ...);

#define LOG_INFO(...) ::ogle::Log::get().add(::ogle::LogLevel::Info, ::ogle::strf(__VA_ARGS__))
#define LOG_WARN(...) ::ogle::Log::get().add(::ogle::LogLevel::Warn, ::ogle::strf(__VA_ARGS__))
#define LOG_ERROR(...) ::ogle::Log::get().add(::ogle::LogLevel::Error, ::ogle::strf(__VA_ARGS__))

}  // namespace ogle
