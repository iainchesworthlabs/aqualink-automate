#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <tuple>

#include "developer/chrono_clocks.h"
#include "developer/serial_port_options.h"

namespace AqualinkAutomate::Utility
{

	template<typename CLOCK_TYPE = Developer::ChronoClocks::SteadyClock>
	class UtilisationOverPeriod
	{
		using TimeElement = typename CLOCK_TYPE::time_point;
		using BytesElement = uint64_t;
		using HistoryElement = std::tuple<TimeElement, BytesElement>;
		using HistoryCollection = std::deque<HistoryElement>;

	public:
		explicit UtilisationOverPeriod(std::chrono::seconds duration) :
			UtilisationOverPeriod(duration, Developer::SerialPortOptions{})
		{
		}

		UtilisationOverPeriod(std::chrono::seconds duration, Developer::SerialPortOptions serial_port_options) :
			m_History(),
			m_ReferenceTime(CLOCK_TYPE::now()),
			m_StatsDuration(duration),
			m_MaxBandwidthBytesPerSec(ConvertSerialSpeedToBytesPerSecond(serial_port_options))
		{
		}

		~UtilisationOverPeriod() = default;

		// Rule-of-zero: the implicitly generated copy/move special members are
		// correct for the value-semantic members held here.
		UtilisationOverPeriod(const UtilisationOverPeriod&) = default;
		UtilisationOverPeriod& operator=(const UtilisationOverPeriod&) = default;
		UtilisationOverPeriod(UtilisationOverPeriod&&) noexcept = default;
		UtilisationOverPeriod& operator=(UtilisationOverPeriod&&) noexcept = default;

		const HistoryCollection& History() const
		{
			return m_History;
		}

		TimeElement ReferenceTime() const
		{
			return m_ReferenceTime;
		}

		std::chrono::seconds StatsDuration() const
		{
			return m_StatsDuration;
		}

		double Utilisation() const
		{
			return CalculateCurrentUtilization();
		}

		/// Returns a copy with the supplied bytes added.
		/// NOTE: deep-copies the entire history; prefer operator+= on the hot path.
		UtilisationOverPeriod operator+(BytesElement bytes) const
		{
			auto updated_this = *this;
			updated_this.UpdateHistory(bytes);
			return updated_this;
		}

		UtilisationOverPeriod& operator+=(BytesElement bytes)
		{
			UpdateHistory(bytes);
			return *this;
		}

	private:
		void UpdateHistory(BytesElement bytes)
		{
			auto now_time = CLOCK_TYPE::now();

			m_History.push_back(std::make_tuple(now_time, bytes));
			m_RunningTotalBytes += bytes;

			PruneHistory(now_time);
		}

		void PruneHistory(const TimeElement& now_time)
		{
			// Elements are appended in monotonic time order, so any sample that
			// has aged out of the window is at the front. Prune from the front
			// and keep the running total in step (amortised O(1) per insert).
			while (!m_History.empty() && ((now_time - std::get<TimeElement>(m_History.front())) > m_StatsDuration))
			{
				m_RunningTotalBytes -= std::get<BytesElement>(m_History.front());
				m_History.pop_front();
			}
		}

		double CalculateCurrentUtilization() const
		{
			if (m_History.empty())
			{
				// Nothing to count so the utilisation is zero.
				return 0.0;
			}

			const double bandwidth = static_cast<double>(m_RunningTotalBytes) / static_cast<double>(m_StatsDuration.count());
			const double utilization = (bandwidth / m_MaxBandwidthBytesPerSec) * 100;

			return utilization;
		}

	public:
		constexpr double ConvertSerialSpeedToBytesPerSecond(const Developer::SerialPortOptions& options)
		{
			const double bits_start = 1;
			const double bits_per_character = options.character_size;
			const double bits_parity = (options.parity != Serial::Parity::None) ? 1 : 0;
			const double bits_stop = (options.stop_bits == Serial::StopBits::One) ? 1 : 2;

			const double temp1 = bits_start + bits_per_character + bits_parity + bits_stop;
			const double temp2 = bits_per_character / temp1;
			const double temp3 = options.baud_rate / 8.0f;

			const double byterate_useful = temp2 * temp3;
			return byterate_useful;
		}

	private:
		HistoryCollection m_History;
		TimeElement m_ReferenceTime;
		std::chrono::seconds m_StatsDuration;
		BytesElement m_RunningTotalBytes{ 0 };

		double m_MaxBandwidthBytesPerSec;
	};

	class FlowStatistics
	{
	public:
		FlowStatistics() = default;

		// Rule-of-zero: implicit copy/move are correct for the held members.
		FlowStatistics(const FlowStatistics&) = default;
		FlowStatistics& operator=(const FlowStatistics&) = default;
		FlowStatistics(FlowStatistics&&) noexcept = default;
		FlowStatistics& operator=(FlowStatistics&&) noexcept = default;
		~FlowStatistics() = default;

		FlowStatistics& operator+=(const uint64_t bytes)
		{
			TotalBytes += bytes;

			Average_OneSecond += bytes;
			Average_ThirtySecond += bytes;
			Average_FiveMinute += bytes;

			return *this;
		}

	public:
		uint64_t TotalBytes{ 0 };

		UtilisationOverPeriod<> Average_OneSecond{ std::chrono::seconds(1) };
		UtilisationOverPeriod<> Average_ThirtySecond{ std::chrono::seconds(30) };
		UtilisationOverPeriod<> Average_FiveMinute{ std::chrono::minutes(5) };
	};

	class BandwidthMetricsCollection
	{
	public:
		FlowStatistics Read;
		FlowStatistics Write;
	};

}
// namespace AqualinkAutomate::Utility
