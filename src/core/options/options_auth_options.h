#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Auth
{

	/// Identity-system (authN/authZ) settings — docs/auth-redesign.md §10.
	typedef struct tagAuthSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Authentication" };
			return AREA_NAME;
		}

		tagAuthSettings() = default;

		/// Global posture: false (the default, "disabled") preserves historical
		/// behaviour — no identity resolution, every PDP decision is Permit.
		/// True ("enabled") resolves every request to a Subject and gates routes
		/// through the PolicyEngine (anonymous == the Guest group).
		bool auth_mode_enabled{ false };

		/// Directory for the auth state (JWT signing keys; users/groups/keys
		/// stores in later slices).  EMPTY resolves the platform's secure state
		/// directory at startup (SecureRuntimeStateDirectories).
		std::string auth_state_dir;

		/// Access-token lifetime in minutes (short-lived by design; refresh
		/// tokens arrive with the session flows in Slice 2).
		std::uint32_t jwt_access_ttl_minutes{ 15 };
	}
	AuthSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_AUTH_MODE{ make_appoption("auth-mode", "identity system posture: 'disabled' (default; historical behaviour) or 'enabled' (login + entitlement enforcement)", boost::program_options::value<std::string>()->default_value("disabled")) };
		AppOptionPtr OPTION_AUTH_STATE_DIR{ make_appoption("auth-state-dir", "directory for authentication state (empty uses the platform's secure state directory)", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_JWT_ACCESS_TTL{ make_appoption("jwt-access-ttl", "access-token lifetime in minutes", boost::program_options::value<std::uint32_t>()->default_value(15)) };

		const std::vector<AppOptionPtr> AuthOptionsCollection
		{
			OPTION_AUTH_MODE,
			OPTION_AUTH_STATE_DIR,
			OPTION_JWT_ACCESS_TTL
		};

	public:
		using SettingsType = AuthSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Auth
