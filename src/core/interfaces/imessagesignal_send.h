#pragma once

#include <format>
#include <functional>
#include <memory>
#include <type_traits>

#include <boost/signals2.hpp>

#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Interfaces
{

	class IMessageSignalSendBase
	{
	public:
		virtual ~IMessageSignalSendBase() = default;
		virtual void Signal_MessageToSend() = 0;
	};

	template<typename MESSAGE_TYPE>
	class IMessageSignalSend : public IMessageSignalSendBase
	{
	protected:
		IMessageSignalSend() = default;

	public:
		virtual ~IMessageSignalSend() = default;

		using PublisherRef = std::reference_wrapper<const MESSAGE_TYPE>;
		using PublisherFunc = boost::signals2::signal<void(PublisherRef)>;
		using PublisherPtr = std::shared_ptr<PublisherFunc>;

		static PublisherPtr& GetPublisher()
		{
			static PublisherPtr publisher(std::make_shared<PublisherFunc>());
			return publisher;
		}

		void Signal_MessageToSend() final
		{
			using enum Channel;

			if (auto publisher_ptr = GetPublisher(); nullptr == publisher_ptr)
			{
				LogTrace(Messages, "Could not retrieve signal shared_ptr from IMessageSignalSend::GetPublisher()");
			}
			else if (auto upcast_ptr = dynamic_cast<MESSAGE_TYPE *>(this); nullptr == upcast_ptr)
			{
				const std::type_info& src_type = typeid(this);
				const std::type_info& dst_type = typeid(MESSAGE_TYPE*);

				LogDebug(Messages, std::format("Could not convert from 'this' to the target message pointer type: src -> {}, dst -> {}", src_type.name(), dst_type.name()));
			}
			else
			{
				LogTrace(Signals, "Signalling all registered slots for published message");
				(*publisher_ptr)(*upcast_ptr);
			}
		}
	};

}
// namespace AqualinkAutomate::Interfaces
