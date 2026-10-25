#include "p2p/p2p_manager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <utility>

#include "settings.h"
#include "util.h"

namespace molink {
namespace {

constexpr uint64_t kConnectTimeoutMs = 20000;
constexpr int kPollIntervalMs = 500;
// Pending signalling is polled while the core is registered. The directory
// allows 8 requests/second per device, so 500 ms (2/s) is well inside the
// budget and makes an incoming connect visible almost immediately.
constexpr uint64_t kPendingIntervalMs = 500;

void plog(const std::string& message) {
  std::fprintf(stderr, "p2p: %s\n", message.c_str());
  std::fflush(stderr);
  const char* path = std::getenv("MOLINK_P2P_LOG");
  if (path == nullptr || *path == '\0') return;
  std::ofstream out(path, std::ios::app);
  if (out) out << message << "\n";
}

}  // namespace

p2p_manager::p2p_manager(uvcpp::uvcpp_loop* loop, post_fn post,
                         std::shared_ptr<server_client> server,
                         std::shared_ptr<session_registry> registry,
                         session_options options)
    : loop_(loop),
      post_(std::move(post)),
      server_(std::move(server)),
      registry_(std::move(registry)),
      options_(std::move(options)) {
  // Cross-NAT connections need a server-reflexive candidate; a client that
  // only gathers host candidates cannot be reached through two NATs. Default
  // to a public STUN server so remote installs work out of the box;
  // MOLINK_STUN_HOST=none disables it, and any other value overrides it.
  const char* stun = std::getenv("MOLINK_STUN_HOST");
  if (stun != nullptr && *stun != '\0') {
    stun_host_ = stun;
  } else {
    stun_host_ = "stun.miwifi.com";
  }
  if (stun_host_ == "none" || stun_host_ == "off" || stun_host_ == "0") {
    stun_host_.clear();
  }
  const char* stun_port = std::getenv("MOLINK_STUN_PORT");
  if (stun_port != nullptr) {
    const int parsed = std::atoi(stun_port);
    if (parsed > 0 && parsed <= 65535) {
      stun_port_ = static_cast<uint16_t>(parsed);
    }
  }
}

p2p_manager::~p2p_manager() { stop(); }

void p2p_manager::start() {
  if (running_.exchange(true)) return;
  plog("signaling worker starting");
  worker_ = std::thread([this] { worker_loop(); });
}

void p2p_manager::stop() {
  if (!running_.exchange(false)) {
    if (worker_.joinable()) worker_.join();
    return;
  }
  task_cv_.notify_all();
  if (worker_.joinable()) worker_.join();

  // Close the ICE sockets on the loop thread while it is still running.
  if (post_) {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    post_([this, done] {
      outgoing_.clear();
      incoming_.clear();
      done->set_value();
    });
    future.wait_for(std::chrono::seconds(2));
  }
}

void p2p_manager::enqueue(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    tasks_.push_back(std::move(task));
  }
  task_cv_.notify_one();
}

void p2p_manager::worker_loop() {
  plog("signaling worker entered");
  std::string last_pending_error;
  uint64_t last_pending_ms = 0;
  while (running_.load()) {
    std::unique_lock<std::mutex> lock(task_mutex_);
    task_cv_.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs), [this] {
      return !tasks_.empty() || !running_.load();
    });
    while (!tasks_.empty()) {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
    lock.unlock();
    if (!running_.load()) break;

    const uint64_t now = now_ms();
    std::vector<signal_session_info> pending;
    std::string error;
    const bool poll_pending =
        last_pending_ms == 0 || now - last_pending_ms >= kPendingIntervalMs;
    if (poll_pending) last_pending_ms = now;
    if (poll_pending && server_->signal_pending(pending, error)) {
      if (!pending.empty()) {
        plog("pending sessions: " + std::to_string(pending.size()));
      }
      for (const signal_session_info& session : pending) {
        if (session.connect_id.empty()) continue;
        {
          std::lock_guard<std::mutex> seen_lock(seen_mutex_);
          if (!seen_incoming_.insert(session.connect_id).second) continue;
        }
        post_([this, session] { start_incoming(session); });
      }
    } else if (poll_pending && error != last_pending_error) {
      plog("pending poll: " + error);
      last_pending_error = error;
    }

    std::vector<outgoing_poll> poll;
    {
      std::lock_guard<std::mutex> poll_lock(outgoing_mutex_);
      for (const auto& entry : outgoing_poll_) poll.push_back(entry.second);
    }
    for (const outgoing_poll& entry : poll) {
      signal_session_info status;
      std::string status_error;
      if (!server_->signal_status(entry.connect_id, status, status_error)) {
        continue;
      }
      post_([this, id = entry.id, status] { handle_status(id, status); });
    }

    post_([this] { sweep(); });
  }
}

