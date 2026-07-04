#include <string>

#include <boost/test/unit_test.hpp>

#include "http/server/responses/html_escape.h"

using namespace AqualinkAutomate::HTTP::Responses;

// =============================================================================
// HtmlEscape - reflected-XSS mitigation for wire-/client-controlled values that
// are interpolated into HTML error bodies. Every one of the five HTML-significant
// characters must be replaced with its entity so a crafted request target cannot
// break out of the body and inject markup.
// =============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_HtmlEscape)

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_Empty)
{
	BOOST_CHECK_EQUAL(HtmlEscape(""), "");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_NoSpecialCharacters)
{
	BOOST_CHECK_EQUAL(HtmlEscape("/api/equipment"), "/api/equipment");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_Ampersand)
{
	BOOST_CHECK_EQUAL(HtmlEscape("a&b"), "a&amp;b");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_LessThan)
{
	BOOST_CHECK_EQUAL(HtmlEscape("a<b"), "a&lt;b");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_GreaterThan)
{
	BOOST_CHECK_EQUAL(HtmlEscape("a>b"), "a&gt;b");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_DoubleQuote)
{
	BOOST_CHECK_EQUAL(HtmlEscape("a\"b"), "a&quot;b");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_SingleQuote)
{
	BOOST_CHECK_EQUAL(HtmlEscape("a'b"), "a&#39;b");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_AllTogether)
{
	// Every significant character in one string, in order, plus benign text.
	BOOST_CHECK_EQUAL(HtmlEscape("&<>\"'"), "&amp;&lt;&gt;&quot;&#39;");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_ScriptInjectionAttempt)
{
	// A representative reflected-XSS payload must be fully neutralised.
	BOOST_CHECK_EQUAL(
		HtmlEscape("<script>alert('x')</script>"),
		"&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;");
}

BOOST_AUTO_TEST_CASE(Test_HtmlEscape_AmpersandEscapedOnlyOnce)
{
	// The ampersand arm must not re-escape the entities it introduces.
	BOOST_CHECK_EQUAL(HtmlEscape("&amp;"), "&amp;amp;");
}

BOOST_AUTO_TEST_SUITE_END()
