#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>

#include "application/application_defaults.h"
#include "certificates/certificate_management.h"
#include "exceptions/exception_certificate_notfound.h"
#include "exceptions/exception_certificate_invalidformat.h"
#include "options/options_web_options.h"

namespace fs = std::filesystem;
using namespace AqualinkAutomate;

//=============================================================================
// Per-install self-signed certificate generation.
//
// Replaces the historical, repository-committed shared private key (identical
// on every install). The security-critical property is UNIQUENESS: two installs
// (here, two generations) must NOT share a key. Also lock that the generated
// material is loadable by the TLS stack and that the private key is owner-only.
//=============================================================================

namespace
{
	std::string ReadFile(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_CertificateGeneration)

BOOST_AUTO_TEST_CASE(GenerateSelfSigned_ProducesParseableUniqueMaterial)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_certgen_test";
	std::error_code rm;
	fs::remove_all(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";

	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));
	BOOST_REQUIRE(fs::exists(cert));
	BOOST_REQUIRE(fs::exists(key));

	// The generated material must be loadable by the TLS stack.
	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	boost::system::error_code ec;
	ctx.use_certificate_file(cert.string(), boost::asio::ssl::context::pem, ec);
	BOOST_CHECK_MESSAGE(!ec, "certificate did not load: " << ec.message());
	ctx.use_private_key_file(key.string(), boost::asio::ssl::context::pem, ec);
	BOOST_CHECK_MESSAGE(!ec, "private key did not load: " << ec.message());

	// Uniqueness: a second generation must produce a DIFFERENT private key.
	const fs::path cert2 = dir / "cert2.pem";
	const fs::path key2 = dir / "key2.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert2, key2));
	BOOST_CHECK(ReadFile(key) != ReadFile(key2));
	BOOST_CHECK(ReadFile(cert) != ReadFile(cert2));

#ifndef _WIN32
	// The private key must not be group/world readable (0600).
	const auto perms = fs::status(key).permissions();
	BOOST_CHECK((perms & fs::perms::group_read) == fs::perms::none);
	BOOST_CHECK((perms & fs::perms::others_read) == fs::perms::none);
	BOOST_CHECK((perms & fs::perms::others_write) == fs::perms::none);

	// The directory holding the private key must also be owner-only (0700) so that
	// on a world-writable temp-directory fallback no other local user can read the
	// key or pre-seed material for the reuse-on-restart path to trust.
	const auto dir_perms = fs::status(dir).permissions();
	BOOST_CHECK((dir_perms & fs::perms::group_all) == fs::perms::none);
	BOOST_CHECK((dir_perms & fs::perms::others_all) == fs::perms::none);