void p2p_manager::refresh_stun() {
  const app_settings settings = load_settings();
  std::string value = settings.stun_server;
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  value = value.substr(start);
  if (value.empty()) return;  // keep the env/default chosen at startup
  if (value == "none" || value == "off" || value == "0") {
    stun_host_.clear();
    return;
  }
  std::string host = value;
  uint16_t port = stun_port_ == 0 ? 3478 : stun_port_;
  const std::size_t colon = value.rfind(':');
  if (colon != std::string::npos && value.find(':') == colon) {
    host = value.substr(0, colon);
    const int parsed = std::atoi(value.c_str() + colon + 1);
    if (parsed > 0 && parsed <= 65535) {
      port = static_cast<uint16_t>(parsed);
    }
  }
  if (!host.empty()) {
    stun_host_ = host;
    stun_port_ = port;
  }
}

void p2p_manager::sweep() {
  const uint64_t now = now_ms();
  for (auto it = outgoing_.begin(); it != outgoing_.end();) {
    if (!it->second.finished && now > it->second.created_ms &&
        now - it->second.created_ms > kConnectTimeoutMs) {
      const uint64_t id = it->first;
      ++it;
      finish_outgoing(id, "ICE timed out");
      continue;
    }
    ++it;
  }
  for (auto it = incoming_.begin(); it != incoming_.end();) {
    if (!it->second.finished && now > it->second.created_ms &&
        now - it->second.created_ms > kConnectTimeoutMs) {
      const std::string id = it->first;
      ++it;
      finish_incoming(id, "ICE timed out");
      continue;
    }
    ++it;
  }
}

std::shared_ptr<ice_agent> p2p_manager::make_agent() {
  return std::make_shared<ice_agent>(loop_);
}

