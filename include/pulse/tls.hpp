#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace pulse::tls {

using Stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

// TLS 1.2+ server context. `certificate` is a PEM chain (leaf first), `key` its
// PEM private key. Throws if either is unreadable or they don't match.
boost::asio::ssl::context server_context(const std::filesystem::path &certificate,
                                         const std::filesystem::path &key);

// TLS 1.2+ client context that always verifies the server. Trusts only `ca`
// when given, otherwise the system's default store.
boost::asio::ssl::context client_context(const std::optional<std::filesystem::path> &ca);

// Pins the name the server's certificate must match (DNS name or IP literal) and
// sends SNI for DNS names. Call before the handshake.
void expect_server_name(Stream &stream, const std::string &host);

// Why certificate verification failed, e.g. "certificate has expired". Empty if
// the failure wasn't about the certificate.
std::string verify_failure(Stream &stream);

} // namespace pulse::tls
