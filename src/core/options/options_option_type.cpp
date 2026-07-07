#include <functional>
#include <map>
#include <vector>

#include "options/options_option_type.h"

namespace AqualinkAutomate::Options
{
	namespace
	{
		std::map<std::string, OptionMetadata, std::less<>>& MetadataRegistry()
		{
			static std::map<std::string, OptionMetadata, std::less<>> registry;
			return registry;
		}
	}

	AppOption::AppOption(const std::string& long_name, const std::string& description) :
		AppOption(long_name, description, new boost::program_options::untyped_value(true))
	{
	}

	AppOption::AppOption(const std::string& long_name, const std::string& description, const boost::program_options::value_semantic* s) :
		boost::program_options::option_description(long_name.c_str(), s, description.c_str()),
		m_LongName(long_name),
		m_Description(description)
	{
		MetadataRegistry()[m_LongName] = OptionMetadata{ m_LongName, m_ShortName, m_Description };
	}

	AppOption::AppOption(const std::string& long_name, const std::string& short_name, const std::string& description) :
		AppOption(long_name, short_name, description, new boost::program_options::untyped_value(true))
	{
	}

	AppOption::AppOption(const std::string& long_name, const std::string& short_name, const std::string& description, const boost::program_options::value_semantic* s) :
		boost::program_options::option_description((long_name + SEPERATOR + short_name).c_str(), s, description.c_str()),
		m_LongName(long_name),
		m_ShortName(short_name),
		m_Description(description)
	{
		MetadataRegistry()[m_LongName] = OptionMetadata{ m_LongName, m_ShortName, m_Description };
	}

	boost::shared_ptr<boost::program_options::option_description> AppOption::operator()()
	{
		return this->shared_from_this();
	}

	bool AppOption::IsPresent(boost::program_options::variables_map& vm) const
	{
		return (0 < vm.count(m_LongName));
	}

	bool AppOption::IsPresentAndNotJustDefaulted(boost::program_options::variables_map& vm) const
	{
		return (IsPresent(vm) && !(vm[m_LongName].defaulted()));
	}

	std::vector<OptionMetadata> AppOption::RegisteredOptions()
	{
		std::vector<OptionMetadata> result;
		result.reserve(MetadataRegistry().size());
		for (const auto& [key, meta] : MetadataRegistry())
		{
			result.push_back(meta);
		}
		return result;
	}

}
// namespace AqualinkAutomate::Options
