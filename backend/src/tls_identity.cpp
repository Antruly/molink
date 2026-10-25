#include "tls_identity.h"

#if MOLINK_HAVE_OPENSSL

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace molink {
namespace {

namespace fs = std::filesystem;

std::string format_fingerprint(const unsigned char* digest, unsigned int len) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 3);
  for (unsigned int i = 0; i < len; ++i) {
    if (i != 0) out.push_back(':');
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

bool fingerprint_of(X509* cert, std::string& out) {
  if (cert == nullptr) return false;
  unsigned char digest[EVP_MAX_MD_SIZE] = {};
  unsigned int length = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) return false;
  out = format_fingerprint(digest, length);
  return true;
}

bool write_certificate_pem(const std::string& path, X509* cert) {
  BIO* bio = BIO_new_file(path.c_str(), "wb");
  if (bio == nullptr) return false;
  const bool ok = PEM_write_bio_X509(bio, cert) == 1;
  BIO_free(bio);
  return ok;
}

bool write_key_pem(const std::string& path, EVP_PKEY* key) {
  BIO* bio = BIO_new_file(path.c_str(), "wb");
  if (bio == nullptr) return false;
  const bool ok = PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0,
                                           nullptr, nullptr) == 1;
  BIO_free(bio);
  return ok;
}

void restrict_key_permissions(const std::string& path) {
#if !defined(_WIN32)
  ::chmod(path.c_str(), 0600);
#else
  (void)path;
#endif
}

bool load_certificate(const std::string& path, X509** cert) {
  BIO* bio = BIO_new_file(path.c_str(), "rb");
  if (bio == nullptr) return false;
  *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return *cert != nullptr;
}

bool load_key(const std::string& path, EVP_PKEY** key) {
  BIO* bio = BIO_new_file(path.c_str(), "rb");
  if (bio == nullptr) return false;
  *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return *key != nullptr;
}

bool generate_identity(const std::string& cert_path, const std::string& key_path,
                       const std::string& common_name, std::string& fingerprint,
                       std::string& error) {
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (ctx == nullptr) {
    error = "could not create the key context";
    return false;
  }
  EVP_PKEY* key = nullptr;
  bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
                ctx, NID_X9_62_prime256v1) == 1 &&
            EVP_PKEY_keygen(ctx, &key) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || key == nullptr) {
    error = "could not generate the device key";
    if (key != nullptr) EVP_PKEY_free(key);
    return false;
  }

  X509* cert = X509_new();
  if (cert == nullptr) {
    EVP_PKEY_free(key);
    error = "could not allocate the certificate";
    return false;
  }
  std::random_device random;
  const long serial =
      static_cast<long>((random() & 0x7fffffffu) ^ (random() << 1));
  ok = X509_set_version(cert, 2) == 1;
  if (ok) ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
  if (ok) {
    ok = X509_gmtime_adj(X509_getm_notBefore(cert), -60) != nullptr;
  }
  if (ok) {
    // Ten years: this is a pinned device identity, not a public certificate.
    ok = X509_gmtime_adj(X509_getm_notAfter(cert), 60ll * 60 * 24 * 3650) !=
         nullptr;
  }
  if (ok) ok = X509_set_pubkey(cert, key) == 1;
  X509_NAME* name = X509_get_subject_name(cert);
  const std::string cn = common_name.empty() ? "moLink" : common_name;
  if (ok && name != nullptr) {
    ok = X509_NAME_add_entry_by_txt(
             name, "CN", MBSTRING_ASC,
             reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0) ==
         1;
    if (ok) ok = X509_set_issuer_name(cert, name) == 1;
  } else {
    ok = false;
  }
  if (ok) ok = X509_sign(cert, key, EVP_sha256()) > 0;
  if (!ok) {
    error = "could not build the self-signed certificate";
    X509_free(cert);
    EVP_PKEY_free(key);
    return false;
  }

  std::string print;
  ok = fingerprint_of(cert, print) && write_certificate_pem(cert_path, cert) &&
       write_key_pem(key_path, key);
  if (ok) restrict_key_permissions(key_path);
  if (!ok) error = "could not write the device certificate";
  if (ok) fingerprint = std::move(print);
  X509_free(cert);
  EVP_PKEY_free(key);
  return ok;
}

}  // namespace

tls_identity load_or_create_tls_identity(const std::string& config_dir,
                                         const std::string& common_name) {
  tls_identity identity;
  if (config_dir.empty()) {
    identity.error = "cannot resolve the configuration directory";
    return identity;
  }
  const fs::path dir = fs::path(config_dir) / "tls";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    identity.error = "cannot create the TLS directory";
    return identity;
  }
  identity.cert_path = (dir / "device.crt").string();
  identity.key_path = (dir / "device.key").string();

  X509* cert = nullptr;
  EVP_PKEY* key = nullptr;
  if (load_certificate(identity.cert_path, &cert) &&
      load_key(identity.key_path, &key) &&
      X509_check_private_key(cert, key) == 1 &&
      fingerprint_of(cert, identity.fingerprint)) {
    X509_free(cert);
    EVP_PKEY_free(key);
    return identity;
  }
  if (cert != nullptr) X509_free(cert);
  if (key != nullptr) EVP_PKEY_free(key);
  // A half-written or mismatched pair would otherwise pin a certificate that
  // can never complete a handshake; regenerate instead.
  fs::remove(identity.cert_path, ec);
  fs::remove(identity.key_path, ec);
  if (!generate_identity(identity.cert_path, identity.key_path, common_name,
                         identity.fingerprint, identity.error)) {
    return identity;
  }
  return identity;
}

}  // namespace molink

#else  // !MOLINK_HAVE_OPENSSL

namespace molink {

tls_identity load_or_create_tls_identity(const std::string&, const std::string&) {
  tls_identity identity;
  identity.error = "this build has no OpenSSL; encrypted transport is unavailable";
  return identity;
}

}  // namespace molink

#endif
