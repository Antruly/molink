#pragma once

#include <string>

namespace molink {

/// Per-device TLS identity for the encrypted transports.
///
/// The certificate is self-signed on purpose: clients pin its SHA-256
/// fingerprint through the already authenticated signaling/code exchange
/// instead of trusting a public CA. It is generated once and reused, so the
/// pin survives restarts.
struct tls_identity {
  std::string cert_path;
  std::string key_path;
  /// Lowercase hex SHA-256 of the DER certificate, colon separated.
  std::string fingerprint;
  /// Empty when the identity is usable.
  std::string error;

  bool ok() const { return error.empty() && !cert_path.empty(); }
};

/// Load `tls/device.crt` + `tls/device.key` from @p config_dir, generating a
/// self-signed certificate on first use. Never throws.
tls_identity load_or_create_tls_identity(const std::string& config_dir,
                                         const std::string& common_name);

}  // namespace molink
