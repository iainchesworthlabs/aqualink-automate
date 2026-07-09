#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

using namespace AqualinkAutomate::Options;

namespace AqualinkAutomate::Pentair::Options
{

	typedef struct tagPentairSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Pentair" };
			return AREA_NAME;
		}

		tagPentairSettings()
		{
		}
	}
	PentairSettings;

	class OptionsProcessor
	{
	private:
		const std::vector<AppOptionPtr> PentairOptionsCollection
		{
		};

	public:
		using SettingsType = PentairSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Pentair::Options
