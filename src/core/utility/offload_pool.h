#pragma once

#include <cstddef>
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

namespace AqualinkAutomate::Utility
{

	//=========================================================================
	// OffloadPool — run deliberately-slow CPU work (argon2id password
	// hashing, PIN verification) OFF the single-threaded cooperative kernel
	// loop, posting the completion back to the caller's executor.
	//
	// The application's entire runtime (serial protocol, HTTP, MQTT, timers)
	// shares ONE io_context thread; an inline argon2 verify (~100ms by
	// design) would stall RS-485 pool control for its duration.  Work posted
	// here runs on a dedicated pool thread; the completion — and therefore
	// every touch of application state — happens back on the completion
	// executor, preserving the single-threaded state model.
	//
	//     offload.Run(io_context.get_executor(),
	//                 []            { return Argon2Verify(hash, password); },
	//                 [](bool okay) { /* back on the kernel thread */ });
	//=========================================================================
	class OffloadPool
	{
	public:
		explicit OffloadPool(std::size_t threads = 1) :
			m_Pool(threads)
		{
		}

		~OffloadPool()
		{
			m_Pool.join();
		}

		OffloadPool(const OffloadPool&) = delete;
		OffloadPool& operator=(const OffloadPool&) = delete;

	public:
		// Execute work() on a pool thread, then completion(result) on
		// completion_executor.  Both callables are moved; captured state must
		// stay alive until the completion runs (capture shared_ptr/by value).
		template<typename Work, typename Completion>
		void Run(boost::asio::any_io_executor completion_executor, Work&& work, Completion&& completion)
		{
			boost::asio::post(m_Pool,
				[completion_executor = std::move(completion_executor), work = std::forward<Work>(work), completion = std::forward<Completion>(completion)]() mutable
				{
					auto result = work();

					boost::asio::post(completion_executor,
						[completion = std::move(completion), result = std::move(result)]() mutable
						{
							completion(std::move(result));
						});
				});
		}

		// Drain outstanding work and stop the pool threads (idempotent; the
		// destructor calls it implicitly via join).
		void Join()
		{
			m_Pool.join();
		}

	private:
		boost::asio::thread_pool m_Pool;
	};

}
// namespace AqualinkAutomate::Utility
