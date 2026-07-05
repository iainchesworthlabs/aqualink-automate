#pragma once

#include <concepts>
#include <memory>

#include <boost/signals2.hpp>

#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Interfaces
{

	class IMessageSignalRecvBase
	{
	public:
		virtual void Signal_MessageWasReceived() = 0;
	};

	// CRTP base: a concrete message type T derives from IMessageSignalRecv<T>,
	// so the most-derived object of any IMessageSignalRecv<MESSAGE_TYPE> instance
	// is always a MESSAGE_TYPE.  That static guarantee lets the per-message
	// dispatch use static_cast instead of a runtime dynamic_cast on the hot path.
	template<typename MESSAGE_TYPE>
	class IMessageSignalRecv : public IMessageSignalRecvBase
	{
	protected:
		IMessageSignalRecv() = default;

	public:
		virtual ~IMessageSignalRecv() = default;

	public:
		using SignalRef = const MESSAGE_TYPE&;
		using SignalFunc = boost::signals2::signal<void(SignalRef)>;
		using SignalPtr = std::shared_ptr<SignalFunc>;

		static SignalPtr& GetSignal()
		{
			static SignalPtr signal(std::make_shared<SignalFunc>());
			return signal;
		}

	public:
		void Signal_MessageWasReceived() final
		{
			// CRTP precondition: MESSAGE_TYPE must derive from this base so the
			// static_cast below is well-defined.  Checked here (rather than in the
			// class body) because MESSAGE_TYPE is incomplete while the template is
			// being defined.
			static_assert(std::derived_from<MESSAGE_TYPE, IMessageSignalRecv<MESSAGE_TYPE>>,
				"IMessageSignalRecv<MESSAGE_TYPE> requires MESSAGE_TYPE to derive from it (CRTP)");

			if (auto signal_ptr = GetSignal(); nullptr == signal_ptr)
			{
				LogTrace(Channel::Signals, "Could not retrieve signal shared_ptr from IMessageSignalRecv::GetSignal()");
			}
			else
			{
				// CRTP guarantees 'this' is a MESSAGE_TYPE subobject, so the cast
				// cannot fail; no dynamic_cast / failure branch is required.
				auto* const upcast_ptr = static_cast<MESSAGE_TYPE*>(this);

				LogTrace(Channel::Signals, "Signalling all registered slots for received message");
				(*signal_ptr)(*upcast_ptr);
			}
		}
	};

}
// namespace AqualinkAutomate::Interfaces
