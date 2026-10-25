#include "transport/relay_transport.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include <ssl/uvcpp_ssl_context.h>
#include <webapp/uvcpp_web_ws_client.h>

namespace molink {

relay_transport::relay_transport(std::string url, std::string ca_path,
                                 post_fn post)
    : url_(std::move(url)), ca_path_(std::move(ca_path)), post_(std::move(post)) {}

relay_transport::~relay_transport() { close(transport_close::normal, "gone"); }

void relay_transport::set_handlers(text_handler on_text,
                                   binary_handler on_binary,
                                   close_handler on_close) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
}

void relay_transport::set_ready_handler(std::function<void()> ready) {
  std::function<void()> notify;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    ready_handler_ = std::move(ready);
    if (ready_) notify = ready_handler_;
  }
  if (notify) {
    if (post_) {
      post_(std::move(notify));
    } else {
      notify();
    }
  }
}

void relay_transport::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { thread_main(); });
}

bool relay_transport::is_open() const {
  return ready_.load() && !closed_.load();
}

bool relay_transport::send_text(const std::string& text) {
  return enqueue(true, text);
}

bool relay_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (data == nullptr && len > 0) return false;
  std::string copy(reinterpret_cast<const char*>(data), len);
  return enqueue(false, std::move(copy));
}

bool relay_transport::enqueue(bool text, std::string data) {
  if (!ready_.load() || closed_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    sends_.emplace_back(text, std::move(data));
  }
  return true;
}

void relay_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_.exchange(true)) return;
  close_handler handler;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = close_;
  }
  if (!handler) return;
  if (post_) {
    post_([handler, code, reason] { handler(code, reason); });
  } else {
    handler(code, reason);
  }
}

void relay_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  closed_ = true;
  running_ = false;
  if (thread_.joinable()) {
    if (std::this_thread::get_id() == thread_.get_id()) return;
    thread_.join();
  }
}

void relay_transport::thread_main() {
  uvcpp::uvcpp_ssl_context tls(uvcpp::tls_mode::CLIENT);
  uvcpp::uvcpp_web_ws_client client;

  std::fprintf(stderr, "relay: connecting\n");
  std::fflush(stderr);
  if (url_.rfind("wss://", 0) == 0) {
    tls.set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
    if (!ca_path_.empty() && !tls.load_ca_file(ca_path_)) {
      std::fprintf(stderr, "relay: cannot load CA %s\n", ca_path_.c_str());
      std::fflush(stderr);
      closed_ = true;
      deliver_close(transport_close::policy, "cannot load the relay CA");
      running_ = false;
      return;
    }
    client.set_ssl_context(&tls);
  }

  client.on_text([this](const std::string& text) {
    if (text.find("\"relay-ready\"") != std::string::npos) {
      ready_ = true;
      std::fprintf(stderr, "relay: ready\n");
      std::fflush(stderr);
      std::function<void()> handler;
      {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = ready_handler_;
      }
      if (handler) {
        if (post_) {
          post_(std::move(handler));
        } else {
          handler();
        }
      }
      return;
    }
    if (text.find("\"relay-closed\"") != std::string::npos) {
      closed_ = true;
      ready_ = false;
      deliver_close(transport_close::normal, "relay closed");
      stop_requested_ = true;
      return;
    }
    text_handler handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = text_;
    }
    if (handler) {
      if (post_) {
        post_([handler, text] { handler(text); });
      } else {
        handler(text);
      }
    }
  });

  client.on_binary([this](const uint8_t* data, std::size_t len) {
    std::string copy(reinterpret_cast<const char*>(data), len);
    binary_handler handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = binary_;
    }
    if (handler) {
      if (post_) {
        post_([handler, copy] {
          handler(reinterpret_cast<const uint8_t*>(copy.data()), copy.size());
        });
      } else {
        handler(reinterpret_cast<const uint8_t*>(copy.data()), copy.size());
      }
    }
  });

  client.on_close([this](uvcpp::ws_close_code, const std::string& reason) {
    std::fprintf(stderr, "relay: closed %s\n", reason.c_str());
    std::fflush(stderr);
    closed_ = true;
    ready_ = false;
    deliver_close(transport_close::normal, reason);
    stop_requested_ = true;
  });

  const int connect_rc = client.connect(url_);
  if (connect_rc != 0) {
    std::fprintf(stderr, "relay: connect rc=%d\n", connect_rc);
    std::fflush(stderr);
    closed_ = true;
    deliver_close(transport_close::policy, "cannot reach the relay");
    running_ = false;
    return;
  }

  while (running_) {
    client.run(UV_RUN_NOWAIT);
    std::deque<std::pair<bool, std::string>> sends;
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      sends.swap(sends_);
    }
    for (const auto& message : sends) {
      if (client.session() == nullptr || closed_) break;
      if (message.first) {
        client.send_text(message.second);
      } else {
        client.send_binary(message.second.data(), message.second.size());
      }
    }
    if (stop_requested_ || !running_) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  client.close();
}

}  // namespace molink