void p2p_manager::connect(const std::string& target, bool relay_only,
                          ready_fn ready, error_fn error) {
  refresh_stun();
  if (target.empty()) {
    error("target device id is empty");
    return;
  }
  const uint64_t id = next_outgoing_++;
  plog("connecting to " + target + (relay_only ? " (relay only)" : ""));
  outgoing entry;
  entry.target = target;
  entry.created_ms = now_ms();
  entry.ready = std::move(ready);
  entry.error = std::move(error);
  entry.relay_only = relay_only;
  if (!relay_only) entry.agent = make_agent();
  outgoing_.emplace(id, std::move(entry));

  if (relay_only) {
    // No ICE: create the signaling session only so both sides can obtain a
    // relay token and pair. The offer is a placeholder; the target's ICE will
    // not complete, which is fine because this side never uses it.
    enqueue([this, id, target] {
      std::string connect_id;
      std::string error;
      if (!server_->signal_create(target, "v=0", {}, connect_id, error)) {
        plog("signal_create failed: " + error);
        post_([this, id, error] { finish_outgoing(id, error); });
        return;
      }
      plog("relay-only connect id " + connect_id);
      post_([this, id, connect_id] {
        auto found = outgoing_.find(id);
        if (found == outgoing_.end() || found->second.finished) return;
        found->second.connect_id = connect_id;
      });
      std::string url;
      std::string ca;
      if (!server_->relay_create(connect_id, url, ca, error)) {
        plog("relay_create failed: " + error);
        post_([this, id, error] { finish_outgoing(id, error); });
        return;
      }
      plog("relay url " + url.substr(0, url.find('?')));
      post_([this, id, url, ca] { start_relay_outgoing(id, url, ca); });
    });
    return;
  }

  std::shared_ptr<ice_agent> agent;
  {
    auto found = outgoing_.find(id);
    if (found == outgoing_.end()) return;
    agent = found->second.agent;
  }
  agent->set_handlers(
      [this, id](const std::string& sdp) {
        auto it = outgoing_.find(id);
        if (it != outgoing_.end()) it->second.candidates.push_back(sdp);
      },
      [this, id] {
        auto it = outgoing_.find(id);
        if (it == outgoing_.end()) return;
        const std::string offer = it->second.agent->local_description();
        const std::vector<std::string> candidates = it->second.candidates;
        const std::string target = it->second.target;
        plog("gathered " + std::to_string(candidates.size()) +
             " candidate(s), signaling " + target);
        enqueue([this, id, target, offer, candidates] {
          std::string connect_id;
          std::string error;
          if (!server_->signal_create(target, offer, candidates, connect_id,
                                      error)) {
            plog("signal_create failed: " + error);
            post_([this, id, error] { finish_outgoing(id, error); });
            return;
          }
          plog("connect id " + connect_id);
          post_([this, id, connect_id] {
            auto entry = outgoing_.find(id);
            if (entry == outgoing_.end() || entry->second.finished) return;
            entry->second.connect_id = connect_id;
            std::lock_guard<std::mutex> lock(outgoing_mutex_);
            outgoing_poll_[id] = outgoing_poll{id, connect_id,
                                               entry->second.created_ms};
          });
          // Relay first: open the data path in parallel with hole punching.
          std::string url;
          std::string ca;
          if (!server_->relay_create(connect_id, url, ca, error)) {
            plog("relay unavailable: " + error);
            return;
          }
          post_([this, id, url, ca] { start_relay_outgoing(id, url, ca); });
        });
      },
      [this, id](int state) {
        if (state == JUICE_STATE_COMPLETED) {
          plog("outgoing state completed");
          deliver_outgoing(id);
        } else if (state == JUICE_STATE_FAILED) {
          plog("outgoing ICE failed");
          finish_outgoing(id, "ICE failed");
        }
      },
      nullptr);

  std::string start_error;
  if (!agent->start(stun_host_, stun_port_, start_error)) {
    finish_outgoing(id, start_error);
  }
}

void p2p_manager::start_relay_outgoing(uint64_t id, std::string url,
                                       std::string ca) {
  auto it = outgoing_.find(id);
  if (it == outgoing_.end() || it->second.finished) return;
  plog("relay start outgoing " + it->second.target);
  auto relay = std::make_shared<relay_transport>(std::move(url), std::move(ca),
                                                 post_);
  auto switcher = std::make_shared<switchable_transport>(relay);
  it->second.relay = relay;
  it->second.switcher = switcher;
  relay->set_ready_handler([this, id] { on_outgoing_relay_ready(id); });
  relay->start();
}

void p2p_manager::on_outgoing_relay_ready(uint64_t id) {
  auto it = outgoing_.find(id);
  if (it == outgoing_.end() || it->second.finished) return;
  outgoing& entry = it->second;
  if (entry.delivered || entry.ice_completed) return;
  plog("relay ready for " + entry.target + ", delivering session");
  entry.delivered = true;
  ready_fn ready = std::move(entry.ready);
  entry.ready = nullptr;
  if (ready) ready(entry.switcher);
}

void p2p_manager::handle_status(uint64_t id,
                                const signal_session_info& session) {
  auto it = outgoing_.find(id);
  if (it == outgoing_.end() || it->second.finished) return;
  outgoing& entry = it->second;
  if (!entry.agent) return;
  if (session.closed || session.expired) {
    finish_outgoing(id, "the ICE session was closed");
    return;
  }
  if (!entry.answer_applied && !session.answer.empty()) {
    plog("applying answer (" + std::to_string(session.to_candidates.size()) +
         " candidate(s))");
    entry.agent->apply_remote_offer(session.answer,
                                    session.to_candidates);
    entry.answer_applied = true;
  }
  if (entry.agent->state() == JUICE_STATE_COMPLETED) deliver_outgoing(id);
}

