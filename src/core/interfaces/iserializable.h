#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AqualinkAutomate::Interfaces
{

    class ISerializable
    {
    public:
        ISerializable() = default;
        virtual ~ISerializable() = default;

        virtual bool Serialize(std::vector<uint8_t>& message_bytes) const = 0;
        virtual bool Deserialize(const std::span<const std::byte>& message_bytes) = 0;
    };

} 
// namespace AqualinkAutomate::Interfaces
