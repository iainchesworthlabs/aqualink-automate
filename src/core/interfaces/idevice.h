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

        const IDeviceIdentifier& DeviceId() const;

    private:
        std::shared_ptr<IDeviceIdentifier> m_DeviceId;
    };

}
// namespace AqualinkAutomate::Interfaces
