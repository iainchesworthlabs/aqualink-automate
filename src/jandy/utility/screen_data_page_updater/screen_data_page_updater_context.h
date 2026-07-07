#pragma once

namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
{

	template<typename PAGE_TYPE>
	class Context
	{
	public:
		explicit Context(PAGE_TYPE& page) :
			m_Page(page)
		{
		};

		PAGE_TYPE& operator()()
		{
			return m_Page;
		}

	private:
		PAGE_TYPE& m_Page;
	};

}
// namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
