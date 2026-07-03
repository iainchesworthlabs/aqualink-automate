#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>

#include <boost/uuid/uuid_io.hpp>

#include "auth/entitlement.h"
#include "auth/subject.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/data_hub.h"
#include "scheduling/schedule.h"
#include "scheduling/schedule_authorization.h"

using namespace AqualinkAutomate;

//=============================================================================
// Schedule action-gating (D14): a subject may only save a schedule that
// performs actions they can perform directly.  Button actions resolve their
// target label to the device id and gate on equipment.control.aux:<id>,
// matching the live control route.
//=============================================================================

namespace
{
	Auth::Subject SubjectWith(std::initializer_list<const char*> entitlements)
	{
		Auth::Subject subject;
		subject.Id = "test-subject";
		subject.Authenticated = true;
		subject.Provider = Auth::SubjectProvider::Local;
		for (const auto* text : entitlements)
		{
			subject.Entitlements.Add(*Auth::Entitlement::Parse(text));
		}
		return subject;
	}

	Scheduling::Schedule ButtonSchedule(std::string target_label)
	{
		Scheduling::Schedule schedule;
		schedule.name = "test";
		schedule.action.type = Scheduling::ActionType::ButtonOn;
		schedule.action.target = std::move(target_label);
		return schedule;
	}

	Scheduling::Schedule TypedSchedule(Scheduling::ActionType type)
	{
		Scheduling::Schedule schedule;
		schedule.name = "test";
		schedule.action.type = type;
		schedule.action.value = 30;
		return schedule;
	}

	struct DataHubFixture
	{
		DataHubFixture()
		{
			auto device = std::make_shared<Kernel::AuxillaryDevice>();
			device->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, std::string{ "Pool Light" });
			PoolLightId = boost::uuids::to_string(device->Id());
			Hub.Devices.Add(std::move(device));
		}

		Kernel::DataHub Hub;
		std::string PoolLightId;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_ScheduleAuthorization, DataHubFixture)

BOOST_AUTO_TEST_CASE(Test_ScheduleAuth_AuthOffPermitsEverything)
{
	std::string error;
	const auto nobody = Auth::Subject::Anonymous();

	// Posture off: no gate, even for an unknown label.
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(nobody, ButtonSchedule("Anything"), Hub, /*auth_enabled=*/false, error));
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(nobody, TypedSchedule(Scheduling::ActionType::ChlorinatorPercent), Hub, false, error));
}

BOOST_AUTO_TEST_CASE(Test_ScheduleAuth_ButtonWildcardAndAdmin)
{
	std::string error;

	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(SubjectWith({ "equipment.control.aux:*" }), ButtonSchedule("Pool Light"), Hub, true, error));
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(SubjectWith({ "system.admin" }), ButtonSchedule("Pool Light"), Hub, true, error));
}

BOOST_AUTO_TEST_CASE(Test_ScheduleAuth_ButtonPerAuxSelectorMatchesDeviceId)
{
	std::string error;

	// Granted exactly this device's id -> permitted...
	const auto scoped = SubjectWith({ std::string{ "equipment.control.aux:" + PoolLightId }.c_str() });
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(scoped, ButtonSchedule("Pool Light"), Hub, true, error));

	// ...but not entitled for the control type at all -> denied.
	const auto viewer = SubjectWith({ "equipment.view" });
	BOOST_CHECK(!Scheduling::AuthorizeScheduleAction(viewer, ButtonSchedule("Pool Light"), Hub, true, error));
	BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(Test_ScheduleAuth_UnknownLabelFailsClosed)
{
	std::string error;
	const auto admin_of_aux = SubjectWith({ "equipment.control.aux:*" });

	// Even a broad aux grant cannot save a schedule for a device that does not
	// exist — the target cannot be resolved, so it fails closed.
	BOOST_CHECK(!Scheduling::AuthorizeScheduleAction(admin_of_aux, ButtonSchedule("Ghost Device"), Hub, true, error));
	BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(Test_ScheduleAuth_NonButtonControlTypes)
{
	std::string error;

	const auto setpointer = SubjectWith({ "equipment.control.setpoints" });
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(setpointer, TypedSchedule(Scheduling::ActionType::PoolSetpoint), Hub, true, error));
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(setpointer, TypedSchedule(Scheduling::ActionType::SpaSetpoint), Hub, true, error));

	// A setpoints grant does not extend to the chlorinator.
	BOOST_CHECK(!Scheduling::AuthorizeScheduleAction(setpointer, TypedSchedule(Scheduling::ActionType::ChlorinatorPercent), Hub, true, error));

	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(SubjectWith({ "equipment.control.chlorinator" }), TypedSchedule(Scheduling::ActionType::ChlorinatorPercent), Hub, true, error));
	BOOST_CHECK(Scheduling::AuthorizeScheduleAction(SubjectWith({ "equipment.control.circulation" }), TypedSchedule(Scheduling::ActionType::CirculationMode), Hub, true, error));
}

BOOST_AUTO_TEST_SUITE_END()
