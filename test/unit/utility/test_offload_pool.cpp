#include <boost/test/unit_test.hpp>

#include <chrono>
#include <thread>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "utility/offload_pool.h"

using namespace AqualinkAutomate;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(TestSuite_OffloadPool)

BOOST_AUTO_TEST_CASE(Test_OffloadPool_WorkRunsOffCallerThread_CompletionRunsOnExecutor)
{
	boost::asio::io_context io_context;

	// Keep run() alive until the completion lands: without a work guard the
	// context would run out of work (and return) before the slow offloaded
	// job posts its completion back.
	auto guard = boost::asio::make_work_guard(io_context);

	const auto caller_thread = std::this_thread::get_id();
	std::thread::id work_thread{}, completion_thread{};
	int completion_result{ 0 };

	{
		Utility::OffloadPool offload{ 1 };

		offload.Run(io_context.get_executor(),
			[&work_thread]()
			{
				work_thread = std::this_thread::get_id();
				std::this_thread::sleep_for(25ms); // Simulate slow crypto.
				return 42;
			},
			[&](int result)
			{
				completion_thread = std::this_thread::get_id();
				completion_result = result;
				guard.reset(); // Let run() finish.
			});

		io_context.run();
	}

	BOOST_CHECK_EQUAL(completion_result, 42);
	BOOST_CHECK(work_thread != std::thread::id{});
	BOOST_CHECK(work_thread != caller_thread);            // Off the kernel thread...
	BOOST_CHECK(completion_thread == caller_thread);      // ...back on it to touch state.
}

BOOST_AUTO_TEST_CASE(Test_OffloadPool_MultipleJobsAllComplete)
{
	boost::asio::io_context io_context;

	auto guard = boost::asio::make_work_guard(io_context);

	int completed{ 0 };

	{
		Utility::OffloadPool offload{ 2 };

		for (int i = 0; i < 8; ++i)
		{
			offload.Run(io_context.get_executor(),
				[i]() { return i; },
				[&](int)
				{
					if (8 == ++completed)
					{
						guard.reset(); // All delivered: let run() finish.
					}
				});
		}

		io_context.run();
	}

	BOOST_CHECK_EQUAL(completed, 8);
}

BOOST_AUTO_TEST_SUITE_END()