void p2p_manager::deliver_outgoing(uint64_t id) {
  auto it = outgoing_.find(id);
  if (it == outgoing_.end() || it->second.finished) return;
  outgoing& entry = it->second;
  if (entry.ice_completed || !entry.agent) return;
  entry.ice_completed = true;
  const auto endpoint = parse_endpoint(entry.agent->selected_remote());
  auto ice = std::make_shared<juice_transport>(entry.agent, endpoint.first,
                                               endpoint.second);

  if (entry.delivered && entry.switcher) {
    // The session is already live on the relay; move it to the direct path.
    plog("switching " + entry.target + " to p2p");
    entry.switcher->set_ice(ice);
    entry.ice = ice;
    entry.finished = true;
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex_);
      outgoing_poll_.erase(id);
    }
    outgoing_.erase(it);
    return;
  }

  // The relay is not ready yet: use ICE directly and ignore a late relay.
  entry.delivered = true;
  entry.ice = ice;
  entry.finished = true;
  ready_fn ready = std::move(entry.ready);
  {
    std::lock_guard<std::mutex> lock(outgoing_mutex_);
    outgoing_poll_.erase(id);
  }
  outgoing_.erase(it);
  if (ready) ready(ice);
}

void p2p_manager::finish_outgoing(uint64_t id, const std::string& error) {
  auto it = outgoing_.find(id);
  if (it == outgoing_.end()) return;
  outgoing& entry = it->second;
  if (entry.finished) {
    outgoing_.erase(it);
    return;
  }
  entry.finished = true;
  if (entry.delivered) {
    // The proxy already owns a live relay session; an ICE failure must not
    // tear it down.
    if (entry.agent) entry.agent->close();
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex_);
      outgoing_poll_.erase(id);
    }
    outgoing_.erase(it);
    return;
  }
  error_fn callback = std::move(entry.error);
  if (entry.agent) entry.agent->close();
  if (entry.relay) entry.relay->close(transport_close::normal, "connect failed");
  {
    std::lock_guard<std::mutex> lock(outgoing_mutex_);
    outgoing_poll_.erase(id);
  }
  outgoing_.erase(it);
  if (callback) callback(error);
}

void p2p_manager::start_incoming(const signal_session_info& session) {
  refresh_stun();
  if (incoming_.count(session.connect_id) != 0 ||
      completed_incoming_.count(session.connect_id) != 0) {
    return;
  }
  plog("incoming " + session.connect_id + " from " + session.from_name);
  incoming entry;
  entry.connect_id = session.connect_id;
  entry.created_ms = now_ms();
  auto agent = make_agent();
  entry.agent = agent;
  incoming_.emplace(session.connect_id, std::move(entry));

  // Open the relay in parallel with ICE; whichever path is ready first starts
  // the session, and the relay switches to ICE when the checks complete.
  enqueue([this, connect_id = session.connect_id] {
    std::string url;
    std::string ca;
    std::string error;
    if (!server_->relay_create(connect_id, url, ca, error)) {
      plog("incoming relay unavailable: " + error);
      return;
    }
    post_([this, connect_id, url, ca] {
      start_relay_incoming(connect_id, url, ca);
    });
  });

  agent->set_handlers(
      [this, id = session.connect_id](const std::string& sdp) {
        auto it = incoming_.find(id);
        if (it != incoming_.end()) it->second.candidates.push_back(sdp);
      },
      [this, id = session.connect_id] {
        auto it = incoming_.find(id);
        if (it == incoming_.end() || it->second.answered) return;
        it->second.answered = true;
        const std::string answer = it->second.agent->local_description();
        const std::vector<std::string> candidates = it->second.candidates;
        enqueue([this, id, answer, candidates] {
          std::string error;
          if (!server_->signal_answer(id, answer, candidates, error)) {
            post_([this, id, error] { finish_incoming(id, error); });
          }
        });
      },
      [this, id = session.connect_id](int state) {
        if (state == JUICE_STATE_COMPLETED) {
          plog("incoming state completed");
          deliver_incoming(id);
        } else if (state == JUICE_STATE_FAILED) {
          plog("incoming ICE failed");
          finish_incoming(id, "ICE failed");
        }
      },
      nullptr);

  std::string error;
  if (!agent->start(stun_host_, stun_port_, error)) {
    finish_incoming(session.connect_id, error);
    return;
  }
  agent->apply_remote_offer(session.offer, session.from_candidates);
}

