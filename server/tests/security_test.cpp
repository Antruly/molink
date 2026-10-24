#include <cstdio>

#include "auth/password.h"
#include "security/ip.h"
#include "security/rate_limiter.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                 \
  do {                                                                   \
    if (!(condition)) {                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                   #condition);                                          \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

void test_ip() {
  using namespace molink;
  ip_addr address;
  CHECK(parse_ip("127.0.0.1", address));
  CHECK(address.family == ip_family::v4);
  CHECK(is_loopback(address));
  CHECK(is_private(address));

  CHECK(parse_ip("192.168.1.20", address));
  CHECK(is_private(address));
  CHECK(classify_ip(address) == ip_category::private_net);

  CHECK(parse_ip("8.8.8.8", address));
  CHECK(classify_ip(address) == ip_category::overseas);

  CHECK(parse_ip("114.114.114.114", address));
  CHECK(classify_ip(address) == ip_category::mainland_china);

  CHECK(parse_ip("::1", address));
  CHECK(address.family == ip_family::v6);
  CHECK(is_loopback(address));

  CHECK(parse_ip("::ffff:10.1.2.3", address));
  CHECK(address.family == ip_family::v4);
  CHECK(is_private(address));

  cidr_block block;
  CHECK(parse_cidr("10.0.0.0/8", block));
  CHECK(parse_ip("10.9.8.7", address));
  CHECK(cidr_contains(block, address));
  CHECK(parse_ip("11.0.0.1", address));
  CHECK(!cidr_contains(block, address));
  CHECK(parse_cidr("0.0.0.0/0", block));
  CHECK(parse_ip("203.0.113.9", address));
  CHECK(cidr_contains(block, address));
  CHECK(!parse_cidr("10.0.0.0/33", block, nullptr));
  CHECK(!parse_ip("not-an-ip", address, nullptr));

  ip_policy policy;
  CHECK(parse_ip("8.8.8.8", address));
  CHECK(!policy_allows(policy, address));
  policy.allow_overseas = true;
  CHECK(policy_allows(policy, address));
  cidr_block denied;
  CHECK(parse_cidr("8.8.8.0/24", denied));
  policy.custom_deny.push_back(denied);
  CHECK(!policy_allows(policy, address));

  std::vector<cidr_block> trusted;
  CHECK(parse_cidr("127.0.0.1/32", trusted.emplace_back()));
  ip_addr client;
  CHECK(resolve_client_ip("127.0.0.1", "8.8.8.8, 127.0.0.1", trusted, client));
  CHECK(canonical_ip(client) == "8.8.8.8");
  CHECK(!resolve_client_ip("127.0.0.1", "garbage", trusted, client));
}

void test_rate_limiter() {
  using namespace molink;
  rate_limiter limiter(10);
  int retry = 0;
  CHECK(limiter.allow("k", 1.0, 2.0, 0));
  CHECK(limiter.allow("k", 1.0, 2.0, 0));
  CHECK(!limiter.allow("k", 1.0, 2.0, 0, &retry));
  CHECK(retry >= 1);
  CHECK(limiter.allow("k", 1.0, 2.0, 1000));
  CHECK(limiter.allow("other", 1.0, 2.0, 0));
  limiter.prune(24u * 60u * 60u * 1000u);
  CHECK(limiter.size() == 0);
}

void test_password() {
  using namespace molink;
  password_hash hash;
  std::string error;
  CHECK(hash_password("correct horse battery staple", hash, error));
  CHECK(hash.iterations == 600000);
  CHECK(!hash.salt_hex.empty());
  CHECK(verify_password("correct horse battery staple", hash));
  CHECK(!verify_password("wrong password", hash));

  password_hash decoded;
  CHECK(decode_password_hash(encode_password_hash(hash), decoded));
  CHECK(decoded.salt_hex == hash.salt_hex);
  CHECK(verify_password("correct horse battery staple", decoded));

  password_hash second;
  CHECK(hash_password("correct horse battery staple", second, error));
  CHECK(second.salt_hex != hash.salt_hex);

  CHECK(!password_acceptable("short", error));
  CHECK(!password_acceptable(std::string("valid but has\ncontrol"), error));
}

}  // namespace

int main() {
  test_ip();
  test_rate_limiter();
  test_password();
  if (failures != 0) {
    std::fprintf(stderr, "%d security checks failed\n", failures);
    return 1;
  }
  std::printf("ok: security primitives\n");
  return 0;
}
