#pragma once

#include <filesystem>
#include <string_view>

#include <boost/url/segments_view.hpp>
#include <boost/url/urls.hpp>
#include <boost/url/url_view.hpp>

namespace AqualinkAutomate::HTTP
{

	class StaticFileHandler
	{
	public:
		StaticFileHandler(std::string_view prefix, const std::filesystem::path& root);
		StaticFileHandler(boost::urls::url prefix, const std::filesystem::path& root);

		bool match(const boost::urls::url_view& target, std::filesystem::path& result);

	private:
		int match_prefix(boost::urls::segments_view target, boost::urls::segments_view prefix);

	private:
		boost::urls::url m_Prefix;
		std::filesystem::path m_DocRoot;
	};

}
// namespace AqualinkAutomate::HTTP
