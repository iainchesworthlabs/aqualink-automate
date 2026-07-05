#pragma once

#include <any>
#include <expected>
#include <format>
#include <optional>
#include <unordered_map>

#include <boost/system/error_code.hpp>

#include "exceptions/exception_traits_doesnotexist.h"
#include "exceptions/exception_traits_failedtoset.h"
#include "exceptions/exception_traits_notmutable.h"
#include "kernel/auxillary_traits/auxillary_traits_base.h"
#include "kernel/auxillary_traits/auxillary_traits_proxy.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{ 

    class Traits
    {
    public:
        template<IsTraitType TRAIT_TYPE>
        TraitValueProxy<Traits, TRAIT_TYPE> Get(const TRAIT_TYPE& trait_type)
        {
            auto it = m_Traits.find(trait_type.Name());
            if (m_Traits.end() == it)
            {
                throw Exceptions::Traits_DoesNotExist();
            }

            return TraitValueProxy<Traits, TRAIT_TYPE>(*this, it->second);
        }

        template<IsTraitType TRAIT_TYPE>
        const ConstTraitValueProxy<Traits, TRAIT_TYPE> Get(const TRAIT_TYPE& trait_type) const
        {
            auto it = m_Traits.find(trait_type.Name());
            if (m_Traits.end() == it)
            {
                throw Exceptions::Traits_DoesNotExist();
            }

            return ConstTraitValueProxy<Traits, TRAIT_TYPE>(*this, it->second);
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        TraitValueProxy<Traits, TRAIT_TYPE> operator[](const TRAIT_TYPE& trait_type)
        {
            return Get(trait_type);
        }

        template<IsTraitType TRAIT_TYPE>
        const ConstTraitValueProxy<Traits, TRAIT_TYPE> operator[](const TRAIT_TYPE& trait_type) const
        {
            return Get(trait_type);
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        bool Has(TRAIT_TYPE trait_type) const
        {
            return m_Traits.contains(trait_type.Name());
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        bool Remove(TRAIT_TYPE trait_type)
        {
            if (!trait_type.IsMutable())
            {
                // NOT PERMITTED -> Trait is immutable so cannot be removed (also covers the
                // "does not exist" case, which simply erases nothing).
                return false;
            }

            // PERMITTED -> mutable trait...remove it (no-op if it does not exist).
            return (0 < m_Traits.erase(trait_type.Name()));
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        void Set(const TRAIT_TYPE& trait_type, TRAIT_TYPE::TraitValue trait_value)
        {
            auto it = m_Traits.find(trait_type.Name());
            if (m_Traits.end() == it)
            {
                // PERMITTED -> Adding the trait for the first time
                auto [_, was_inserted] = m_Traits.emplace(trait_type.Name(), std::make_any<typename TRAIT_TYPE::TraitValue>(trait_value));
                if (!was_inserted)
                {
                    throw Exceptions::Traits_FailedToSet();
                }
            }
            else if (trait_type.IsMutable())
            {
                // PERMITTED -> The trait is mutable so the value can be changed.
                it->second = std::make_any<typename TRAIT_TYPE::TraitValue>(trait_value);
            }
            else
            {
                // NOT PERMITTED -> immutable trait (which exists) so the value is fixed.
                throw Exceptions::Traits_NotMutable();
            }
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        std::optional<typename TRAIT_TYPE::TraitValue> TryGet(const TRAIT_TYPE& trait_type) const
        {
            if (auto it = m_Traits.find(trait_type.Name()); m_Traits.end() == it)
            {
                LogTrace(Channel::Devices, [&trait_type]() { return std::format("Requested device trait ({}) does not exist", trait_type.Name()); });
            }
            else if (!(it->second.has_value()))
            {
                LogDebug(Channel::Devices, [&trait_type]() { return std::format("Requested device trait ({}) does not have an associated value", trait_type.Name()); });
            }
            else
            {
                try
                {
                    return std::any_cast<typename TRAIT_TYPE::TraitValue>(it->second);
                }
                catch (const std::bad_any_cast&)
                {
                    LogDebug(Channel::Devices, [&trait_type]() { return std::format("Requested device trait ({}) associated value is the incorrect type", trait_type.Name()); });
                }
            }

            return std::nullopt;
        }

    public:
        template<IsTraitType TRAIT_TYPE>
        std::expected<std::reference_wrapper<Traits>, boost::system::error_code> TrySet(const TRAIT_TYPE& trait_type, TRAIT_TYPE::TraitValue trait_value)
        {
            if (auto it = m_Traits.find(trait_type.Name()); m_Traits.end() == it)
            {
                // PERMITTED -> Adding the trait for the first time
                auto [_, was_inserted] = m_Traits.emplace(trait_type.Name(), std::make_any<typename TRAIT_TYPE::TraitValue>(trait_value));
                if (!was_inserted)
                {
                    /// FAILED -> Could not add the trait for some unspecified reason.
                    return std::unexpected(boost::system::errc::make_error_code(boost::system::errc::operation_not_permitted));
                }
            }
            else if (trait_type.IsMutable())
            {
                // PERMITTED -> The trait is mutable so the value can be changed.
                it->second = std::make_any<typename TRAIT_TYPE::TraitValue>(trait_value);
            }
            else
            {
                // NOT PERMITTED -> immutable trait (which exists) so the value is fixed.
                return std::unexpected(boost::system::errc::make_error_code(boost::system::errc::operation_not_permitted));
            }

            return *this;
        }

    private:
        std::unordered_map<std::string, std::any> m_Traits{};
    };

}
// namespace AqualinkAutomate::Kernel
