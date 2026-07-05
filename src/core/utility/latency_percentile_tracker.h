#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <boost/circular_buffer.hpp>

namespace AqualinkAutomate::Utility
{

	/// Tracks latency samples and computes percentiles (p1, p50, p99) on demand.
	/// Uses a fixed-capacity ring buffer (bounded by max_samples) combined with a
	/// time-based sliding window so that only recent samples contribute to the
	/// window statistics.
	///
	/// NOTE: This type is NOT thread-safe. The application runs a single-threaded
	/// cooperative poll() loop, so no synchronisation is required; recording a
	/// sample is an amortised O(1) ring-buffer push with no per-sample allocation.
	template<typename CLOCK_TYPE = std::chrono::steady_clock>
	class LatencyPercentileTracker
	{
	public:
		using Duration = std::chrono::nanoseconds;
		using TimePoint = typename CLOCK_TYPE::time_point;

		struct LatencySample
		{
			TimePoint timestamp{};
			Duration latency{ 0 };
		};

		struct PercentileSnapshot
		{
			Duration p1{ 0 };    // 1st percentile (best case)
			Duration p50{ 0 };   // Median
			Duration p95{ 0 };   // 95th percentile
			Duration p99{ 0 };   // 99th percentile (worst case)
			Duration min{ 0 };   // Minimum observed (within window)
			Duration max{ 0 };   // Maximum observed (within window)
			Duration alltime_min{ 0 }; // All-time minimum
			Duration alltime_max{ 0 }; // All-time maximum
			Duration mean{ 0 };  // Arithmetic mean
			std::size_t sample_count{ 0 };
		};

	public:
		/// Constructs a latency tracker with the specified window size and duration.
		/// @param max_samples Maximum number of samples to retain (default 1000).
		/// @param window_duration Only samples within this duration are considered (default 60 seconds).
		explicit LatencyPercentileTracker(
			std::size_t max_samples = 1000,
			std::chrono::seconds window_duration = std::chrono::seconds(60))
			: m_Samples(max_samples == 0 ? 1 : max_samples)
			, m_WindowDuration(window_duration)
		{
			m_Scratch.reserve(m_Samples.capacity());
		}

		~LatencyPercentileTracker() = default;

		// Non-copyable but movable
		LatencyPercentileTracker(const LatencyPercentileTracker&) = delete;
		LatencyPercentileTracker& operator=(const LatencyPercentileTracker&) = delete;
		LatencyPercentileTracker(LatencyPercentileTracker&&) noexcept = default;
		LatencyPercentileTracker& operator=(LatencyPercentileTracker&&) noexcept = default;

	public:
		/// Records a latency sample with the current timestamp.
		void Record(Duration latency)
		{
			const TimePoint now = CLOCK_TYPE::now();

			// push_back on a full circular_buffer overwrites the oldest entry,
			// which enforces the max-sample limit with no extra book-keeping.
			m_Samples.push_back(LatencySample{ now, latency });

			// Prune samples that have fallen outside the time window.
			PruneOldSamples(now);

			// Update running (lifetime / cumulative) statistics. These persist beyond
			// the sliding window so a "since restart" view can be reported alongside
			// the recent-window percentiles.
			m_TotalSamples++;
			m_CumulativeSumNs += static_cast<double>(latency.count());

			if (m_TotalSamples == 1 || latency < m_AlltimeMin) { m_AlltimeMin = latency; }
			if (latency > m_AlltimeMax) { m_AlltimeMax = latency; }
		}

		/// Records a latency sample measured from a start time to now.
		void RecordSince(TimePoint start_time)
		{
			const TimePoint now = CLOCK_TYPE::now();
			const Duration latency = std::chrono::duration_cast<Duration>(now - start_time);
			Record(latency);
		}

		/// Computes and returns the current percentile snapshot.
		PercentileSnapshot GetSnapshot() const
		{
			PercentileSnapshot snapshot;

			if (m_Samples.empty())
			{
				return snapshot;
			}

			// Copy current window latencies into reusable scratch storage and sort
			// once so percentiles can be read by index (the existing interpolation
			// semantics are preserved exactly).
			m_Scratch.clear();
			Duration sum{ 0 };
			for (const auto& sample : m_Samples)
			{
				m_Scratch.push_back(sample.latency);
				sum += sample.latency;
			}

			std::ranges::sort(m_Scratch);

			snapshot.sample_count = m_Scratch.size();
			snapshot.min = m_Scratch.front();
			snapshot.max = m_Scratch.back();
			snapshot.alltime_min = m_AlltimeMin;
			snapshot.alltime_max = m_AlltimeMax;
			snapshot.mean = sum / static_cast<Duration::rep>(m_Scratch.size());

			snapshot.p1 = ComputePercentileFromSorted(1.0);
			snapshot.p50 = ComputePercentileFromSorted(50.0);
			snapshot.p95 = ComputePercentileFromSorted(95.0);
			snapshot.p99 = ComputePercentileFromSorted(99.0);

			return snapshot;
		}

		/// Returns the total number of samples ever recorded.
		std::size_t TotalSampleCount() const
		{
			return m_TotalSamples;
		}

