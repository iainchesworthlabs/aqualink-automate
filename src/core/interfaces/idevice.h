#pragma once

#include <memory>

#include "interfaces/ideviceidentifier.h"

namespace AqualinkAutomate::Interfaces
{

    class IDevice
    {
    public:
        explicit IDevice(std::shared_ptr<IDeviceIdentifier> device_id);
        virtual ~IDevice() = default;

        IDevice(const IDevice& other) = delete;
        IDevice(IDevice&& other) noexcept = default;

        // Virtual so a protocol's device can narrow the return type to its own identifier
        // (a covariant override) instead of declaring a same-named accessor that HIDES this
        // one -- which would make the answer depend on the static type of the reference the
        // caller happens to hold.
        virtual const IDeviceIdentifier& DeviceId() const;

    private:
        std::shared_ptr<IDeviceIdentifier> m_DeviceId;
    };

}
// namespace AqualinkAutomate::Interfaces