void p2p_manager::start_relay_incoming(const std::string& connect_id,
                                       std::string url, std::string ca) {
  auto it = incoming_.find(connect_id);
  if (it == incoming_.end() || it->second.finished) return;
  plog("relay start incoming " + connect_id);
  auto relay = std::make_shared<relay_transport>(std::move(url), std::move(ca),
                                                 post_);
  auto switcher = std::make_shared<switchable_transport>(relay);
  it->second.relay = relay;
  it->second.switcher = switcher;
  relay->set_ready_handler(
      [this, connect_id] { on_incoming_relay_ready(connect_id); });
  relay->start();
}

void p2p_manager::on_incoming_relay_ready(const std::string& connect_id) {
  auto it = incoming_.find(connect_id);
  if (it == incoming_.end() || it->second.finished) return;
  incoming& entry = it->second;
  if (entry.relay_delivered) return;
  if (entry.ice_delivered) {
    if (entry.relay) entry.relay->close(transport_close::normal,
                                        "p2p already established");
    return;
  }
  plog("incoming " + connect_id + " relay ready, starting session");
  entry.relay_delivered = true;
  auto controller = std::make_shared<session_controller>(
      registry_, entry.switcher, options_);
  controller->start(nullptr);
}

void p2p_manager::deliver_incoming(const std::string& connect_id) {
  auto it = incoming_.find(connect_id);
  if (it == incoming_.end() || it->second.finished) return;
  incoming& entry = it->second;
  if (entry.ice_delivered) return;
  entry.ice_delivered = true;
  plog("incoming " + connect_id + " ICE completed");
  const auto endpoint = parse_endpoint(entry.agent->selected_remote());
  auto transport = std::make_shared<juice_transport>(
      entry.agent, endpoint.first, endpoint.second);

  if (entry.relay_delivered && entry.switcher) {
    plog("incoming " + connect_id + " switching to p2p");
    entry.switcher->set_ice(transport);
    entry.finished = true;
    completed_incoming_.insert(connect_id);
    incoming_.erase(it);
    return;
  }

  entry.finished = true;
  completed_incoming_.insert(connect_id);
  incoming_.erase(it);
  auto controller =
      std::make_shared<session_controller>(registry_, transport, options_);
  controller->start(nullptr);
}

void p2p_manager::finish_incoming(const std::string& connect_id,
                                  const std::string& error) {
  (void)error;
  auto it = incoming_.find(connect_id);
  if (it == incoming_.end()) return;
  incoming& entry = it->second;
  entry.finished = true;
  if (entry.agent) entry.agent->close();
  if (entry.relay_delivered) {
    // The session already lives on the relay; do not close the relay/switcher.
    incoming_.erase(it);
    return;
  }
  if (entry.relay) entry.relay->close(transport_close::normal, "connect failed");
  completed_incoming_.insert(connect_id);
  incoming_.erase(it);
}

std::pair<std::string, unsigned int> p2p_manager::parse_endpoint(
    const std::string& endpoint) {
  if (endpoint.empty()) return {"p2p", 0};
  std::string host;
  std::string port;
  if (endpoint.front() == '[') {
    const std::size_t close = endpoint.find(']');
    if (close == std::string::npos) return {endpoint, 0};
    host = endpoint.substr(1, close - 1);
    if (close + 2 <= endpoint.size() && endpoint[close + 1] == ':') {
      port = endpoint.substr(close + 2);
    }
  } else {
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) return {endpoint, 0};
    host = endpoint.substr(0, colon);
    port = endpoint.substr(colon + 1);
  }
  const long parsed = std::strtol(port.c_str(), nullptr, 10);
  const unsigned int value =
      parsed > 0 && parsed <= 65535 ? static_cast<unsigned int>(parsed) : 0;
  return {host, value};
}

}  // namespace molink
