// Makes a throwaway CA and a localhost server certificate for development.
// Never use these outside your own machine: the CA key sits right next to them.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

template <auto Free> struct Deleter {
    template <class T> void operator()(T *pointer) const { Free(pointer); }
};
using Key = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY_free>>;
using Certificate = std::unique_ptr<X509, Deleter<X509_free>>;
using Bio = std::unique_ptr<BIO, Deleter<BIO_free_all>>;

constexpr long day = 24 * 60 * 60;

[[noreturn]] void fail(const std::string &what) {
    const auto code = ERR_get_error();
    throw std::runtime_error(
        what + (code ? std::string(": ") + ERR_error_string(code, nullptr) : std::string()));
}

void check(int result, const char *what) {
    if (result != 1) {
        fail(what);
    }
}

Key new_key() {
    Key key(EVP_EC_gen("P-256"));
    if (!key) {
        fail("Can't generate a key");
    }
    return key;
}

void add_extension(X509 *certificate, X509 *issuer, int nid, const std::string &value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509_EXTENSION *extension = X509V3_EXT_nconf_nid(nullptr, &context, nid, value.c_str());
    if (extension == nullptr) {
        fail("Bad extension " + value);
    }
    const int added = X509_add_ext(certificate, extension, -1);
    X509_EXTENSION_free(extension);
    check(added, "Can't add an extension");
}

struct Validity {
    long not_before = -day; // a day of slack for clocks that are a bit behind
    long not_after = 365 * day;
};

// issuer == nullptr means self-signed, i.e. a CA
Certificate make_certificate(const std::string &common_name, EVP_PKEY *key, X509 *issuer,
                             EVP_PKEY *issuer_key, const std::string &alt_names,
                             Validity validity) {
    Certificate certificate(X509_new());
    if (!certificate) {
        fail("Out of memory");
    }
    X509 *self = certificate.get();
    check(X509_set_version(self, X509_VERSION_3), "Can't set version");

    std::unique_ptr<BIGNUM, Deleter<BN_free>> serial(BN_new());
    check(BN_rand(serial.get(), 127, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY), "Can't pick a serial");
    if (BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(self)) == nullptr) {
        fail("Can't set the serial");
    }
    X509_gmtime_adj(X509_getm_notBefore(self), validity.not_before);
    X509_gmtime_adj(X509_getm_notAfter(self), validity.not_after);
    check(X509_set_pubkey(self, key), "Can't set the public key");

    X509_NAME *name = X509_get_subject_name(self);
    check(X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char *>("Pulse dev"), -1, -1,
                                     0),
          "Can't set O");
    check(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char *>(common_name.c_str()),
                                     -1, -1, 0),
          "Can't set CN");

    X509 *signer = issuer != nullptr ? issuer : self;
    check(X509_set_issuer_name(self, X509_get_subject_name(signer)), "Can't set the issuer");
    if (issuer == nullptr) {
        add_extension(self, signer, NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
        add_extension(self, signer, NID_key_usage, "critical,keyCertSign,cRLSign");
        add_extension(self, signer, NID_subject_key_identifier, "hash");
    } else {
        add_extension(self, signer, NID_basic_constraints, "critical,CA:FALSE");
        add_extension(self, signer, NID_key_usage, "critical,digitalSignature");
        add_extension(self, signer, NID_ext_key_usage, "serverAuth");
        add_extension(self, signer, NID_subject_alt_name, alt_names);
        add_extension(self, signer, NID_subject_key_identifier, "hash");
        add_extension(self, signer, NID_authority_key_identifier, "keyid:always");
    }
    if (X509_sign(self, issuer_key != nullptr ? issuer_key : key, EVP_sha256()) == 0) {
        fail("Can't sign " + common_name);
    }
    return certificate;
}

void write_certificate(const fs::path &path, X509 *certificate) {
    Bio bio(BIO_new_file(path.string().c_str(), "w"));
    if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) {
        fail("Can't write " + path.string());
    }
}

void write_key(const fs::path &path, EVP_PKEY *key) {
    Bio bio(BIO_new_file(path.string().c_str(), "w"));
    if (!bio ||
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        fail("Can't write " + path.string());
    }
    bio.reset();
    // owner-only on Linux; Windows ignores most of this and that's fine for a dev key
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
}

void issue(const fs::path &directory, const std::string &name, X509 *ca, EVP_PKEY *ca_key,
           const std::string &alt_names, Validity validity = {}) {
    const auto key = new_key();
    const auto certificate =
        make_certificate("Pulse " + name, key.get(), ca, ca_key, alt_names, validity);
    write_certificate(directory / (name + ".crt"), certificate.get());
    write_key(directory / (name + ".key"), key.get());
}

bool is_ip(const std::string &host) {
    return host.find_first_not_of("0123456789.") == std::string::npos ||
           host.find(':') != std::string::npos;
}

int usage(std::ostream &out, int code) {
    out << "Usage: pulse-dev-certs <directory> [--host name]... [--test-set] [--force]\n"
           "Writes ca.crt, ca.key, server.crt and server.key. The server certificate covers\n"
           "localhost, 127.0.0.1 and ::1 plus every --host you add.\n"
           "--test-set also writes expired, wrong-name and untrusted certificates for tests.\n"
           "--force replaces an existing CA (anyone trusting the old one has to re-trust).\n";
    return code;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            return usage(argc < 2 ? std::cerr : std::cout, argc < 2 ? 1 : 0);
        }
        const fs::path directory = argv[1];
        std::vector<std::string> hosts{"localhost", "127.0.0.1", "::1"};
        bool test_set = false;
        bool force = false;
        for (int index = 2; index < argc; ++index) {
            const std::string_view option = argv[index];
            if (option == "--host" && index + 1 < argc) {
                hosts.emplace_back(argv[++index]);
            } else if (option == "--test-set") {
                test_set = true;
            } else if (option == "--force") {
                force = true;
            } else {
                return usage(std::cerr, 1);
            }
        }
        if (fs::exists(directory / "ca.key") && !force) {
            std::cerr << directory.string()
                      << " already has a CA. Pass --force to replace it, or pick another "
                         "directory.\n";
            return 1;
        }
        fs::create_directories(directory);

        std::string alt_names;
        for (const auto &host : hosts) {
            alt_names +=
                (alt_names.empty() ? "" : ",") + std::string(is_ip(host) ? "IP:" : "DNS:") + host;
        }

        const auto ca_key = new_key();
        const auto ca =
            make_certificate("Pulse development CA", ca_key.get(), nullptr, nullptr, "", {});
        write_certificate(directory / "ca.crt", ca.get());
        write_key(directory / "ca.key", ca_key.get());
        issue(directory, "server", ca.get(), ca_key.get(), alt_names);

        if (test_set) {
            issue(directory, "expired", ca.get(), ca_key.get(), alt_names,
                  Validity{-30 * day, -day});
            issue(directory, "wrong-name", ca.get(), ca_key.get(), "DNS:wrong.example");
            // signed by a CA nobody trusts
            const auto rogue_key = new_key();
            const auto rogue =
                make_certificate("Pulse rogue CA", rogue_key.get(), nullptr, nullptr, "", {});
            issue(directory, "untrusted", rogue.get(), rogue_key.get(), alt_names);
        }

        std::cout << "Wrote development certificates to " << directory.string() << '\n'
                  << "Server: --cert " << (directory / "server.crt").string() << " --key "
                  << (directory / "server.key").string() << '\n'
                  << "Client: --ca " << (directory / "ca.crt").string() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pulse-dev-certs: " << error.what() << '\n';
        return 1;
    }
}
