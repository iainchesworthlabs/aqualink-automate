#pragma once

#include <cstddef>
#include <string>

namespace AqualinkAutomate::Interfaces
{

	/// @brief Runtime control surface for on-demand serial-traffic recording.
	///
	/// Implemented by the recording decorator that sits in the production serial
	/// chain (see Developer::RecordingSerialPortImpl).  Registered in the
	/// HubLocator so a diagnostics HTTP route can resolve it via Find<> and
	/// toggle recording against live hardware without restarting the application.
	///
	/// When NOT recording the implementation MUST be a transparent pass-through
	/// with negligible overhead (no file held open, no per-byte work).
	class IRecordingController
	{
	public:
		/// @brief Snapshot of the recorder's current state.
		struct Status
		{
			bool recording{ false };       ///< True while a capture is being written.
			std::string file;              ///< Path of the active capture (empty when not recording).
			std::size_t bytes_written{ 0 }; ///< Wire bytes (R+W) written to the active capture.
		};

	public:
		virtual ~IRecordingController() = default;

		/// @brief Begin recording wire traffic to @p filename.
		/// @returns true if recording started; false if a capture is already in
		///          progress or the file could not be opened.
		virtual bool StartRecording(const std::string& filename) = 0;

		/// @brief Stop the active recording (flushing and closing the file).
		/// @returns true if a recording was active and has been stopped; false if
		///          nothing was recording.
		virtual bool StopRecording() = 0;

		/// @brief True while a capture is in progress.
		virtual bool IsRecording() const = 0;

		/// @brief Current recorder status (recording flag, file, bytes written).
		virtual Status RecordingStatus() const = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
