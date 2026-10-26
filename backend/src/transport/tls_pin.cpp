#include "transport/tls_pin.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifndef MOLINK_HAVE_OPENSSL
#define MOLINK_HAVE_OPENSSL 0
#endif

#if MOLINK_HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

namespace molink {
namespace {

namespace fs = std::filesystem;

#if MOLINK_HAVE_OPENSSL
std::string digest_of(X509* cert) {
  unsigned char digest[EVP_MAX_MD_SIZE] = {};
  unsigned int length = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
    return std::string();
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 3);
  for (unsigned int i = 0; i < length; ++i) {
    if (i != 0) out.push_back(':');
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}
#endif

fs::path pin_directory() {
  std::error_code ignored;
  const fs::path dir = fs::temp_directory_path(ignored) / "molink-pins";
  fs::create_directories(dir, ignored);
  return dir;
}

bool write_private(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out.good()) return false;
  out.close();
#if !defined(_WIN32)
  std::error_code ignored;
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ignored);
#endif
  return true;
}

}  // namespace

std::string normalize_fingerprint(const std::string& fingerprint) {
  std::string out;
  out.reserve(fingerprint.size());
  for (unsigned char c : fingerprint) {
    if (c == ':' || c == '-' || c == ' ' || c == '\t') continue;
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

pinned_certificate pin_certificate(const std::string& pem,
                                   const std::string& expected_fingerprint) {
  pinned_certificate result;
  if (pem.empty()) {
    result.error = "no certificate was provided";
    return result;
  }
  std::string fingerprint;
#if MOLINK_HAVE_OPENSSL
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    result.error = "could not read the certificate";
    return result;
  }
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (cert == nullptr) {
    result.error = "the advertised certificate is not valid PEM";
    return result;
  }
  fingerprint = digest_of(cert);
  X509_free(cert);
  if (fingerprint.empty()) {
    result.error = "could not hash the certificate";
    return result;
  }
#else
  if (!expected_fingerprint.empty()) {
    result.error = "this build cannot verify certificate fingerprints";
    return result;
  }
  // Stable content-addressed name without a crypto library.
  std::size_t hash = 1469598103934665603ull;
  for (unsigned char c : pem) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  fingerprint = std::to_string(hash);
#endif

  if (!expected_fingerprint.empty() &&
      normalize_fingerprint(expected_fingerprint) !=
          normalize_fingerprint(fingerprint)) {
    result.error = "the advertised certificate does not match its fingerprint";
    return result;
  }
  result.fingerprint = fingerprint;

  std::string name = normalize_fingerprint(fingerprint);
  if (name.empty()) name = "cert";
  const fs::path path = pin_directory() / (name + ".pem");
  std::error_code ignored;
  if (!fs::exists(path, ignored)) {
    if (!write_private(path, pem)) {
      result.error = "could not cache the certificate";
      return result;
    }
  } else {
    // Reuse the cached copy only if it is the same certificate.
    std::ifstream in(path, std::ios::binary);
    std::string existing((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (existing != pem && !write_private(path, pem)) {
      result.error = "could not refresh the certificate cache";
      return result;
    }
  }
  result.ca_path = path.string();
  return result;
}

}  // namespace molink
