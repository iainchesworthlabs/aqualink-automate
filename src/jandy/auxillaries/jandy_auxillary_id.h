#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <boost/uuid/uuid.hpp>
#include <magic_enum/magic_enum.hpp>

namespace AqualinkAutomate::Auxillaries
{

	enum class JandyAuxillaryIds
	{
		Aux_1 = 0x01, 
		Aux_2 = 0x02, 
		Aux_3 = 0x03, 
		Aux_4 = 0x04, 
		Aux_5 = 0x05, 
		Aux_6 = 0x06, 
		Aux_7 = 0x07,

		Aux_B1 = 0x08, 
		Aux_B2 = 0x09,
		Aux_B3 = 0x0A, 
		Aux_B4 = 0x0B, 
		Aux_B5 = 0x0C, 
		Aux_B6 = 0x0D, 
		Aux_B7 = 0x0E, 
		Aux_B8 = 0x0F,

		Aux_C1 = 0x10, 
		Aux_C2 = 0x11, 
		Aux_C3 = 0x12, 
		Aux_C4 = 0x13, 
		Aux_C5 = 0x14, 
		Aux_C6 = 0x15, 
		Aux_C7 = 0x16, 
		Aux_C8 = 0x17,

		Aux_D1 = 0x18, 
		Aux_D2 = 0x19, 
		Aux_D3 = 0x1A, 
		Aux_D4 = 0x1B, 
		Aux_D5 = 0x1C, 
		Aux_D6 = 0x1D, 
		Aux_D7 = 0x1E, 
		Aux_D8 = 0x1F,

		ExtraAux = 0x00
	};

	// Parse a human / screen-scraped aux-id label into the canonical enum, tolerant of the
	// no-space vs single-space bank forms and interior-whitespace scraping artefacts:
	// "Aux5", "Aux 5", "AuxB1", "Aux B1", "Aux  B1", "Extra Aux", "ExtraAux" all resolve.
	// Returns nullopt for non-aux strings ("Pool Light", "pool light", "Aux11", "Aux E1").
	std::optional<JandyAuxillaryIds> ParseAuxId(std::string_view label);

	// The protocol-native key ("jandy:aux:<n>") and the deterministic stable device UUID
	// derived from it. Keyed on the enum's INTEGER value so every spelling of an aux maps to
	// ONE identity. Used by the factory (at device creation) and by every reconciliation site
	// so a cache-restored device and a live-discovered device for the same aux share identity.
	std::string AuxNativeKey(JandyAuxillaryIds id);
	boost::uuids::uuid AuxStableId(JandyAuxillaryIds id);

}
// namespace AqualinkAutomate::Auxillaries

namespace magic_enum::customize
{
	
	using AqualinkAutomate::Auxillaries::JandyAuxillaryIds;

	//
	// Specialisation of magic_enum::enum_name for AqualinkAutomate::Auxillaries::JandyAuxillaryIds
	//

	template<>
	constexpr customize_t enum_name<JandyAuxillaryIds>(JandyAuxillaryIds value) noexcept
	{
		using enum AqualinkAutomate::Auxillaries::JandyAuxillaryIds;

		switch (value)
		{
		case Aux_1:
			return "Aux1";
		case Aux_2:
			return "Aux2";
		case Aux_3:
			return "Aux3";
		case Aux_4:
			return "Aux4";
		case Aux_5:
			return "Aux5";
		case Aux_6:
			return "Aux6";
		case Aux_7:
			return "Aux7";
		case Aux_B1:
			return "Aux B1";
		case Aux_B2:
			return "Aux B2";
		case Aux_B3:
			return "Aux B3";
		case Aux_B4:
			return "Aux B4";
		case Aux_B5:
			return "Aux B5";
		case Aux_B6:
			return "Aux B6";
		case Aux_B7:
			return "Aux B7";
		case Aux_B8:
			return "Aux B8";
		case Aux_C1:
			return "Aux C1";
		case Aux_C2:
			return "Aux C2";
		case Aux_C3:
			return "Aux C3";
		case Aux_C4:
			return "Aux C4";
		case Aux_C5:
			return "Aux C5";
		case Aux_C6:
			return "Aux C6";
		case Aux_C7:
			return "Aux C7";
		case Aux_C8:
			return "Aux C8";
		case Aux_D1:
			return "Aux D1";
		case Aux_D2:
			return "Aux D2";
		case Aux_D3:
			return "Aux D3";
		case Aux_D4:
			return "Aux D4";
		case Aux_D5:
			return "Aux D5";
		case Aux_D6:
			return "Aux D6";
		case Aux_D7:
			return "Aux D7";
		case Aux_D8:
			return "Aux D8";
		case ExtraAux:
			return "Extra Aux";
		}

		return default_tag;
	}

}
// magic_enum::customize
