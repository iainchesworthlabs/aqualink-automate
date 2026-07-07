#pragma once

#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

    class HTTP_DuplicateRoute : public GenericAqualinkException
    {
        static const std::string DUPLICATE_ROUTE_MESSAGE;

    public:
        HTTP_DuplicateRoute();
        explicit HTTP_DuplicateRoute(const std::string &message);
    };

}
// namespace AqualinkAutomate::Exceptions
