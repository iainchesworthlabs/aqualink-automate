#pragma once

#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

#include "exceptions/exception_hubnotfound.h"
#include "interfaces/ihub.h"

namespace AqualinkAutomate::Kernel
{

	/// @brief Types that may be registered with / resolved from the HubLocator.
	///
	/// Resolution is keyed on the exact static type (std::type_index{ typeid(T) }),
	/// so a handle registered as shared_ptr<DataHub> is found by Find<DataHub>(),
	/// NOT by Find<IHub>(). The type must therefore be a concrete (non-cv) class
	/// type whose typeid uniquely identifies it. This accommodates both the IHub
	/// state hubs (DataHub / EquipmentHub / StatisticsHub) and the standalone
	/// service interfaces registered under the same container
	/// (Interfaces::ICommandDispatcher, Interfaces::IRecordingController).
	template <typename T>
	concept RegistrableHub = std::is_class_v<T> && std::is_same_v<T, std::remove_cv_t<T>>;

	/// @brief Lightweight dependency-injection container for the application hubs
	///        and service interfaces.
	///
	/// Handles are stored type-erased as shared_ptr<void> keyed by the registering
	/// type's std::type_index, giving O(1) Register / Find / TryFind / Unregister.
	/// The container is unsynchronised; it is constructed and consumed on the single
	/// cooperative poll() thread.
	class HubLocator
	{
	public:
		template <RegistrableHub T>
		HubLocator& Register(std::shared_ptr<T> hub)
		{
			m_Hubs.insert_or_assign(std::type_index{ typeid(T) }, std::static_pointer_cast<void>(std::move(hub)));
			return *this;
		}

		template <RegistrableHub T>
		std::shared_ptr<T> Find()
		{
			if (auto hub = TryFind<T>(); nullptr != hub)
			{
				return hub;
			}

			throw Exceptions::Hub_NotFound();
		}

		template <RegistrableHub T>
		std::shared_ptr<T> TryFind()
		{
			if (auto it = m_Hubs.find(std::type_index{ typeid(T) }); m_Hubs.end() != it)
			{
				return std::static_pointer_cast<T>(it->second);
			}

			return nullptr;
		}

		template <RegistrableHub T>
		HubLocator& Unregister()
		{
			m_Hubs.erase(std::type_index{ typeid(T) });
			return *this;
		}

	public:
		std::unordered_map<std::type_index, std::shared_ptr<void>> m_Hubs;
	};

}
// namespace AqualinkAutomate::Kernel
