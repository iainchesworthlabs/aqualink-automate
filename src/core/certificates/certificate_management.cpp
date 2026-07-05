#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <boost/system/error_code.hpp>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "application/application_defaults.h"
#include "application/secure_runtime_paths.h"
#include "certificates/certificate_management.h"
#include "exceptions/exception_certificate_invalidformat.h"
#include "exceptions/exception_certificate_notfound.h"
#include "logging/logging.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace fs = std::filesystem;

namespace AqualinkAutomate::Certificates
{

	namespace
	{
		using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
		using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
		using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free_all)>;

		// SHA-256 fingerprint of a certificate, as colon-separated uppercase hex.
		std::string CertificateFingerprint(const X509* cert)
		{
			std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
			unsigned int md_len = 0;
			if (1 != X509_digest(cert, EVP_sha256(), md.data(), &md_len))
			{
				return "<unavailable>";
			}

			std::string out;
			out.reserve(md_len * 3);
			for (unsigned int i = 0; i < md_len; ++i)
			{
				out += std::format("{}{:02X}", (i == 0 ? "" : ":"), md[i]);
			}
			return out;
		}

		// True if 'dir' exists (created if necessary) and a file can be written there.
		bool DirectoryIsWritable(const fs::path& dir)
		{
			if (dir.empty())
			{
				return false;
			}

			std::error_code ec;
			fs::create_directories(dir, ec);
			if (ec)
			{
				return false;
			}

			const fs::path probe = dir / ".aqualink_write_probe";
			{
				std::ofstream f(probe, std::ios::out | std::ios::trunc);
				if (!f.is_open())
				{
					return false;
				}
			}
			std::error_code rm;
			fs::remove(probe, rm);
			return true;
		}
	}
	// unnamed namespace

	bool GenerateSelfSignedCertificate(const fs::path& certificate_path, const fs::path& private_key_path)
	{
		std::error_code dir_ec;
		fs::create_directories(certificate_path.parent_path(), dir_ec);
		fs::create_directories(private_key_path.parent_path(), dir_ec);

		// The auto-generated material can land under the system temp directory, which
		// is world-writable on shared hosts. Restrict the directory holding the private
		// key to owner-only access so other local users can neither read the key nor
		// pre-seed material that a later reuse-on-restart would trust. The key file
		// itself is additionally locked to 0600 once written (below).
		{
			std::error_code perm_ec;
			fs::permissions(private_key_path.parent_path(), fs::perms::owner_all, fs::perm_options::replace, perm_ec);
			if (perm_ec)
			{
				LogWarning(Channel::Certificates, std::format("Could not restrict permissions on SSL material directory ({}): {}", private_key_path.parent_path().string(), perm_ec.message()));
			}
		}

		// 2048-bit RSA keypair.
		EvpPkeyPtr pkey(EVP_RSA_gen(2048), &EVP_PKEY_free);
		if (!pkey)
		{
			LogWarning(Channel::Certificates, "Self-signed generation failed: could not generate RSA key");
			return false;
		}

		X509Ptr cert(X509_new(), &X509_free);
		if (!cert)
		{
			LogWarning(Channel::Certificates, "Self-signed generation failed: X509_new returned null");
			return false;
		}

		// X.509 v3.
		X509_set_version(cert.get(), 2);

		// Random 63-bit serial (top bit cleared to keep it positive).
		{
			std::array<unsigned char, 8> serial_bytes{};
			if (1 != RAND_bytes(serial_bytes.data(), static_cast<int>(serial_bytes.size())))
			{
				LogWarning(Channel::Certificates, "Self-signed generation failed: RAND_bytes for serial failed");
				return false;
			}
			serial_bytes[0] &= 0x7F;
			BIGNUM* bn = BN_bin2bn(serial_bytes.data(), static_cast<int>(serial_bytes.size()), nullptr);
			if (nullptr == bn)
			{
				return false;
			}
			BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
			BN_free(bn);
		}

		// Validity: now .. now + ~10 years.
		X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
		X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(60) * 60 * 24 * 3650);

		X509_set_pubkey(cert.get(), pkey.get());

		// Subject == issuer (self-signed), CN=localhost.
		X509_NAME* name = X509_get_subject_name(cert.get());
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
		X509_set_issuer_name(cert.get(), name);

		// Subject Alternative Names so the cert is usable for localhost/loopback.
		if (X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:::1"); nullptr != ext)
		{
			X509_add_ext(cert.get(), ext, -1);
			X509_EXTENSION_free(ext);
		}

		if (0 == X509_sign(cert.get(), pkey.get(), EVP_sha256()))
		{
			LogWarning(Channel::Certificates, "Self-signed generation failed: X509_sign failed");
			return false;
		}

		// Write the private key FIRST, then immediately restrict it to owner-only
		// (0600) before the certificate so the key is never world-readable on disk.
		{
			BioPtr key_bio(BIO_new_file(private_key_path.string().c_str(), "wb"), &BIO_free_all);
			if (!key_bio || 1 != PEM_write_bio_PrivateKey(key_bio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr))
			{
				LogWarning(Channel::Certificates, std::format("Self-signed generation failed: could not write private key to {}", private_key_path.string()));
				return false;
			}
		}
		{
			std::error_code perm_ec;
			fs::permissions(private_key_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, perm_ec);
			if (perm_ec)
			{
				LogWarning(Channel::Certificates, std::format("Generated private key but could not restrict its permissions ({}): {}", private_key_path.string(), perm_ec.message()));
			}
		}
		{
			BioPtr cert_bio(BIO_new_file(certificate_path.string().c_str(), "wb"), &BIO_free_all);
			if (!cert_bio || 1 != PEM_write_bio_X509(cert_bio.get(), cert.get()))
			{
				LogWarning(Channel::Certificates, std::format("Self-signed generation failed: could not write certificate to {}", certificate_path.string()));
				return false;
			}
		}

		LogInfo(Channel::Certificates, std::format(
			"Generated a unique self-signed certificate (CN=localhost) at {} (key: {}); SHA-256 fingerprint {}",
			certificate_path.string(), private_key_path.string(), CertificateFingerprint(cert.get())));
		return true;
	}

	namespace Detail
	{

		std::optional<Options::Web::SslCertificate> GenerateOrReuseInSecureDirectories(const std::vector<fs::path>& base_dirs)
		{
			std::error_code ec;

			// Each base is a per-user PRIVATE runtime directory (see
			// Application::SecureRuntimeStateDirectories). PrepareSecureDirectory creates
			// the "ssl" leaf owner-only and verifies it is a real, self-owned, non-symlink
			// directory before it is used, so both the write and the reuse below happen only
			// in a directory we control -- defeating symlink / pre-seed attacks on a shared
			// path. This is deliberately NOT the world-writable system temp directory.
			for (const fs::path& base : base_dirs)
			{
				const fs::path dir = base / "ssl";
				if (!Application::PrepareSecureDirectory(dir))
				{
					continue;
				}

				const fs::path cert_out = dir / "cert.pem";
				const fs::path key_out = dir / "key.pem";

				// Safe to reuse across restarts: the directory has just been verified as
				// owner-only and self-owned, so an attacker cannot have planted these.
				if (fs::exists(cert_out, ec) && fs::exists(key_out, ec))
				{
					LogInfo(Channel::Certificates, std::format("Reusing previously-generated self-signed certificate at {}", cert_out.string()));
					return Options::Web::SslCertificate{ cert_out, key_out };
				}

				if (GenerateSelfSignedCertificate(cert_out, key_out))
				{
					return Options::Web::SslCertificate{ cert_out, key_out };
				}
			}

			return std::nullopt;
		}

	}
	// namespace Detail

	std::optional<Options::Web::SslCertificate> EnsureSelfSignedMaterial(const Options::Web::SslCertificate& configured)
	{
		std::error_code ec;
		if (fs::exists(configured.certificate, ec) && fs::exists(configured.private_key, ec))
		{
			// Operator-supplied or previously-generated material is present; use it.
			return configured;
		}

		// Auto-generate ONLY for the built-in default paths. If an operator explicitly
		// pointed --cert/--cert-key at a file that does not exist, that is a
		// misconfiguration that must fail loudly (the caller throws), not be silently
		// papered over with a generated cert.
		if (const bool using_defaults =
			(configured.certificate == fs::path(Application::DEFAULT_CERTIFICATE)) &&
			(configured.private_key == fs::path(Application::DEFAULT_PRIVATE_KEY)); !using_defaults)
		{
			return std::nullopt;
		}

		// Prefer the configured directory so material persists at the expected
		// location on writable installs.
		if (const fs::path dir = configured.private_key.parent_path(); DirectoryIsWritable(dir))
		{
			const fs::path cert_out = dir / "cert.pem";
			const fs::path key_out = dir / "key.pem";

			if (fs::exists(cert_out, ec) && fs::exists(key_out, ec))
			{
				LogInfo(Channel::Certificates, std::format("Reusing previously-generated self-signed certificate at {}", cert_out.string()));
				return Options::Web::SslCertificate{ cert_out, key_out };
			}

			if (GenerateSelfSignedCertificate(cert_out, key_out))
			{
				return Options::Web::SslCertificate{ cert_out, key_out };
			}
		}

		// Fall back to a per-user PRIVATE runtime directory when the install tree is
		// read-only (the common packaged case). Deliberately NOT the world-writable
		// system temp directory; see Detail::GenerateOrReuseInSecureDirectories.
		return Detail::GenerateOrReuseInSecureDirectories(Application::SecureRuntimeStateDirectories());
	}

	void LoadSslCertificates(const AqualinkAutomate::Options::Web::WebSettings& cfg, boost::asio::ssl::context& ctx)
	{
		if (!cfg.https_server_is_enabled)
		{
			LogDebug(Channel::Certificates, "Certificates::LoadCertificates - HTTPS server has been disabled...not loading certificates");
			return;
		}

		// Resolve the certificate material: use the configured/operator-supplied files
		// if present, otherwise (for the built-in defaults only) generate a UNIQUE
		// per-install self-signed pair. This replaces the historical behaviour of
		// shipping a single private key committed to the public repository -- which
		// gave every install the same key and provided no real protection.
		const auto resolved = EnsureSelfSignedMaterial(cfg.ssl_certificate);
		if (!resolved.has_value())
		{
			LogWarning(Channel::Certificates, std::format(
				"Certificates::LoadCertificates Failure: certificate '{}' or private key '{}' is missing and could not be generated",
				cfg.ssl_certificate.certificate.string(), cfg.ssl_certificate.private_key.string()));
			throw Exceptions::Certificate_NotFound();
		}

		boost::system::error_code ec;

		if (ctx.use_certificate_file(resolved->certificate.string(), boost::asio::ssl::context::pem, ec); ec)
		{
			LogWarning(Channel::Certificates, std::format("Certificates::LoadCertificates Failure: Cannot Load Certificate File - Error: {}", ec.message()));
			throw Exceptions::Certificate_InvalidFormat();
		}
		else if (ctx.use_private_key_file(resolved->private_key.string(), boost::asio::ssl::context::pem, ec); ec)
		{
			LogWarning(Channel::Certificates, std::format("Certificates::LoadCertificates Failure: Cannot Load Certificate Key File - Error: {}", ec.message()));
			throw Exceptions::Certificate_InvalidFormat();
		}
		else
		{
			if (!cfg.ca_chain_certificate.has_value())
			{
				LogTrace(Channel::Certificates, "Certificates::LoadCertificates - CA chain certificate path was not provided; ignoring");
			}
			else if (!std::filesystem::exists(cfg.ca_chain_certificate.value()))
			{
				LogWarning(Channel::Certificates, std::format("Certificates::LoadCertificates Failure: cannot find specified ca chain certificate file; path was -> {}", cfg.ca_chain_certificate.value().string()));
				throw Exceptions::Certificate_NotFound();
			}
			else if (ctx.use_certificate_chain_file(cfg.ca_chain_certificate.value().string(), ec); ec)
			{
				LogWarning(Channel::Certificates, std::format("Certificates::LoadCertificates Failure: Cannot Load CA Chain Certificate File - Error: {}", ec.message()));
				throw Exceptions::Certificate_InvalidFormat();
			}

			LogDebug(Channel::Certificates, "Certificates::LoadCertificates - webserver certificates loaded successfully");
		}
	}

}
// namespace AqualinkAutomate::Certificates
