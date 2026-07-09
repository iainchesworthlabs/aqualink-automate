#pragma once

#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "profiling/profiling_units/unit_colours.h"
#include "profiling/types/profiling_types.h"

namespace AqualinkAutomate::Factory
{
    using ProfilingUnitGeneratorFunc = std::function<Types::ProfilingUnitTypePtr(std::string_view, const std::source_location&, Profiling::UnitColours)>;
    using ProfilingUnitGenerators = std::tuple<ProfilingUnitGeneratorFunc, ProfilingUnitGeneratorFunc, ProfilingUnitGeneratorFunc>;

	class ProfilingUnitFactory
	{
    public:
        ProfilingUnitFactory();
        ~ProfilingUnitFactory() = default;

        ProfilingUnitFactory(const ProfilingUnitFactory&) = delete;
        ProfilingUnitFactory& operator=(const ProfilingUnitFactory&) = delete;
        ProfilingUnitFactory(ProfilingUnitFactory&&) = delete;
        ProfilingUnitFactory& operator=(ProfilingUnitFactory&&) = delete;

        static ProfilingUnitFactory& Instance();

        bool Register(const Types::ProfilerTypes type, ProfilingUnitGenerators&& generators);
        void SetDesired(Types::ProfilerTypes type);

        Types::ProfilingUnitTypePtr CreateDomain(std::string_view name, const std::source_location& src_loc = std::source_location::current(), Profiling::UnitColours colour = Profiling::UnitColours::NotSpecified);
        Types::ProfilingUnitTypePtr CreateFrame(std::string_view name, const std::source_location& src_loc = std::source_location::current(), Profiling::UnitColours colour = Profiling::UnitColours::NotSpecified);
        Types::ProfilingUnitTypePtr CreateZone(std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour = Profiling::UnitColours::NotSpecified);

    private:
        const ProfilingUnitGenerators& Get();

    private:
        std::unordered_map<Types::ProfilerTypes, const ProfilingUnitGenerators> m_Generators{};
        std::optional<Types::ProfilerTypes> m_DesiredProfiler{ std::nullopt };
	};

}
// namespace AqualinkAutomate::Factory
