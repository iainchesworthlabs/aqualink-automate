#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <boost/enable_shared_from_this.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>

#include "exceptions/exception_developer_optionkeyhasnovalue.h"
#include "exceptions/exception_developer_optionkeyinvalidtype.h"
#include "exceptions/exception_developer_optionkeynotprovided.h"

namespace AqualinkAutomate::Options
{
	// Snapshot of an option's user-facing metadata, captured at construction so it can be
	// introspected (e.g. by the /api/diagnostics/options route) after parsing has finished.
	struct OptionMetadata
	{
		std::string long_name;
		std::string short_name;
		std::string description;
	};

	class AppOption : public boost::program_options::option_description, public boost::enable_shared_from_this<AppOption>
	{
		inline static const std::string SEPERATOR{ "," };

	private:
		explicit AppOption(const std::string& long_name, const std::string& description);
		explicit AppOption(const std::string& long_name, const std::string& description, const boost::program_options::value_semantic* s);
		explicit AppOption(const std::string& long_name, const std::string& short_name, const std::string& description);
		explicit AppOption(const std::string& long_name, const std::string& short_name, const std::string& description, const boost::program_options::value_semantic* s);

		AppOption(const AppOption&) = delete;
		AppOption(AppOption&&) = delete;

	public:
		boost::shared_ptr<boost::program_options::option_description> operator()();

		bool IsPresent(boost::program_options::variables_map& vm) const;
		bool IsPresentAndNotJustDefaulted(boost::program_options::variables_map& vm) const;

		const std::string& LongName() const { return m_LongName; }
		const std::string& ShortName() const { return m_ShortName; }
		const std::string& Description() const { return m_Description; }

		// Metadata for every AppOption constructed so far, one entry per long name, ordered
		// by long name. Populated as options are constructed during option parsing.
		static std::vector<OptionMetadata> RegisteredOptions();

		template<typename T>
		const T& As(boost::program_options::variables_map& vm) const
		{
			const std::string& key = m_LongName;

			if (const auto it = vm.find(key); it == vm.end())
			{
				throw Exceptions::Developer_OptionKeyNotProvided(std::format("Option {} was not provided.", key));
			}
			else if (const auto& vv = it->second; vv.empty())
			{
				throw Exceptions::Developer_OptionKeyHasNoValue(std::format("Option {} is present but has no value (empty).", key));
			}
			else
			{
				try
				{
					return vv.as<T>();
				}
				catch (const boost::bad_any_cast& /* ignored */)
				{
					throw Exceptions::Developer_OptionKeyInvalidType(std::format("Type mismatch for option {}; requested type: {}", key, typeid(T).name()));
				}
			}
		}

	private:
		const std::string m_LongName;
		const std::string m_ShortName;
		const std::string m_Description;

	private:
		template<typename...Args>
		friend auto make_appoption(Args&&... args);
	};

	typedef boost::shared_ptr<AppOption> AppOptionPtr;

	template<typename...Args>
	auto make_appoption(Args&&... args)
	{
		auto ao = new AppOption(std::forward<Args>(args)...);
		return AppOptionPtr(ao);
	};
 }
// namespace AqualinkAutomate::Options
