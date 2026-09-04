#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>

#include "application/application_defaults.h"
#include "certificates/certificate_management.h"
#include "exceptions/exception_certificate_invalidformat.h"
#include "exceptions/exception_certificate_notfound.h"
#include "options/options_web_options.h"

namespace fs = std::filesystem;
using namespace AqualinkAutomate;

//=============================================================================
// Remaining decision arms of the certificate subsystem:
//
//   * the "using the built-in defaults" test is an AND of both paths - a
//     half-default configuration is operator-specified and must never have
//     material fabricated for it;
//   * LoadSslCertificates must distinguish WHICH file failed to load (a good
//     certificate with a bad key, and a good pair with a bad CA chain);
//   * the secure-directory fallback with nothing to try.
//=============================================================================

namespace
{

	Options::Web::WebSettings EnabledHttpsSettings(const fs::path& cert, const fs::path& key)
	{
		Options::Web::WebSettings cfg;   // https_server_is_enabled defaults true
		cfg.ssl_certificate = Options::Web::SslCertificate{ cert, key };
		return cfg;
	}

	fs::path FreshDir(std::string_view name)
	{
		const fs::path dir = fs::temp_directory_path() / fs::path{ std::string{ name } };
		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::create_directories(dir, ec);
		return dir;
	}

	void WriteGarbage(const fs::path& p, std::string_view contents)
	{
		std::ofstream out(p, std::ios::binary);
		out << contents;
	}

}
// unnamed namespace

BOOST_AUTO_TEST_SUITE(TestSuite_CertificateManagementBranches)

//-----------------------------------------------------------------------------
// "using_defaults" is an AND over BOTH configured paths
//-----------------------------------------------------------------------------

// Half-default configurations are operator-specified: mixing the built-in
// certificate path with a custom key path (or vice versa) must NOT unlock the
// self-signed generator, or an operator's typo would be papered over with a
// fabricated certificate.
BOOST_AUTO_TEST_CASE(CertMgmtBranches_HalfDefaultPathsNeverGenerate)
{
	const fs::path dir = FreshDir("aqualink_certmgmt_halfdefault");

	{
		// Default certificate path, custom (absent) key path.
		Options::Web::SslCertificate mixed{ fs::path(Application::DEFAULT_CERTIFICATE), dir / "their-key.pem" };
		BOOST_CHECK(!Certificates::EnsureSelfSignedMaterial(mixed).has_value());
		BOOST_CHECK(!fs::exists(dir / "their-key.pem"));
	}

	{
		// Custom (absent) certificate path, default key path.
		Options::Web::SslCertificate mixed{ dir / "their-cert.pem", fs::path(Application::DEFAULT_PRIVATE_KEY) };
		BOOST_CHECK(!Certificates::EnsureSelfSignedMaterial(mixed).has_value());
		BOOST_CHECK(!fs::exists(dir / "their-cert.pem"));
	}

	std::error_code ec;
	fs::remove_all(dir, ec);
}

//-----------------------------------------------------------------------------
// LoadSslCertificates: which half is broken
//-----------------------------------------------------------------------------

// The certificate loads but the private key does not: the failure must still be
// reported as an invalid-format certificate error rather than being swallowed
// (a context left with a certificate and no key would fail every handshake).
BOOST_AUTO_TEST_CASE(CertMgmtBranches_ValidCertificateWithBrokenKeyThrows)
{
	const fs::path dir = FreshDir("aqualink_certmgmt_badkey");

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	// Keep the good certificate; replace ONLY the key with something unparseable.
	WriteGarbage(key, "-----BEGIN PRIVATE KEY-----\nnot base64 at all\n-----END PRIVATE KEY-----\n");

	auto cfg = EnabledHttpsSettings(cert, key);

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_THROW(Certificates::LoadSslCertificates(cfg, ctx), Exceptions::Certificate_InvalidFormat);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

// A CA chain that is present but not valid PEM fails the chain load (distinct
// from the "configured but absent" case, which is a NotFound).
BOOST_AUTO_TEST_CASE(CertMgmtBranches_PresentButUnparseableCaChainThrows)
{
	const fs::path dir = FreshDir("aqualink_certmgmt_badchain");

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";
	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	const fs::path chain = dir / "chain.pem";
	WriteGarbage(chain, "definitely not a certificate chain");

	auto cfg = EnabledHttpsSettings(cert, key);
	cfg.ca_chain_certificate = chain;

	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	BOOST_CHECK_THROW(Certificates::LoadSslCertificates(cfg, ctx), Exceptions::Certificate_InvalidFormat);

	std::error_code ec;
	fs::remove_all(dir, ec);
}

//-----------------------------------------------------------------------------
// Secure-directory fallback
//-----------------------------------------------------------------------------

// No candidate directories at all: the fallback has nothing to secure and must
// answer "no material" rather than inventing a location.
BOOST_AUTO_TEST_CASE(CertMgmtBranches_SecureFallbackWithNoCandidates)
{
	BOOST_CHECK(!Certificates::Detail::GenerateOrReuseInSecureDirectories({}).has_value());
}

// Regeneration over an existing pair replaces BOTH files with fresh material:
// a re-run must not leave a certificate paired with the previous key.
BOOST_AUTO_TEST_CASE(CertMgmtBranches_RegenerationReplacesBothHalves)
{
	const fs::path dir = FreshDir("aqualink_certmgmt_regen");

	const fs::path cert = dir / "cert.pem";
	const fs::path key = dir / "key.pem";

	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	const auto read = [](const fs::path& p)
		{
			std::ifstream in(p, std::ios::binary);
			return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		};

	const std::string first_cert = read(cert);
	const std::string first_key = read(key);
	BOOST_REQUIRE(!first_cert.empty());
	BOOST_REQUIRE(!first_key.empty());

	BOOST_REQUIRE(Certificates::GenerateSelfSignedCertificate(cert, key));

	BOOST_CHECK(read(cert) != first_cert);
	BOOST_CHECK(read(key) != first_key);

	// ...and the regenerated pair is still loadable together.
	boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
	boost::system::error_code load_ec;
	ctx.use_certificate_file(cert.string(), boost::asio::ssl::context::pem, load_ec);
	BOOST_CHECK_MESSAGE(!load_ec, "regenerated certificate did not load: " << load_ec.message());
	ctx.use_private_key_file(key.string(), boost::asio::ssl::context::pem, load_ec);
	BOOST_CHECK_MESSAGE(!load_ec, "regenerated private key did not load: " << load_ec.message());

	std::error_code ec;
	fs::remove_all(dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()
