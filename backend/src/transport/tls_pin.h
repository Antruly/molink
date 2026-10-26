#pragma once

#include <string>

namespace molink {

/// A peer certificate materialized as a CA file for libuvcpp.
///
/// libuvcpp's ssl context can only load trusted CAs from a file
/// (`load_ca_file`), so a certificate advertised in JSON is written to a
/// content-addressed temp file first. The fingerprint is verified before the
/// file is used when the peer also published one.
struct pinned_certificate {
  /// PEM file path to pass to `uvcpp_ssl_context::load_ca_file`.
  std::string ca_path;
  /// SHA-256 of the DER certificate, colon-separated lowercase hex.
  std::string fingerprint;
  /// Empty when the certificate was accepted.
  std::string error;

  bool ok() const { return error.empty() && !ca_path.empty(); }
};

/// Verify @p pem against @p expected_fingerprint (when non-empty) and write it
/// to a cached PEM file. Never throws. When the build has no OpenSSL the
/// fingerprint cannot be computed and an expected fingerprint is rejected;
/// a certificate without one is still cached.
pinned_certificate pin_certificate(const std::string& pem,
                                   const std::string& expected_fingerprint);

/// Normalize a fingerprint for comparison: lowercase, no separators.
std::string normalize_fingerprint(const std::string& fingerprint);

}  // namespace molink
