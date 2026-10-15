#include <pulse/tls.hpp>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <stdexcept>

namespace pulse::tls {
namespace ssl = boost::asio::ssl;

namespace {

void harden(ssl::context &context) {
    context.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                        ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
    if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1) {
        throw std::runtime_error("Can't require TLS 1.2");
    }
}

bool is_ip_literal(const std::string &host) {
    boost::system::error_code error;
    boost::asio::ip::make_address(host, error);
    return !error;
}

} // namespace

ssl::context server_context(const std::filesystem::path &certificate,
                            const std::filesystem::path &key) {
    ssl::context context(ssl::context::tls_server);
    harden(context);
    try {
        context.use_certificate_chain_file(certificate.string());
        context.use_private_key_file(key.string(), ssl::context::pem);
    } catch (const boost::system::system_error &error) {
        throw std::runtime_error("Can't load TLS certificate or key: " + std::string(error.what()));
    }
    if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
        throw std::runtime_error(
            "TLS key mismatch: the private key doesn't belong to this certificate");
    }
    return context;
}

ssl::context client_context(const std::optional<std::filesystem::path> &ca) {
    ssl::context context(ssl::context::tls_client);
    harden(context);
    context.set_verify_mode(ssl::verify_peer);
    try {
        if (ca) {
            context.load_verify_file(ca->string());
        } else {
            context.set_default_verify_paths();
        }
    } catch (const boost::system::system_error &error) {
        throw std::runtime_error("Can't load trusted CA certificates: " +
                                 std::string(error.what()));
    }
    return context;
}

void expect_server_name(Stream &stream, const std::string &host) {
    SSL *handle = stream.native_handle();
    X509_VERIFY_PARAM *params = SSL_get0_param(handle);
    if (is_ip_literal(host)) {
        if (X509_VERIFY_PARAM_set1_ip_asc(params, host.c_str()) != 1) {
            throw std::runtime_error("Can't verify against IP " + host);
        }
        return;
    }
    // no "*.example.com" matching "a.b.example.com" style surprises
    X509_VERIFY_PARAM_set_hostflags(params, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(params, host.c_str(), host.size()) != 1 ||
        SSL_set_tlsext_host_name(handle, host.c_str()) != 1) {
        throw std::runtime_error("Can't verify against host name " + host);
    }
}

std::string verify_failure(Stream &stream) {
    const long result = SSL_get_verify_result(stream.native_handle());
    if (result == X509_V_OK) {
        return {};
    }
    return X509_verify_cert_error_string(result);
}

} // namespace pulse::tls
