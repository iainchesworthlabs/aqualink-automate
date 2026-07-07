#pragma once

#include <any>
#include <format>

#include "exceptions/exception_traits_invalidtraitvalue.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{

    template<typename TRAITS, typename TRAIT_TYPE>
    class TraitValueProxy
    {
    public:
        using ValueType = typename TRAIT_TYPE::TraitValue;
        using Reference = ValueType&;
        using Pointer = ValueType*;

    public:
        TraitValueProxy(TRAITS& traits, std::any& trait_valueany) :
            m_TraitsRef(traits),
            m_ValueAny(trait_valueany)
        {
        }

        TraitValueProxy& operator=(TRAIT_TYPE::TraitValue trait_value)
        {
            m_TraitsRef.Set(TRAIT_TYPE{}, trait_value);
            return *this;
        }

        operator auto() const
        {
            return CastOrThrow<ValueType>(m_ValueAny);
        }

        Reference operator*()
        {
            return CastOrThrow<Reference>(m_ValueAny);
        }

        Pointer operator->()
        {
            return CastOrThrow<Pointer>(m_ValueAny);
        }

    private:
        // Single point for the any_cast + diagnostic. The std::any holding the trait value is
        // guaranteed to exist (the proxy is only constructed once Traits::Get has confirmed the
        // trait is present), so the only failure mode is a value-type mismatch (bad_any_cast).
        template<typename CAST_TYPE>
        static CAST_TYPE CastOrThrow(std::any& value_any)
        {
            try
            {
                return std::any_cast<CAST_TYPE>(value_any);
            }
            catch (const std::bad_any_cast&)
            {
                LogDebug(Channel::Devices, [] { return std::format("Attempted to retrieve invalid trait value: type -> {}", TRAIT_TYPE{}.Name()); });
                throw Exceptions::Traits_InvalidTraitValue();
            }
        }

    private:
        TRAITS& m_TraitsRef;
        std::any& m_ValueAny;
    };

    template<typename TRAITS, typename TRAIT_TYPE>
    class ConstTraitValueProxy
    {
    public:
        using ValueType = TRAIT_TYPE::TraitValue;
        using Reference = const ValueType&;
        using Pointer = const ValueType*;

    public:
        ConstTraitValueProxy(const TRAITS& traits, const std::any& trait_valueany) :
            m_TraitsRef(traits),
            m_ValueAny(trait_valueany)
        {
        }

        operator auto() const
        {
            return CastOrThrow<ValueType>(m_ValueAny);
        }

        Reference operator*() const
        {
            return CastOrThrow<Reference>(m_ValueAny);
        }

        Pointer operator->() const
        {
            return &(operator*());
        }

    private:
        // See TraitValueProxy::CastOrThrow: the underlying std::any is always present, so a
        // bad_any_cast is the only reachable failure (a value-type mismatch).
        template<typename CAST_TYPE>
        static CAST_TYPE CastOrThrow(const std::any& value_any)
        {
            try
            {
                return std::any_cast<CAST_TYPE>(value_any);
            }
            catch (const std::bad_any_cast&)
            {
                LogDebug(Channel::Devices, [] { return std::format("Attempted to retrieve invalid trait value: type -> {}", TRAIT_TYPE{}.Name()); });
                throw Exceptions::Traits_InvalidTraitValue();
            }
        }

    private:
        const TRAITS& m_TraitsRef;
        const std::any& m_ValueAny;
    };

}
// namespace AqualinkAutomate::Kernel
