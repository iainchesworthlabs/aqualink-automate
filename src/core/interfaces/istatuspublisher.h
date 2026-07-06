#pragma once

#include <concepts>
#include <memory>
#include <type_traits>

#include <boost/signals2.hpp>

#include "interfaces/istatus.h"

namespace AqualinkAutomate::Interfaces
{

	class IStatusPublisher
	{
	public:
		using StatusType = std::weak_ptr<const Interfaces::IStatus>;
		using StatusSignalType = boost::signals2::signal<void(StatusType)>;

	public:
		IStatusPublisher() = delete;
		~IStatusPublisher() = default;

	public:
		template<typename DEVICE_STATUS>
			requires std::derived_from<DEVICE_STATUS, Interfaces::IStatus>
		IStatusPublisher(const DEVICE_STATUS& status)
		{
			Status(status);
		}

	public:
		StatusSignalType StatusSignal;

	public:
		StatusType Status() const;

	public:
		template<typename DEVICE_STATUS>
			requires std::derived_from<DEVICE_STATUS, Interfaces::IStatus>
		void Status(const DEVICE_STATUS& status)
		{
			// Devices re-publish their status on every poll; only fan out the signal when the
			// status actually changes. Statuses are compile-time tag types (see StatusTag), so
			// identity is by concrete type - re-publishing the same status type is a genuine no-op
			// for consumers (WebSocket / MQTT / EquipmentHub). The currently-published status is
			// a DEVICE_STATUS iff a dynamic_cast to it succeeds (mirrors operator== below); this
			// avoids a typeid on the polymorphic glvalue, which Clang flags under
			// -Wpotentially-evaluated-expression.
			const bool changed = (nullptr == m_Status) || (nullptr == dynamic_cast<const DEVICE_STATUS*>(m_Status.get()));

			m_Status = std::make_shared<DEVICE_STATUS>(status);

			if (changed)
			{
				StatusSignal(StatusType(m_Status));
			}
		}

	private:
		std::shared_ptr<const Interfaces::IStatus> m_Status{ nullptr };
	};

	//---------------------------------------------------------------------
	// STATUS COMPARE FUNCTIONS
	//---------------------------------------------------------------------

	// To support status publishing, there needs to be a mechanism to compare statuses.
	//
	// Two concrete IStatus types are considered "the same status" iff they are the
	// same C++ type.  This is a compile-time, type-only relation: it inspects no
	// runtime field, so it is exposed as a named constexpr predicate rather than as
	// an operator==.  Modelling it as operator== was misleading — it silently applied
	// to any pair of IStatus subtypes and returned a value-blind result that read like
	// a value comparison.

	template<typename THIS_STATUS, typename THAT_STATUS>
		requires std::derived_from<THIS_STATUS, Interfaces::IStatus> && std::derived_from<THAT_STATUS, Interfaces::IStatus>
	[[nodiscard]] consteval bool IsSameStatusType() noexcept
	{
		return std::same_as<std::remove_cvref_t<THIS_STATUS>, std::remove_cvref_t<THAT_STATUS>>;
	}

	// Enable real (category) comparisons of a concrete status type against the
	// published std::weak_ptr status.  This is genuine runtime equality — it locks
	// the weak_ptr and checks whether the live published status is-a THIS_STATUS —
	// so it remains an operator==.

	template<typename THIS_STATUS>
		requires std::derived_from<THIS_STATUS, Interfaces::IStatus>
	[[nodiscard]] bool operator==(const THIS_STATUS&, IStatusPublisher::StatusType that_status)
	{
		if (auto locked = that_status.lock())
		{
			return dynamic_cast<const THIS_STATUS*>(locked.get()) != nullptr;
		}
		return false;
	}

	template<typename THIS_STATUS>
		requires std::derived_from<THIS_STATUS, Interfaces::IStatus>
	[[nodiscard]] bool operator!=(const THIS_STATUS& this_status, IStatusPublisher::StatusType that_status)
	{
		return !(this_status == that_status);
	}

}
// namespace AqualinkAutomate::Interfaces
