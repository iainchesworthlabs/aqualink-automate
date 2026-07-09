#include <format>

#include <boost/regex.hpp>

#include "logging/logging.h"
#include "utility/string_conversion/auxillary_state_string_converter.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{
	const std::string AuxillaryStateStringConverter::REGEX_PATTERN{ R"((.{1,13})\s+(ON|OFF|ENA|\*\*\*))" };
	const boost::regex AuxillaryStateStringConverter::REGEX_PARSER{ REGEX_PATTERN };

	AuxillaryStateStringConverter::AuxillaryStateStringConverter() noexcept :
		m_ErrorOccurred(ErrorCodes::StringConversion_ErrorCodes::MalformedInput)
	{
	}

	AuxillaryStateStringConverter::AuxillaryStateStringConverter(const std::string& auxillary_status_string) noexcept :
		m_State(Kernel::AuxillaryStatuses::Unknown),
		m_ErrorOccurred(std::nullopt)
	{
		ConvertStringToStatus(auxillary_status_string);
	}

	AuxillaryStateStringConverter::AuxillaryStateStringConverter(const AuxillaryStateStringConverter& other) noexcept = default;

	AuxillaryStateStringConverter::AuxillaryStateStringConverter(AuxillaryStateStringConverter&& other) noexcept :
		m_Label(std::move(other.m_Label)),
		m_State(std::move(other.m_State)),
		m_ErrorOccurred(std::move(other.m_ErrorOccurred))
	{
	}

	AuxillaryStateStringConverter& AuxillaryStateStringConverter::operator=(const AuxillaryStateStringConverter& other) noexcept
	{
		if (this != &other)
		{
			m_Label = other.m_Label;
			m_State = other.m_State;
			m_ErrorOccurred = other.m_ErrorOccurred;
		}

		return *this;
	}

	AuxillaryStateStringConverter& AuxillaryStateStringConverter::operator=(AuxillaryStateStringConverter&& other) noexcept
	{
		if (this != &other)
		{
			m_Label = std::move(other.m_Label);
			m_State = std::move(other.m_State);
			m_ErrorOccurred = std::move(other.m_ErrorOccurred);
		}

		return *this;
	}

	AuxillaryStateStringConverter& AuxillaryStateStringConverter::operator=(const std::string& auxillary_status_string) noexcept
	{
		ConvertStringToStatus(auxillary_status_string);
		return *this;
	}

	std::expected<std::string, boost::system::error_code> AuxillaryStateStringConverter::Label() const noexcept
	{
		if (m_ErrorOccurred.has_value())
		{
			return std::unexpected<boost::system::error_code>(make_error_code(m_ErrorOccurred.value()));
		}

		return m_Label;
	}

	std::expected<Kernel::AuxillaryStatuses, boost::system::error_code> AuxillaryStateStringConverter::State() const noexcept
	{
		if (m_ErrorOccurred.has_value())
		{
			return std::unexpected<boost::system::error_code>(make_error_code(m_ErrorOccurred.value()));
		}

		return m_State;
	}

	void AuxillaryStateStringConverter::ConvertStringToStatus(const std::string& auxillary_status_string) noexcept
	{
		m_ErrorOccurred = std::nullopt;

		const auto auxillary_status_data = ValidateAndExtractData(auxillary_status_string);

		const auto name = std::get<0>(auxillary_status_data);
		const auto status = std::get<1>(auxillary_status_data);

		if (name && status)
		{
			m_Label = TrimWhitespace(name.value());

			if ("ON" == status.value())
			{
				m_State = Kernel::AuxillaryStatuses::On;
			}
			else if ("OFF" == status.value())
			{
				m_State = Kernel::AuxillaryStatuses::Off;
			}
			else if ("ENA" == status.value())
			{
				m_State = Kernel::AuxillaryStatuses::Enabled;
			}
			else if ("***" == status.value())
			{
				m_State = Kernel::AuxillaryStatuses::Pending;
			}
			else
			{
				LogDebug(Channel::Devices, std::format("Failed to convert equipment status; could not convert state value -> {}", status.value()));
				m_ErrorOccurred = ErrorCodes::StringConversion_ErrorCodes::MalformedInput;
			}
		}
		else
		{
			LogDebug(Channel::Devices, std::format("Failed to convert equipment status; malformed input -> {}", auxillary_status_string));
			m_ErrorOccurred = ErrorCodes::StringConversion_ErrorCodes::MalformedInput;
		}
	}

	std::tuple<std::optional<std::string>, std::optional<std::string>> AuxillaryStateStringConverter::ValidateAndExtractData(const std::string& auxillary_status_string) noexcept
	{
		boost::smatch match_results;

		if (MINIMUM_STRING_LENGTH > auxillary_status_string.size() || MAXIMUM_STRING_LENGTH < auxillary_status_string.size() ||
			!boost::regex_search(auxillary_status_string, match_results, REGEX_PARSER) ||
			2 > match_results.size())
		{
			// Invalid string length, pattern match, or insufficient resultset...do nothing.
			return std::make_tuple(std::nullopt, std::nullopt);
		}

		return std::make_tuple<>(
			std::optional<std::string>(match_results[1]),
			std::optional<std::string>(match_results[2])
		);
	}

}
// namespace AqualinkAutomate::Utility
