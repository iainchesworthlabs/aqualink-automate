#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <tuple>

#include <boost/regex.hpp>
#include <boost/system/error_code.hpp>

#include "errors/string_conversion_errors.h"
#include "kernel/orp.h"
#include "kernel/ph.h"

using namespace AqualinkAutomate::ErrorCodes;

namespace AqualinkAutomate::Utility
{

	class ChemistryStringConverter
	{
		static const std::string REGEX_PATTERN;
		static const boost::regex REGEX_PARSER;

		static const uint8_t MAXIMUM_STRING_LENGTH = 14;
		static const uint8_t MINIMUM_STRING_LENGTH = 14;

	public:
		ChemistryStringConverter() noexcept;
		explicit ChemistryStringConverter(const std::string& chemistry_string) noexcept;
		ChemistryStringConverter(const ChemistryStringConverter& other) noexcept;
		ChemistryStringConverter(ChemistryStringConverter&& other) noexcept;

		ChemistryStringConverter& operator=(const ChemistryStringConverter& other) noexcept;
		ChemistryStringConverter& operator=(ChemistryStringConverter&& other) noexcept;
		ChemistryStringConverter& operator=(const std::string& chemistry_string) noexcept;

		std::expected<Kernel::ORP, boost::system::error_code> ORP() const noexcept;
		std::expected<Kernel::pH, boost::system::error_code> PH() const noexcept;

	private:
		void ConvertStringToChemistry(const std::string& chemistry_string) noexcept;
		std::tuple<std::optional<std::string>, std::optional<std::string>> ValidateAndExtractData(const std::string& chemistry_string) noexcept;

	private:
		Kernel::ORP m_ORP{ 0 };
		Kernel::pH m_PH{ 0.0f };

	private:
		std::optional<ErrorCodes::StringConversion_ErrorCodes> m_ErrorOccurred;
	};

}
// namespace AqualinkAutomate::Utility
