#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "profiling/types/profiling_types.h"

namespace AqualinkAutomate::Factory
{
    
    typedef Types::ProfilerTypePtr(*ProfilerInstanceGenerator)();

	class ProfilerFactory
	{
    public:
        ProfilerFactory();
        ~ProfilerFactory() = default;

        ProfilerFactory(const ProfilerFactory&) = delete;
        ProfilerFactory& operator=(const ProfilerFactory&) = delete;
        ProfilerFactory(ProfilerFactory&&) = delete;
        ProfilerFactory& operator=(ProfilerFactory&&) = delete;

        static ProfilerFactory& Instance();

        bool Register(Types::ProfilerTypes type, Types::ProfilerTypePtr&& instance_ptr);
        void SetDesired(Types::ProfilerTypes type);
        Types::ProfilerTypePtr Get();

        // Introspection for the runtime control surface / startup diagnostics:
        // which backends were compiled in & registered, and which (if any) was
        // requested via --profiler. A requested-but-not-registered backend means
        // Get() falls back to NoOp (see the startup warning in main).
        std::vector<Types::ProfilerTypes> RegisteredTypes() const;
        bool IsRegistered(Types::ProfilerTypes type) const;
        std::optional<Types::ProfilerTypes> Selected() const;

    private:
        std::unordered_map<Types::ProfilerTypes, Types::ProfilerTypePtr> m_Profilers{};
        std::optional<Types::ProfilerTypes> m_DesiredProfiler{ std::nullopt };

	};

}
// namespace AqualinkAutomate::Factory
