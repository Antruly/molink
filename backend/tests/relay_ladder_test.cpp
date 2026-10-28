// Relay endpoint fallback: the ladder starts the preferred transport, moves
// to the next when an attempt fails before pairing, and only reports a close
// once an active transport is gone.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "transport/relay_ladder.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

class fake_transport final : public molink::relay_session {
 public:
  /// `announce_ready = false` models an attempt that is still pairing: it has
  /// started but the relay has not sent `relay-ready` yet.
  explicit fake_transport(std::string name, bool fail_on_start,
                          bool announce_ready = true)
      : name_(std::move(name)),
        fail_on_start_(fail_on_start),
        announce_ready_(announce_ready) {}

  const char* name() const override { return name_.c_str(); }
  const char* path() const override { return "relay"; }
  std::string peer_ip() const override { return name_; }
  unsigned int peer_port() const override { return 0; }
  bool is_open() const override { return opened_; }
  bool send_text(const std::string& text) override {
    sends.push_back(text);
    return true;
  }
  bool send_binary(const std::uint8_t*, std::size_t) override { return true; }
  void close(int, const std::string&) override { close_called = true; }
  void set_handlers(molink::session_transport::text_handler on_text,
                    molink::session_transport::binary_handler,
                    molink::session_transport::close_handler on_close) override {
    text_ = std::move(on_text);
    close_ = std::move(on_close);
  }
  void set_ready_handler(std::function<void()> ready) override {
    ready_cb_ = std::move(ready);
    if (opened_ && ready_cb_) {
      std::function<void()> handler = ready_cb_;
      handler();
    }
  }
  void start() override {
    if (started) return;
    started = true;
    if (fail_on_start_) {
      close_handler handler = close_;
      if (handler) {
        handler(molink::transport_close::policy, "fake connect failed");
      }
      return;
    }
    if (!announce_ready_) return;   // still pairing
    opened_ = true;
    if (ready_cb_) {
      std::function<void()> handler = ready_cb_;
      handler();
    }
  }

  /// Feeds a text frame the way a transport's read callback would.
  void deliver_text(const std::string& text) {
    text_handler handler = text_;
    if (handler) handler(text);
  }

  void emit_close() {
    close_handler handler = close_;
    if (handler) handler(molink::transport_close::normal, "fake closed");
  }

  bool started = false;
  bool close_called = false;
  std::vector<std::string> sends;

 private:
  std::string name_;
  bool fail_on_start_;
  bool announce_ready_;
  bool opened_ = false;
  molink::session_transport::text_handler text_;
  molink::session_transport::close_handler close_;
  std::function<void()> ready_cb_;
};

std::shared_ptr<molink::relay_session> as_session(
    const std::shared_ptr<fake_transport>& transport) {
  return transport;
}

}  // namespace

int main() {
  // 1. The preferred endpoint fails; the next one becomes active.
  {
    auto failing = std::make_shared<fake_transport>("quic", true);
    auto working = std::make_shared<fake_transport>("tcp", false);
    std::vector<molink::relay_ladder::factory> factories = {
        [failing] { return as_session(failing); },
        [working] { return as_session(working); },
    };
    molink::relay_ladder ladder(nullptr, std::move(factories));
    bool ready = false;
    int ladder_closes = 0;
    std::vector<std::string> received;
    ladder.set_ready_handler([&] { ready = true; });
    ladder.set_handlers(
        [&](const std::string& text) { received.push_back(text); },
        [](const std::uint8_t*, std::size_t) {},
        [&](int, const std::string&) { ++ladder_closes; });
    ladder.start();
    expect(ready, "the second endpoint becomes ready");
    expect(std::string(ladder.name()) == "tcp", "the active protocol is tcp");
    expect(ladder.is_open(), "the ladder is open once paired");
    expect(working->started, "the second endpoint was started");
    expect(ladder.send_text("hello"), "send after pairing");
    expect(working->sends.size() == 1 && working->sends[0] == "hello",
           "the send reaches the active endpoint");
    expect(received.empty(), "no text was invented");

    // A late close from the abandoned attempt must not tear down the ladder.
    failing->emit_close();
    expect(ladder.is_open(), "a stale attempt cannot close the ladder");
    expect(ladder_closes == 0, "no close was reported for the stale attempt");

    // The active transport closing is reported.
    working->emit_close();
    expect(ladder_closes == 1, "the active close is reported once");
    expect(!ladder.is_open(), "the ladder is closed after the active close");
  }

  // 2. Every endpoint fails: one close, no ready.
  {
    auto only = std::make_shared<fake_transport>("quic", true);
    std::vector<molink::relay_ladder::factory> factories = {
        [only] { return as_session(only); },
    };
    molink::relay_ladder ladder(nullptr, std::move(factories));
    bool ready = false;
    int closes = 0;
    ladder.set_ready_handler([&] { ready = true; });
    ladder.set_handlers([](const std::string&) {},
                        [](const std::uint8_t*, std::size_t) {},
                        [&](int, const std::string&) { ++closes; });
    ladder.start();
    expect(!ready, "no endpoint became ready");
    expect(closes == 1, "the ladder reports one final close");
    expect(!ladder.is_open(), "a failed ladder is not open");
  }

  // 3. A transport that fails from inside its own data callback must outlive
  //    that callback. Letting the last reference go there frees the WebSocket
  //    parser while the frame it is dispatching is still on the stack -- the
  //    h3 relay crash of 2026-10-29 (trampoline_frame with a null argument).
  {
    std::weak_ptr<fake_transport> first_weak;
    fake_transport* first_raw = nullptr;
    auto working = std::make_shared<fake_transport>("tcp", false);
    std::vector<molink::relay_ladder::factory> factories = {
        [&first_weak, &first_raw] {
          auto transport = std::make_shared<fake_transport>(
              "h3", false, /*announce_ready=*/false);
          first_weak = transport;
          first_raw = transport.get();
          return as_session(transport);   // only the ladder keeps it alive
        },
        [working] { return as_session(working); },
    };
    molink::relay_ladder ladder(nullptr, std::move(factories));
    bool ready = false;
    bool alive_in_callback = false;
    ladder.set_ready_handler([&] { ready = true; });
    ladder.set_handlers(
        [&](const std::string&) {
          // This runs inside the transport's read callback: the relay answered
          // and the ladder decides to give up on this endpoint right here.
          first_raw->emit_close();
          alive_in_callback = !first_weak.expired();
        },
        [](const std::uint8_t*, std::size_t) {},
        [](int, const std::string&) {});
    ladder.start();
    expect(first_raw != nullptr, "the first endpoint was created");
    if (first_raw != nullptr) first_raw->deliver_text("relay-error");
    expect(alive_in_callback,
           "the abandoned transport outlives its own data callback");
    expect(ready, "the ladder falls through to the next endpoint");
  }

  // 4. No factories at all also closes exactly once.
  {
    molink::relay_ladder ladder(nullptr, {});
    int closes = 0;
    ladder.set_handlers([](const std::string&) {},
                        [](const std::uint8_t*, std::size_t) {},
                        [&](int, const std::string&) { ++closes; });
    ladder.start();
    expect(closes == 1, "an empty ladder closes once");
  }

  std::printf("relay ladder: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
