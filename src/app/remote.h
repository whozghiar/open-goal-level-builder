#pragma once

// Remote control of the editor, used by its MCP server (open-goal-level-editor --mcp, see mcp.cpp):
// the editor listens on a local port (127.0.0.1 only) for requests, one JSON object per line,
//
//   {"id": 1, "tool": "open_level", "args": {"name": "ruins"}}
//
// and answers each on its connection, one line too:
//
//   {"id": 1, "ok": true, "result": {...}}
//   {"id": 1, "ok": true, "result": {...}, "image": "<PNG in base64>"}   (a capture)
//   {"id": 1, "ok": false, "error": "..."}
//
// The editor carries the requests out on its main thread between frames, like the user's actions:
// they are shown as they happen and can be undone.

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"

namespace ogle {

using json = nlohmann::json;

constexpr int kRemotePort = 47821;

struct RemoteRequest {
  int client = 0;
  json id;
  std::string tool;
  json args;
};

class RemoteServer {
 public:
  ~RemoteServer() { stop(); }
  bool start(int port, std::string* error);  // a thread accepts and reads the connections
  void stop();
  bool running() const { return m_thread.joinable(); }
  int port() const { return m_port; }
  size_t clients() const { return m_client_count; }
  std::vector<RemoteRequest> take();                // the requests received since the last call
  void reply(int client, const json& message);      // one line on that connection

 private:
  void loop();

  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<size_t> m_client_count{0};
  int m_port = 0;
  intptr_t m_listen = -1;
  std::mutex m_mutex;
  std::map<int, intptr_t> m_sockets;  // client id -> socket
  std::vector<RemoteRequest> m_requests;
  int m_next_client = 1;
};

// The other end: the MCP server sends a request and waits for its answer.
class RemoteClient {
 public:
  ~RemoteClient() { close(); }
  bool connect(int port, std::string* error);
  bool connected() const { return m_socket != -1; }
  // sends `request` (one line), reads the answer line; false on a broken connection or timeout
  bool call(const json& request, json* answer, int timeout_ms, std::string* error);
  void close();

 private:
  intptr_t m_socket = -1;
  std::string m_pending;  // bytes read past the last answer
};

// Base64 of bytes (images of the MCP answers).
std::string base64_encode(const uint8_t* data, size_t size);

}  // namespace ogle
