#include "formatters/url_segments_encoded_view_formatter.h"


namespace std
{

	std::ostream& operator<<(std::ostream& os, const boost::urls::segments_encoded_view& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace std
