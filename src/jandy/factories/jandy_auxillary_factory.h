#include <expected>
#include <memory>
#include <string>
#include <variant>

#include <boost/system/error_code.hpp>

#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_status.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "utility/string_conversion/auxillary_state_string_converter.h"

namespace AqualinkAutomate::Factory
{

	class JandyAuxillaryFactory
	{
		inline static const std::string AQUAPURE { "AquaPure" };
		inline static const std::string CHLORINATOR { "Chlorinator" };
		inline static const std::string CLEANER { "Cleaner" };
		inline static const std::string HEAT { "Heat" };
		inline static const std::string HEATER { "Heater" };
		inline static const std::string PUMP { "Pump" };
		inline static const std::string SPILLOVER { "Spillover" };
		inline static const std::string SPRINKLER { "Sprinkler" };
		inline static const std::string UNKNOWN { "Unknown" };

		struct GenericDevice_Data 
		{
			std::optional<std::string> Label;
		};

		struct AuxillaryDevice_Data : public GenericDevice_Data 
		{
			Auxillaries::JandyAuxillaryIds Id;
			std::optional<Kernel::AuxillaryStatuses> Status;
		};

		struct ChlorinatorDevice_Data : public GenericDevice_Data 
		{
			std::optional<Kernel::AuxillaryStatuses> Status;
		};

		struct CleanerDevice_Data : public GenericDevice_Data {};

		struct HeaterDevice_Data : public GenericDevice_Data
		{
			std::optional<Kernel::AuxillaryStatuses> Status;
		};

		struct PumpDevice_Data : public GenericDevice_Data 
		{
			std::optional<Kernel::AuxillaryStatuses> Status;
		};

		struct SpilloverDevice_Data : public GenericDevice_Data {};

		struct SprinklerDevice_Data : public GenericDevice_Data {};

		struct UnknownDevice_Data : public GenericDevice_Data {};

		using DeviceData = std::variant<
			AuxillaryDevice_Data,
			ChlorinatorDevice_Data,
			CleanerDevice_Data,
			HeaterDevice_Data,
			PumpDevice_Data,
			SpilloverDevice_Data,
			SprinklerDevice_Data,
			UnknownDevice_Data
		>;

	private:
		JandyAuxillaryFactory();
		~JandyAuxillaryFactory() = default;

	public:
		JandyAuxillaryFactory(const JandyAuxillaryFactory&) = delete;
		JandyAuxillaryFactory& operator=(const JandyAuxillaryFactory&) = delete;
		JandyAuxillaryFactory(JandyAuxillaryFactory&&) = delete;
		JandyAuxillaryFactory& operator=(JandyAuxillaryFactory&&) = delete;

		static JandyAuxillaryFactory& Instance();

		std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> SerialAdapterDevice_CreateDevice(const Auxillaries::JandyAuxillaryIds aux_id);
		std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> SerialAdapterDevice_CreateDevice(const Auxillaries::JandyAuxillaryIds aux_id, const Auxillaries::JandyAuxillaryStatuses status);
		std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> OneTouchDevice_CreateDevice(const Utility::AuxillaryStateStringConverter& aux_state);

	private:
		bool IsAuxillaryDevice(const std::string& label) const;
		bool IsChlorinatorDevice(const std::string& label) const;
		bool IsCleanerDevice(const std::string& label) const;
		bool IsHeaterDevice(const std::string& label) const;
		bool IsPumpDevice(const std::string& label) const;
		bool IsSpilloverDevice(const std::string& label) const;
		bool IsSprinklerDevice(const std::string& label) const;

		std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> CreateDevice_Impl(DeviceData& device_data);
	};

}
// namespace AqualinkAutomate::Factory
