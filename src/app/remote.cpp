#include "app/remote.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ogle {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kNoSocket = INVALID_SOCKET;
void close_socket(sock_t s) { closesocket(s); }
bool net_init() {
  static bool done = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  return done;
}
#else
using sock_t = int;
constexpr sock_t kNoSocket = -1;
void close_socket(sock_t s) { ::close(s); }
bool net_init() { return true; }
#endif

sock_t to_sock(intptr_t s) { return (sock_t)s; }

bool send_all(sock_t s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    int n = (int)::send(s, data.data() + sent, (int)(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return true;
}

// waits until `s` can be read, at most `ms`
bool readable(sock_t s, int ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  return ::select((int)s + 1, &set, nullptr, nullptr, &tv) > 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// server (the editor)
// ------------------------------------------------------------------------------------------------

bool RemoteServer::start(int port, std::string* error) {
  stop();
  if (!net_init()) {
    if (error) *error = "network unavailable";
    return false;
  }
  sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) {
    if (error) *error = "no socket";
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this computer only
#ifdef _WIN32
  // another program cannot take the port while the editor has it
  BOOL exclusive = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
#else
  int reuse = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(s, 4) != 0) {
    close_socket(s);
    if (error) *error = "port " + std::to_string(port) + " already in use (another editor?)";
    return false;
  }
  m_listen = (intptr_t)s;
  m_port = port;
  m_stop = false;
  m_thread = std::thread([this] { loop(); });
  return true;
}

void RemoteServer::stop() {
  if (!m_thread.joinable()) return;
  m_stop = true;
  m_thread.join();
  close_socket(to_sock(m_listen));
  m_listen = -1;
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto& [id, s] : m_sockets) close_socket(to_sock(s));
  m_sockets.clear();
  m_client_count = 0;
}

void RemoteServer::loop() {
  std::map<int, std::string> pending;  // client -> bytes of an unfinished line
  while (!m_stop) {
    fd_set set;
    FD_ZERO(&set);
    sock_t top = to_sock(m_listen);
    FD_SET(top, &set);
    std::vector<std::pair<int, sock_t>> clients;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (auto& [id, s] : m_sockets) {
        clients.push_back({id, to_sock(s)});
        FD_SET(to_sock(s), &set);
        if (to_sock(s) > top) top = to_sock(s);
      }
    }
    timeval tv{0, 100 * 1000};
    if (::select((int)top + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
    if (FD_ISSET(to_sock(m_listen), &set)) {
      sock_t c = ::accept(to_sock(m_listen), nullptr, nullptr);
      if (c != kNoSocket) {
        int nodelay = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sockets[m_next_client++] = (intptr_t)c;
        m_client_count = m_sockets.size();
      }
    }
    for (auto [id, s] : clients) {
      if (!FD_ISSET(s, &set)) continue;
      char buf[65536];
      int n = (int)::recv(s, buf, sizeof(buf), 0);
      if (n <= 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        close_socket(s);
        m_sockets.erase(id);
        m_client_count = m_sockets.size();
        pending.erase(id);
        continue;
      }
      std::string& text = pending[id];
      text.append(buf, (size_t)n);
      for (size_t eol; (eol = text.find('\n')) != std::string::npos;) {
        const std::string line = text.substr(0, eol);
        text.erase(0, eol + 1);
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        RemoteRequest r;
        r.client = id;
        const json j = json::parse(line, nullptr, false);
        if (!j.is_object() || !j.contains("tool") || !j["tool"].is_string()) {
          json answer = {{"id", j.is_object() && j.contains("id") ? j["id"] : json()},
                         {"ok", false},
                         {"error", "not a request: {\"id\": ..., \"tool\": \"...\", \"args\": {...}}"}};
          send_all(s, answer.dump() + "\n");
          continue;
        }
        r.id = j.value("id", json());
        r.tool = j["tool"].get<std::string>();
        r.args = j.contains("args") && j["args"].is_object() ? j["args"] : json::object();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(std::move(r));
      }
    }
  }
}

std::vector<RemoteRequest> RemoteServer::take() {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<RemoteRequest> out;
  out.swap(m_requests);
  return out;
}

void RemoteServer::reply(int client, const json& message) {
  sock_t s = kNoSocket;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sockets.find(client);
    if (it == m_sockets.end()) return;  // gone
    s = to_sock(it->second);
  }
  send_all(s, message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
}

// ------------------------------------------------------------------------------------------------
// client (the MCP server)
// ------------------------------------------------------------------------------------------------

bool RemoteClient::connect(int port, std::string* error) {
  close();
  if (!net_init()) {
    if (error) *error = "network unavailable";
    return false;
  }
  sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) {
    if (error) *error = "no socket";
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
    close_socket(s);
    if (error) *error = "the editor is not running (nothing on port " + std::to_string(port) + ")";
    return false;
  }
  int nodelay = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
  m_socket = (intptr_t)s;
  m_pending.clear();
  return true;
}

void RemoteClient::close() {
  if (m_socket != -1) close_socket(to_sock(m_socket));
  m_socket = -1;
  m_pending.clear();
}

bool RemoteClient::call(const json& request, json* answer, int timeout_ms, std::string* error) {
  if (m_socket == -1) {
    if (error) *error = "not connected";
    return false;
  }
  const sock_t s = to_sock(m_socket);
  if (!send_all(s, request.dump(-1, ' ', false, json::error_handler_t::replace) + "\n")) {
    close();
    if (error) *error = "the connection to the editor was lost";
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    const size_t eol = m_pending.find('\n');
    if (eol != std::string::npos) {
      const std::string line = m_pending.substr(0, eol);
      m_pending.erase(0, eol + 1);
      const json j = json::parse(line, nullptr, false);
      if (!j.is_object()) continue;
      if (j.contains("id") && request.contains("id") && j["id"] != request["id"]) continue;  // a late answer
      *answer = j;
      return true;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      if (error) *error = "the editor did not answer in time";
      return false;
    }
    if (!readable(s, (int)std::min<long long>(left.count(), 200))) continue;
    char buf[65536];
    int n = (int)::recv(s, buf, sizeof(buf), 0);
    if (n <= 0) {
      close();
      if (error) *error = "the editor closed the connection";
      return false;
    }
    m_pending.append(buf, (size_t)n);
  }
}

std::string base64_encode(const uint8_t* data, size_t size) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b = (uint32_t)data[i] << 16 | (i + 1 < size ? (uint32_t)data[i + 1] << 8 : 0) |
                       (i + 2 < size ? (uint32_t)data[i + 2] : 0);
    out.push_back(table[(b >> 18) & 63]);
    out.push_back(table[(b >> 12) & 63]);
    out.push_back(i + 1 < size ? table[(b >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? table[b & 63] : '=');
  }
  return out;
}

}  // namespace ogle
