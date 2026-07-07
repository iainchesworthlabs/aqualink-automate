#pragma once

#include <concepts>
#include <string>

namespace AqualinkAutomate::Kernel
{

    template<typename TRAIT_TYPE>
    class TraitType
    {
    public:
        virtual ~TraitType() = default;

        using TraitKey = std::string;
        using TraitValue = TRAIT_TYPE;

    public:
        virtual TraitKey Name() const = 0;
        virtual bool IsMutable() const = 0;
    };

    /// Concept constraining types usable as trait descriptors (the Traits store key/value and
    /// the device-graph trait filters). Models the TraitType<> contract.
    template<typename T>
    concept IsTraitType = requires(T t) {
        typename T::TraitValue;
        typename T::TraitKey;
        { t.Name() } -> std::convertible_to<std::string>;
        { t.IsMutable() } -> std::same_as<bool>;
    };

    template<typename TRAIT_TYPE>
    class ImmutableTraitType : public TraitType<TRAIT_TYPE>
    {
    public:
        bool IsMutable() const final
        {
            return false;
        }
    };

    template<typename TRAIT_TYPE>
    class MutableTraitType : public TraitType<TRAIT_TYPE>
    {
    public:
        bool IsMutable() const final
        {
            return true;
        }
    };

}
// namespace AqualinkAutomate::Kernel
