#pragma once

#include <boost/cstdfloat.hpp>

#include "types/units_electric_potential.h"

using namespace AqualinkAutomate::Units;

namespace AqualinkAutomate::Kernel
{

    class ORP
    {
    public:
        ORP(boost::float64_t value_in_mV);  // NOSONAR(cpp:S1709) — implicit numeric conversion is part of this value-wrapper's contract (see operator= / operator== on float64_t)
        ORP(const ORP& other) = default;
        ORP& operator=(const ORP& other) = default;
        ORP(ORP&& other) noexcept = default;
        ORP& operator=(ORP&& other) noexcept = default;

        Units::millivolt_quantity operator()() const;

        ORP& operator=(const Units::millivolt_quantity& value_in_mV);
        ORP& operator=(boost::float64_t value_in_mV);

        bool operator==(const ORP& other) const;
        bool operator==(const boost::float64_t value_in_mV) const;
        bool operator==(const Units::millivolt_quantity& value_in_mV) const;

    private:
        Units::millivolt_quantity m_ORP;
    };

}
// namespace AqualinkAutomate::Kernel