#endif

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(GenerateSelfSigned_FailsWhenPrivateKeyPathIsUnwritable)
{
	// The private key is written first; if the BIO cannot open the key path the
	// generation must fail (returning false) rather than proceed. Force an
	// unopenable key path by making it an existing DIRECTORY -- a regular-file
	// BIO open against a directory fails on both Windows and POSIX.
	const fs::path dir = fs::temp_directory_path() / "aqualink_certgen_badkey";
	std::error_code rm;
	fs::remove_all(dir, rm);
	fs::create_directories(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	fs::create_directories(key, rm);   // key path is now a directory, not a file
	BOOST_REQUIRE(fs::is_directory(key));

	BOOST_CHECK(!Certificates::GenerateSelfSignedCertificate(cert, key));

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(GenerateSelfSigned_FailsWhenCertificatePathIsUnwritable)
{
	// The key writes fine but the certificate path is unopenable (an existing
	// directory), so generation must fail on the certificate write.
	const fs::path dir = fs::temp_directory_path() / "aqualink_certgen_badcert";
	std::error_code rm;
	fs::remove_all(dir, rm);
	fs::create_directories(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	fs::create_directories(cert, rm);   // cert path is now a directory, not a file
	BOOST_REQUIRE(fs::is_directory(cert));

	BOOST_CHECK(!Certificates::GenerateSelfSignedCertificate(cert, key));

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(EnsureSelfSignedMaterial_UsesExistingFiles)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_certgen_existing";
	std::error_code rm;
	fs::remove_all(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	const auto resolved = Certificates::EnsureSelfSignedMaterial(Options::Web::SslCertificate{ cert, key });
	BOOST_REQUIRE(resolved.has_value());
	BOOST_CHECK(resolved->certificate == cert);
	BOOST_CHECK(resolved->private_key == key);

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(EnsureSelfSignedMaterial_RefusesToGenerateForOperatorSpecifiedMissingPaths)
{
	// An operator who explicitly points --cert/--cert-key at a non-default path
	// that does not exist must NOT have a cert silently fabricated for them: that
	// is a misconfiguration the caller surfaces as an error.
	const fs::path missing_dir = fs::temp_directory_path() / "aqualink_certgen_missing";
	std::error_code rm;
	fs::remove_all(missing_dir, rm);

	Options::Web::SslCertificate operator_specified{ missing_dir / "their-cert.pem", missing_dir / "their-key.pem" };
	const auto resolved = Certificates::EnsureSelfSignedMaterial(operator_specified);
	BOOST_CHECK(!resolved.has_value());
}

BOOST_AUTO_TEST_CASE(EnsureSelfSignedMaterial_DefaultPaths_GeneratesThenReuses)
{
	// Exercise the default-path branch of EnsureSelfSignedMaterial: when the
	// configured cert/key are the built-in defaults and are absent, a per-install
	// pair is produced -- preferring the configured (writable) directory, else
	// falling back to a per-user private runtime directory. Either way a usable,
	// loadable pair must come back. Driving it through the public API also covers
	// the DirectoryIsWritable probe and the "reuse on second call" path.
	const Options::Web::SslCertificate defaults{
		fs::path(Application::DEFAULT_CERTIFICATE),
		fs::path(Application::DEFAULT_PRIVATE_KEY)
	};

	std::error_code ec;
	const bool cert_preexisted = fs::exists(defaults.certificate, ec);
	const bool key_preexisted = fs::exists(defaults.private_key, ec);

	const auto first = Certificates::EnsureSelfSignedMaterial(defaults);
	BOOST_REQUIRE_MESSAGE(first.has_value(),
		"EnsureSelfSignedMaterial must produce material for the built-in default paths");
	BOOST_REQUIRE(fs::exists(first->certificate, ec));
	BOOST_REQUIRE(fs::exists(first->private_key, ec));

	// Whatever pair came back must be loadable by the TLS stack.
	{
		boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
		boost::system::error_code load_ec;
		ctx.use_certificate_file(first->certificate.string(), boost::asio::ssl::context::pem, load_ec);
		BOOST_CHECK_MESSAGE(!load_ec, "generated certificate did not load: " << load_ec.message());
		ctx.use_private_key_file(first->private_key.string(), boost::asio::ssl::context::pem, load_ec);
		BOOST_CHECK_MESSAGE(!load_ec, "generated private key did not load: " << load_ec.message());
	}

	const std::string cert_after_first = ReadFile(first->certificate);
	const std::string key_after_first = ReadFile(first->private_key);

	// Second call must REUSE the freshly-persisted pair rather than regenerate it:
	// same resolved paths and byte-identical material.
	const auto second = Certificates::EnsureSelfSignedMaterial(defaults);
	BOOST_REQUIRE(second.has_value());
	BOOST_CHECK(second->certificate == first->certificate);
	BOOST_CHECK(second->private_key == first->private_key);
	BOOST_CHECK(ReadFile(second->certificate) == cert_after_first);
	BOOST_CHECK(ReadFile(second->private_key) == key_after_first);

	// Clean up ONLY material this test created, leaving any pre-existing install
	// material untouched.
	if (!cert_preexisted)
	{
		fs::remove(first->certificate, ec);
	}
	if (!key_preexisted)
	{
		fs::remove(first->private_key, ec);
	}
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// LoadSslCertificates: resolve + load TLS material into an ssl::context, or
// throw a typed certificate exception when the configured material is missing
// or unloadable.
//=============================================================================

namespace
{
	Options::Web::WebSettings EnabledHttpsSettings(const fs::path& cert, const fs::path& key)
	{
		Options::Web::WebSettings cfg;   // https_server_is_enabled defaults true
		cfg.ssl_certificate = Options::Web::SslCertificate{ cert, key };
		return cfg;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_LoadSslCertificates)

BOOST_AUTO_TEST_CASE(LoadSslCertificates_HttpsDisabled_IsNoOp)
{
	Options::Web::WebSettings cfg;
	cfg.https_server_is_enabled = false;

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);

	// Disabled HTTPS short-circuits before any certificate resolution.
	BOOST_CHECK_NO_THROW(Certificates::LoadSslCertificates(cfg, ctx));
}

BOOST_AUTO_TEST_CASE(LoadSslCertificates_OperatorMissingMaterial_ThrowsNotFound)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_loadssl_missing";
	std::error_code rm;
	fs::remove_all(dir, rm);

	// Non-default, non-existent paths: EnsureSelfSignedMaterial refuses to fabricate
	// material for operator-specified paths, so loading must fail loudly.
	auto cfg = EnabledHttpsSettings(dir / "cert.pem", dir / "key.pem");

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_THROW(Certificates::LoadSslCertificates(cfg, ctx), Exceptions::Certificate_NotFound);
}

BOOST_AUTO_TEST_CASE(LoadSslCertificates_ValidMaterial_LoadsSuccessfully)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_loadssl_valid";
	std::error_code rm;
	fs::remove_all(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	auto cfg = EnabledHttpsSettings(cert, key);   // no ca_chain_certificate

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_NO_THROW(Certificates::LoadSslCertificates(cfg, ctx));

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(LoadSslCertificates_InvalidPem_ThrowsInvalidFormat)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_loadssl_garbage";
	std::error_code rm;
	fs::remove_all(dir, rm);
	fs::create_directories(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	// Both files exist (so they are accepted as present) but are not valid PEM.
	{ std::ofstream(cert, std::ios::binary) << "not a certificate"; }
	{ std::ofstream(key, std::ios::binary) << "not a key"; }

	auto cfg = EnabledHttpsSettings(cert, key);

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_THROW(Certificates::LoadSslCertificates(cfg, ctx), Exceptions::Certificate_InvalidFormat);

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_CASE(LoadSslCertificates_MissingCaChain_ThrowsNotFound)
{
	const fs::path dir = fs::temp_directory_path() / "aqualink_loadssl_cachain";
	std::error_code rm;
	fs::remove_all(dir, rm);

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	auto cfg = EnabledHttpsSettings(cert, key);
	cfg.ca_chain_certificate = dir / "missing-ca-chain.pem";   // configured but absent

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_THROW(Certificates::LoadSslCertificates(cfg, ctx), Exceptions::Certificate_NotFound);

	fs::remove_all(dir, rm);
}

BOOST_AUTO_TEST_SUITE_END()

//=============================================================================
// Read-only-install fallback (Detail::GenerateOrReuseInSecureDirectories).
//
// This is the security-critical branch that backs EnsureSelfSignedMaterial when
// the built-in default cert/key directory is not writable (the common packaged
// case): it walks per-user PRIVATE runtime directories in order and, in the
// first one that PrepareSecureDirectory accepts, reuses or freshly generates a
// per-install self-signed pair under an owner-only "ssl" leaf. Driving the
// helper directly with controlled base directories exercises the generate /
// reuse / skip-rejected-candidate / exhausted paths without depending on the
// process's real runtime directories.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_CertificateSecureFallback)

BOOST_AUTO_TEST_CASE(SecureFallback_GeneratesFreshPairInFirstUsableDirectory)
{
	const fs::path base = fs::temp_directory_path() / "aqualink_certfb_gen";
	std::error_code rm;
	fs::remove_all(base, rm);
	fs::create_directories(base, rm);

	const auto resolved = Certificates::Detail::GenerateOrReuseInSecureDirectories({ base });
	BOOST_REQUIRE(resolved.has_value());

	// Material must land under the "ssl" leaf of the chosen base directory.
	const fs::path ssl_dir = base / "ssl";
	BOOST_CHECK(resolved->certificate == ssl_dir / "cert.pem");
	BOOST_CHECK(resolved->private_key == ssl_dir / "key.pem");
	BOOST_REQUIRE(fs::exists(resolved->certificate));
	BOOST_REQUIRE(fs::exists(resolved->private_key));

	// The freshly generated material must be loadable by the TLS stack.
	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	boost::system::error_code ec;
	ctx.use_certificate_file(resolved->certificate.string(), boost::asio::ssl::context::pem, ec);
	BOOST_CHECK_MESSAGE(!ec, "generated certificate did not load: " << ec.message());
	ctx.use_private_key_file(resolved->private_key.string(), boost::asio::ssl::context::pem, ec);
	BOOST_CHECK_MESSAGE(!ec, "generated private key did not load: " << ec.message());

#ifndef _WIN32
	// The private key must be owner-only (0600) on disk...
	const auto key_perms = fs::status(resolved->private_key).permissions();
	BOOST_CHECK((key_perms & fs::perms::group_read) == fs::perms::none);
	BOOST_CHECK((key_perms & fs::perms::others_read) == fs::perms::none);
	BOOST_CHECK((key_perms & fs::perms::others_write) == fs::perms::none);

	// ...and PrepareSecureDirectory must have restricted the containing dir to 0700.
	const auto dir_perms = fs::status(ssl_dir).permissions();
	BOOST_CHECK((dir_perms & fs::perms::group_all) == fs::perms::none);
	BOOST_CHECK((dir_perms & fs::perms::others_all) == fs::perms::none);
#endif

	fs::remove_all(base, rm);
}

BOOST_AUTO_TEST_CASE(SecureFallback_ReusesExistingPairAcrossCalls)
{
	const fs::path base = fs::temp_directory_path() / "aqualink_certfb_reuse";
	std::error_code rm;
	fs::remove_all(base, rm);
	fs::create_directories(base, rm);

	// First call generates the pair.
	const auto first = Certificates::Detail::GenerateOrReuseInSecureDirectories({ base });
	BOOST_REQUIRE(first.has_value());
	const std::string key_after_first = ReadFile(first->private_key);
	const std::string cert_after_first = ReadFile(first->certificate);
	BOOST_REQUIRE(!key_after_first.empty());

	// Second call must REUSE the existing pair -- same paths, byte-identical
	// material (i.e. it must not regenerate and clobber the persisted key).
	const auto second = Certificates::Detail::GenerateOrReuseInSecureDirectories({ base });
	BOOST_REQUIRE(second.has_value());
	BOOST_CHECK(second->certificate == first->certificate);
	BOOST_CHECK(second->private_key == first->private_key);
	BOOST_CHECK(ReadFile(second->private_key) == key_after_first);
	BOOST_CHECK(ReadFile(second->certificate) == cert_after_first);

	fs::remove_all(base, rm);
}

BOOST_AUTO_TEST_CASE(SecureFallback_SkipsRejectedCandidateAndUsesNext)
{
	const fs::path base = fs::temp_directory_path() / "aqualink_certfb_skip";
	std::error_code rm;
	fs::remove_all(base, rm);

	// First candidate: its "ssl" leaf already exists as a regular FILE, so
	// PrepareSecureDirectory rejects it (not a directory). Cross-platform -- no
	// reliance on POSIX-only symlink/ownership semantics.
	const fs::path rejected_base = base / "rejected";
	fs::create_directories(rejected_base, rm);
	{ std::ofstream(rejected_base / "ssl") << "occupied"; }
	BOOST_REQUIRE(fs::is_regular_file(rejected_base / "ssl"));

	// Second candidate: usable.
	const fs::path good_base = base / "good";
	fs::create_directories(good_base, rm);

	const auto resolved = Certificates::Detail::GenerateOrReuseInSecureDirectories({ rejected_base, good_base });
	BOOST_REQUIRE(resolved.has_value());

	// It must have fallen through to the second (usable) candidate.
	BOOST_CHECK(resolved->certificate == good_base / "ssl" / "cert.pem");
	BOOST_CHECK(resolved->private_key == good_base / "ssl" / "key.pem");
	BOOST_CHECK(fs::exists(resolved->certificate));
	BOOST_CHECK(fs::exists(resolved->private_key));

#ifndef _WIN32
	// On POSIX, additionally prove the symlink-redirect vector is rejected: a
	// symlink planted at the "ssl" leaf must be refused (lstat, not followed).
	const fs::path sym_base = base / "symlinked";
	fs::create_directories(sym_base, rm);
	std::error_code link_ec;
	fs::create_directory_symlink(good_base, sym_base / "ssl", link_ec);
	BOOST_REQUIRE_MESSAGE(!link_ec, "could not create test symlink: " << link_ec.message());

	const fs::path good_base2 = base / "good2";
	fs::create_directories(good_base2, rm);

	const auto resolved_sym = Certificates::Detail::GenerateOrReuseInSecureDirectories({ sym_base, good_base2 });
	BOOST_REQUIRE(resolved_sym.has_value());
	BOOST_CHECK(resolved_sym->private_key == good_base2 / "ssl" / "key.pem");
#endif

	fs::remove_all(base, rm);
}

BOOST_AUTO_TEST_CASE(SecureFallback_ReturnsNulloptWhenNoCandidateIsUsable)
{
	const fs::path base = fs::temp_directory_path() / "aqualink_certfb_none";
	std::error_code rm;
	fs::remove_all(base, rm);
	fs::create_directories(base, rm);

	// Every candidate's "ssl" leaf is an existing regular file, so none can be
	// secured as a directory. With no usable candidate the fallback yields nullopt
	// (the caller then surfaces the "could not be generated" error).
	const fs::path blocked_a = base / "a";
	const fs::path blocked_b = base / "b";
	for (const auto& b : { blocked_a, blocked_b })
	{
		fs::create_directories(b, rm);
		std::ofstream(b / "ssl") << "occupied";
	}

	const auto resolved = Certificates::Detail::GenerateOrReuseInSecureDirectories({ blocked_a, blocked_b });
	BOOST_CHECK(!resolved.has_value());

	fs::remove_all(base, rm);
}

BOOST_AUTO_TEST_SUITE_END()