		/// Returns the sliding-window duration (the span over which the GetSnapshot()
		/// percentiles are computed). Reported so consumers can label the window (e.g.
		/// "last 15 minutes") rather than assuming a fixed span.
		std::chrono::seconds WindowDuration() const
		{
			return m_WindowDuration;
		}

		/// Returns the cumulative (since-restart) arithmetic mean across every sample
		/// ever recorded, independent of the sliding window.
		Duration CumulativeMean() const
		{
			if (m_TotalSamples == 0)
			{
				return Duration{ 0 };
			}

			return Duration{ static_cast<Duration::rep>(m_CumulativeSumNs / static_cast<double>(m_TotalSamples)) };
		}

		/// Clears all recorded samples.
		void Reset()
		{
			m_Samples.clear();
			m_Scratch.clear();
			m_TotalSamples = 0;
			m_CumulativeSumNs = 0.0;
			m_AlltimeMin = Duration{ 0 };
			m_AlltimeMax = Duration{ 0 };
		}

	private:
		void PruneOldSamples(TimePoint now)
		{
			while (!m_Samples.empty() && (now - m_Samples.front().timestamp) > m_WindowDuration)
			{
				m_Samples.pop_front();
			}
		}

		/// Computes a percentile from the sorted scratch buffer using linear
		/// interpolation between the two nearest ranks.
		Duration ComputePercentileFromSorted(double percentile) const
		{
			if (m_Scratch.empty())
			{
				return Duration{ 0 };
			}

			if (m_Scratch.size() == 1)
			{
				return m_Scratch.front();
			}

			const double rank = (percentile / 100.0) * static_cast<double>(m_Scratch.size() - 1);
			const auto lower_idx = static_cast<std::size_t>(std::floor(rank));
			const auto upper_idx = static_cast<std::size_t>(std::ceil(rank));
			const double fraction = rank - static_cast<double>(lower_idx);

			const Duration::rep lower_val = m_Scratch[lower_idx].count();

			if (lower_idx == upper_idx)
			{
				return Duration{ lower_val };
			}

			const Duration::rep upper_val = m_Scratch[upper_idx].count();

			const double interpolated = static_cast<double>(lower_val) + (fraction * static_cast<double>(upper_val - lower_val));
			return Duration{ static_cast<Duration::rep>(interpolated) };
		}

	private:
		boost::circular_buffer<LatencySample> m_Samples;
		std::chrono::seconds m_WindowDuration;

		// Reusable scratch storage for percentile computation; mutable so the
		// const GetSnapshot() can borrow it without per-call allocation.
		mutable std::vector<Duration> m_Scratch;

		// Running (cumulative / lifetime) statistics
		std::size_t m_TotalSamples{ 0 };
		double m_CumulativeSumNs{ 0.0 };
		Duration m_AlltimeMin{ 0 };
		Duration m_AlltimeMax{ 0 };
	};

	/// Convenience class for measuring operation latency using RAII.
	/// Records the latency when the scope exits.
	template<typename CLOCK_TYPE = std::chrono::steady_clock>
	class ScopedLatencyMeasurement
	{
	public:
		using Tracker = LatencyPercentileTracker<CLOCK_TYPE>;

		explicit ScopedLatencyMeasurement(Tracker& tracker)
			: m_Tracker(tracker)
			, m_StartTime(CLOCK_TYPE::now())
		{
		}

		~ScopedLatencyMeasurement()
		{
			if (!m_Cancelled)
			{
				m_Tracker.RecordSince(m_StartTime);
			}
		}

		// Non-copyable, non-movable
		ScopedLatencyMeasurement(const ScopedLatencyMeasurement&) = delete;
		ScopedLatencyMeasurement& operator=(const ScopedLatencyMeasurement&) = delete;
		ScopedLatencyMeasurement(ScopedLatencyMeasurement&&) = delete;
		ScopedLatencyMeasurement& operator=(ScopedLatencyMeasurement&&) = delete;

		/// Cancel recording - useful when the operation failed or was aborted.
		void Cancel()
		{
			m_Cancelled = true;
		}

	private:
		Tracker& m_Tracker;
		typename CLOCK_TYPE::time_point m_StartTime;
		bool m_Cancelled{ false };
	};

	/// Collection of latency metrics for serial operations.
	class SerialLatencyMetrics
	{
	public:
		SerialLatencyMetrics() = default;
		~SerialLatencyMetrics() = default;

		// Non-copyable, non-movable
		SerialLatencyMetrics(const SerialLatencyMetrics&) = delete;
		SerialLatencyMetrics& operator=(const SerialLatencyMetrics&) = delete;

	public:
		/// Latency for serial read operations (time from initiating read to receiving data).
		LatencyPercentileTracker<> ReadLatency{ 8192, std::chrono::seconds(900) };

		/// Latency for serial write operations (time from initiating write to completion).
		LatencyPercentileTracker<> WriteLatency{ 8192, std::chrono::seconds(900) };

		/// Latency for complete message processing (from receiving bytes to signaling handlers).
		LatencyPercentileTracker<> MessageProcessingLatency{ 8192, std::chrono::seconds(900) };
	};

}
// namespace AqualinkAutomate::Utility
