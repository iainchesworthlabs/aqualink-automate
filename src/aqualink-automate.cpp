// Standard library
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

// Third-party
#include <boost/asio.hpp>
#include <boost/stacktrace.hpp>
#include <magic_enum/magic_enum.hpp>

// Core — infrastructure
#include "certificates/certificate_management.h"
#include "exceptions/exception_optionparsingfailed.h"
#include "exceptions/exception_optionshelporversion.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/irecordingcontroller.h"
#include "interfaces/iserialportimpl.h"
#include "logging/logging.h"
#include "logging/logging_initialise.h"
#include "logging/sinks/sink_journald.h"
#include "logging/logging_severity_filter.h"
#include "options/options.h"
#include "profiling/profiling.h"
#include "profiling/profiling_controller.h"
#include "version/version.h"

// Core — kernel
#include "kernel/data_hub.h"
#include "kernel/equipment_hub.h"
#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "kernel/statistics_hub.h"

// Core — developer tools
#include "developer/firewall_manager.h"
#include "developer/mock_serial_port_impl.h"
#include "developer/recording_serial_port_impl.h"

// Core — authentication / authorization substrate
#include "application/secure_runtime_paths.h"
#include "application/service_host.h"
#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/bootstrap.h"
#include "auth/group.h"
#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/kiosk_service.h"
#include "auth/kiosk_store.h"
#include "auth/session_service.h"
#include "auth/session_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "utility/offload_pool.h"

// Core — HTTP server and routes
#include "http/server/http_server.h"
#include "http/server/static_file_handler.h"
#include "http/server/routing/routing.h"
#include "http/webroute_apikey.h"
#include "http/webroute_apikeys.h"
#include "http/webroute_auth_check.h"
#include "http/webroute_auth_login.h"
#include "http/webroute_auth_logout.h"
#include "http/webroute_auth_me.h"
#include "http/webroute_auth_pin.h"
#include "http/webroute_auth_refresh.h"
#include "http/webroute_auth_setup.h"
#include "http/webroute_diagnostics_actualdevices.h"
#include "http/webroute_diagnostics_devices.h"
#include "http/webroute_diagnostics_matter.h"
#include "http/webroute_diagnostics_mqtt.h"
#include "http/webroute_diagnostics_logging.h"
#include "http/webroute_diagnostics_options.h"
#include "http/webroute_diagnostics_profiling.h"
#include "http/webroute_diagnostics_recording.h"
#include "http/webroute_equipment.h"
#include "http/webroute_equipment_spaside_remotes.h"
#include "http/webroute_equipment_button.h"
#include "http/webroute_equipment_buttons.h"
#include "http/webroute_equipment_chlorinator.h"
#include "http/webroute_equipment_devices.h"
#include "http/webroute_equipment_iaq.h"
#include "http/webroute_equipment_heater.h"
#include "http/webroute_equipment_circulation.h"
#include "http/webroute_equipment_setpoints.h"
#include "http/webroute_equipment_version.h"
#include "http/webroute_entitlements.h"
#include "http/webroute_group.h"
#include "http/webroute_groups.h"
#include "http/webroute_health.h"
#include "http/webroute_health_detailed.h"
#include "http/webroute_kiosk.h"
#include "http/webroute_metrics.h"
#include "http/webroute_session.h"
#include "http/webroute_sessions.h"
#include "http/webroute_user.h"
#include "http/webroute_user_password.h"
#include "http/webroute_users.h"
#include "http/webroute_version.h"
#include "http/websocket_equipment.h"
#include "http/websocket_equipment_stats.h"

// Core — alerting (fault detection)
#include "alerting/alert_condition.h"
#include "alerting/alert_monitor.h"
#include "alerting/webhook_sink.h"

// Core — history (time-series persistence)
#include "history/history_service.h"
#include "http/webroute_history.h"

// Core — preferences (user/admin settings)
#include "preferences/preferences_service.h"
#include "preferences/user_preferences_store.h"
#include "http/webroute_preferences.h"

// Core — equipment cache (instant dashboard on restart)
#include "equipment_cache/equipment_cache_service.h"

// Core — scheduling (time-based automation)
#include "scheduling/controller_schedule.h"
#include "scheduling/scheduler_service.h"
#include "http/webroute_controller_schedules.h"
#include "http/webroute_schedules.h"

// Core — MQTT, serial, protocol
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_integration.h"
#include "protocol/message_generator_registry.h"
#include "protocol/protocol_thread.h"
#include "serial/port_types/network_serial_port_impl.h"
#include "serial/port_types/physical_serial_port_impl.h"
#include "serial/serial_initialise.h"
#include "serial/serial_port.h"

// Shared device capabilities
#include "devices/capabilities/restartable.h"

// Jandy protocol
#include "jandy/devices/command_dispatcher.h"
#include "jandy/devices/spaside_remote_controller.h"
#include "jandy/options/options_jandy.h"
#include "jandy/startup/jandy_startup_service.h"
#include "jandy/jandy.h"
#include "jandy/messages/jandy_message_ack.h"
#include "jandy/messages/iaq/iaq_message_control_data_response.h"
#include "jandy/protocol/jandy_protocol_registration.h"

// Pentair protocol
#include "pentair/options/options_pentair.h"
#include "pentair/protocol/pentair_protocol_registration.h"
#include "pentair/pentair.h"

#include "aqualink-automate.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

//
// The application body: the console/service-agnostic startup -> run -> shutdown
// sequence. Runs identically whether launched from a console (empty hooks) or under
// the Windows Service Control Manager (hooks report status + bridge the SCM stop into
// the ordered shutdown). Kept in this executable translation unit (NOT core) because it
// pulls in the HTTP/MQTT/Jandy/Pentair stack; the OS service host reaches it via the
// AppEntry callback passed to Application::RunHosted (see main, below).
//
static int RunApplication(int argc, char* argv[], const Application::AppHostHooks& hooks)
{
	int return_value = EXIT_FAILURE;

	try
	{

		//---------------------------------------------------------------------
		// LOGGING
		//---------------------------------------------------------------------

		Logging::SeverityFiltering::SetGlobalFilterLevel(Severity::Info);

		// Lightweight pre-scan of argv for --log-format json BEFORE options are parsed,
		// so the bootstrap console is JSON too when JSON was requested (a container
		// pipeline would otherwise choke on the pre-options text lines). The
		// authoritative, validated parse happens below; this only tilts the bootstrap.
		Logging::LogFormat bootstrap_format = Logging::LogFormat::Text;
		for (int arg_index = 1; arg_index < argc; ++arg_index)
		{
			const std::string_view arg{ argv[arg_index] };
			std::string_view value;
			if (arg == "--log-format" && (arg_index + 1) < argc)
			{
				value = argv[arg_index + 1];
			}
			else if (arg.starts_with("--log-format="))
			{
				value = arg.substr(std::string_view{ "--log-format=" }.size());
			}

			if (value == "json" || value == "JSON" || value == "Json")
			{
				bootstrap_format = Logging::LogFormat::Json;
				break;
			}
		}

		Logging::Initialise(bootstrap_format);

		//---------------------------------------------------------------------
		// PROFILING
		//---------------------------------------------------------------------

		auto& profiler = Factory::ProfilerFactory::Instance();
		auto& profiler_units = Factory::ProfilingUnitFactory::Instance();

		// Register the compiled-in backends now, but DEFER StartProfiling()/AppInfo()
		// until AFTER options are processed: the requested backend is selected by
		// SetDesired() inside the Developer options Process() step below, so calling
		// Get() here would return the NoOp profiler and StartProfiling() would be a
		// no-op (VTune/uProf would never resume capture). See the PROFILING
		// ACTIVATION block after the options block.
		Profiling::RegisterAvailableProfilers(profiler, profiler_units);

		//---------------------------------------------------------------------
		// OPTIONS
		//---------------------------------------------------------------------

		LogInfo(Channel::Options, "Configuring application options");

		Options::Settings settings;

		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> options_parsing", std::source_location::current());

			LogDebug(Channel::Options, "Parsing application options provided via command line");

			auto processed_options = Options::Initialise()
				| Add(Options::Alerting::OptionsProcessor{})
				| Add(Options::App::OptionsProcessor{})
				| Add(Options::Auth::OptionsProcessor{})
				| Add(Options::Developer::OptionsProcessor{})
				| Add(Options::Equipment::OptionsProcessor{})
				| Add(Options::History::OptionsProcessor{})
				| Add(Options::LogSinks::OptionsProcessor{})
				| Add(Options::Matter::OptionsProcessor{})
				| Add(Options::Mqtt::OptionsProcessor{})
				| Add(Options::Preferences::OptionsProcessor{})
				| Add(Options::Scheduling::OptionsProcessor{})
				| Add(Options::Serial::OptionsProcessor{})
				| Add(Options::Web::OptionsProcessor{})
				| Add(Jandy::Options::OptionsProcessor{})
				| Add(Pentair::Options::OptionsProcessor{})
				| Parse(argc, argv)
				| ParseConfigFile()
				// CheckHelpAndVersion MUST run before Validate: --help/--version
				// short-circuit (they throw OptionsHelpOrVersion, caught in main as
				// EXIT_SUCCESS) and must not be blocked by a conflict/dependency
				// validation failure on the rest of the command line.
				| CheckHelpAndVersion()
				| Validate()
				| Process(
					Options::Alerting::OptionsProcessor{},
					Options::App::OptionsProcessor{},
					Options::Auth::OptionsProcessor{},
					Options::Developer::OptionsProcessor{},
					Options::Equipment::OptionsProcessor{},
					Options::History::OptionsProcessor{},
					Options::LogSinks::OptionsProcessor{},
					Options::Matter::OptionsProcessor{},
					Options::Mqtt::OptionsProcessor{},
					Options::Preferences::OptionsProcessor{},
					Options::Scheduling::OptionsProcessor{},
					Options::Serial::OptionsProcessor{},
					Options::Web::OptionsProcessor{},
					Jandy::Options::OptionsProcessor{},
					Pentair::Options::OptionsProcessor{})
				| Finalise();

			if (!processed_options)
			{
				LogFatal(Channel::Options, std::format("Failed to process application options: {}", magic_enum::enum_name(processed_options.error())));
				return EXIT_FAILURE;
			}

			settings = processed_options.value();
		}

		//---------------------------------------------------------------------
		// LOGGING ACTIVATION
		//---------------------------------------------------------------------
		//
		// Options are known now, so replace the bootstrap console sink with the
		// resolved operational sink set (--log-sinks / --log-syslog-facility, or the
		// environment-derived `auto` policy). The audit sink is installed later, by
		// the auth bootstrap, and is independent of this.

		{
			Logging::RuntimeConfig log_runtime_config;

			// When hosted by a service manager (the Windows SCM), force the
			// service-context probe so the `auto` sink policy resolves to the Event Log
			// sink (docs/logging-sinks-redesign.md §6.2). No console is attached in that
			// case, so this is the only trail. In console mode the hook is false and the
			// environment is detected exactly as before.
			auto log_probes = Sinks::DefaultProbes();
			if (hooks.RunningAsManagedService)
			{
				log_probes.WindowsServiceContext = [] { return true; };
			}
			const auto log_environment = Sinks::DetectLogEnvironment(log_probes);

			// Whether the structured journald sink can be used: Linux with libsystemd
			// resolvable at runtime (dlopen); false elsewhere via the platform stub.
			// Passed to the auto policy so it stays free of platform/runtime probing.
			const bool journald_available = Sinks::IsJournaldAvailable();

			if (const auto logging_settings = settings.Get<Options::LogSinks::LoggingSettings>(); logging_settings.has_value())
			{
				const auto& log_settings = logging_settings.value().get();
				const bool have_log_file = log_settings.LogFile.has_value();

				if (Options::LogSinks::SinkMode::Auto == log_settings.Sinks)
				{
					log_runtime_config.Selection = Sinks::ResolveAutoSinks(log_environment, have_log_file, journald_available);
				}
				else
				{
					log_runtime_config.Selection.Console = log_settings.Console;
					log_runtime_config.Selection.Native = log_settings.Native;
					log_runtime_config.Selection.File = log_settings.File;
					log_runtime_config.Selection.Journald = log_settings.Journald;
					log_runtime_config.Selection.ConsoleJournaldPrefixes = log_settings.Console && log_environment.StderrIsJournal;
				}

				log_runtime_config.GeneralNativeFacility = log_settings.Facility;
				log_runtime_config.Format = log_settings.Format;
				log_runtime_config.LogFilePath = log_settings.LogFile;
				log_runtime_config.LogFileMaxBytes = log_settings.LogFileMaxBytes;
				log_runtime_config.LogFileMaxFiles = log_settings.LogFileMaxFiles;
			}
			else
			{
				log_runtime_config.Selection = Sinks::ResolveAutoSinks(log_environment, /* have_log_file */ false, journald_available);
			}

			Logging::Reconfigure(log_runtime_config);
		}

		//---------------------------------------------------------------------
		// PROFILING ACTIVATION
		//---------------------------------------------------------------------
		//
		// Options have now been processed, so --profiler (if supplied) has called
		// SetDesired(). Warn if the requested backend was not compiled into this
		// build (Get() would otherwise silently fall back to NoOp), then resume
		// capture and stamp the trace with build/thread metadata.

		if (const auto desired = profiler.Selected(); desired.has_value() && !profiler.IsRegistered(*desired))
		{
			std::string available;
			for (const auto type : profiler.RegisteredTypes())
			{
				available += (available.empty() ? "" : ", ");
				available += magic_enum::enum_name(type);
			}

			LogWarning(Channel::Profiling, std::format(
				"Requested profiler '{}' is not available in this build; profiling is disabled. Compiled-in backends: [{}]",
				magic_enum::enum_name(*desired),
				available.empty() ? std::string{ "none (ENABLE_PROFILING=OFF)" } : available));
		}

		profiler.Get()->StartProfiling();
		profiler.Get()->AppInfo(Version::VersionDetails());
		profiler.Get()->SetThreadName("MainLoop");

		//---------------------------------------------------------------------
		// IO CONTEXT
		//---------------------------------------------------------------------

		boost::asio::io_context io_context;

		//---------------------------------------------------------------------
		// INFORMATION DISTRIBUTION HUBS
		//---------------------------------------------------------------------

		Kernel::HubLocator hub_locator;

		auto data_hub = std::make_shared<Kernel::DataHub>();
		auto equipment_hub = std::make_shared<Kernel::EquipmentHub>();
		auto preferences_hub = std::make_shared<Kernel::PreferencesHub>();
		auto statistics_hub = std::make_shared<Kernel::StatisticsHub>();

		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> hub_initialisation", std::source_location::current());
			hub_locator.Register(data_hub).Register(equipment_hub).Register(preferences_hub).Register(statistics_hub);

			auto command_dispatcher = std::make_shared<Devices::CommandDispatcher>(data_hub, equipment_hub);
			hub_locator.Register<Interfaces::ICommandDispatcher>(command_dispatcher);

			// Spa-side remote control surface (read decoded LED/last-press state; inject button
			// presses on emulated remotes). Resolved by the /api/equipment/spaside-remotes route.
			auto spaside_controller = std::make_shared<Devices::SpasideRemoteController>(equipment_hub);
			hub_locator.Register<Interfaces::ISpasideRemoteController>(spaside_controller);

			// Runtime profiler control surface (report compiled-in/selected backends,
			// pause/resume capture). Owning handle, always registered — reports
			// enabled=false when ENABLE_PROFILING is OFF. Resolved by the
			// /api/diagnostics/profiling route.
			auto profiling_controller = std::make_shared<Profiling::ProfilingController>();
			hub_locator.Register<Interfaces::IProfilingController>(profiling_controller);
		}

		//---------------------------------------------------------------------
		// EQUIPMENT CONFIGURATION (from CLI options)
		//---------------------------------------------------------------------

		{
			auto equipment_settings_result = settings.Get<Options::Equipment::EquipmentSettings>();
			if (equipment_settings_result)
			{
				const auto& equipment_settings = equipment_settings_result.value().get();

				if (equipment_settings.pool_configuration_is_user_specified)
				{
					data_hub->ApplyPoolConfiguration(equipment_settings.pool_configuration, Kernel::ConfigurationSource::UserSpecified, equipment_settings.single_body_kind);

					LogInfo(Channel::Equipment, std::format("Pool configuration set from CLI: {} (single-body kind: {})",
						magic_enum::enum_name(equipment_settings.pool_configuration),
						magic_enum::enum_name(equipment_settings.single_body_kind)));
				}

				data_hub->TemperatureStalenessThreshold = std::chrono::seconds(equipment_settings.temperature_staleness_threshold_seconds);
			}
		}

		//---------------------------------------------------------------------
		// SUPPORTED EQUIPMENT TYPES
		//---------------------------------------------------------------------

		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> equipment_initialisation", std::source_location::current());
			Jandy::Initialise(hub_locator);
			Pentair::Initialise(hub_locator);
		}

		//---------------------------------------------------------------------
		// SERIAL PORT
		//---------------------------------------------------------------------

		std::shared_ptr<AqualinkAutomate::Serial::SerialPort> serial_port;

		{
			auto serial_settings_result = settings.Get<Options::Serial::SerialSettings>();
			auto developer_settings_result = settings.Get<Options::Developer::DeveloperSettings>();

			if (!serial_settings_result)
			{
				LogFatal(Channel::Serial, std::format("Serial settings not available: {}", serial_settings_result.error()));
				return EXIT_FAILURE;
			}
			else if (!developer_settings_result)
			{
				LogFatal(Channel::Main, std::format("Developer settings not available: {}", developer_settings_result.error()));
				return EXIT_FAILURE;
			}
			else
			{
				const auto& developer_settings = developer_settings_result.value().get();
				const auto& serial_settings = serial_settings_result.value().get();

				LogDebug(Channel::Serial, std::format("Serial settings: port='{}', remote='{}', baud={}, rfc2217={}, rawtcp={}",
					serial_settings.serial_port,
					serial_settings.remote_serial_port,
					serial_settings.baud_rate,
					serial_settings.use_rfc2217,
					serial_settings.use_rawtcp));

				auto executor = io_context.get_executor();

				// Always place the runtime-controllable recording decorator in the
				// PRODUCTION serial chain (physical AND remote).  It defaults to OFF
				// (a transparent pass-through) and is toggled at runtime via the
				// /api/diagnostics/recording route, which resolves it from the
				// HubLocator as an IRecordingController.  If `--record-serial <file>`
				// was supplied it starts recording at boot (legacy behaviour).  The
				// decorator is owned by the SerialPort (lifetime = whole app); the
				// HubLocator holds a non-owning handle (null deleter).
				auto install_recording_decorator =
					[&hub_locator, &developer_settings](std::unique_ptr<Interfaces::ISerialPortImpl> base) -> std::unique_ptr<Interfaces::ISerialPortImpl>
					{
						std::unique_ptr<AqualinkAutomate::Developer::RecordingSerialPortImpl> recorder;
						if (!developer_settings.recording_file.empty())
						{
							LogInfo(Channel::Serial, std::format("Enabling serial recording to: {}", developer_settings.recording_file));
							recorder = std::make_unique<AqualinkAutomate::Developer::RecordingSerialPortImpl>(std::move(base), developer_settings.recording_file);
						}
						else
						{
							recorder = std::make_unique<AqualinkAutomate::Developer::RecordingSerialPortImpl>(std::move(base));
						}

						// Register a non-owning handle so the diagnostics route can
						// toggle recording at runtime.  The null deleter keeps the
						// SerialPort as the sole owner.
						std::shared_ptr<Interfaces::IRecordingController> controller_handle(
							static_cast<Interfaces::IRecordingController*>(recorder.get()),
							[](Interfaces::IRecordingController*) { /* non-owning */ });
						hub_locator.Register<Interfaces::IRecordingController>(controller_handle);

						return recorder;
					};

				if ((developer_settings.dev_mode_enabled) && (!developer_settings.replay_file.empty()))
				{
					LogInfo(Channel::Main, "Enabling developer mode");

					auto serial_port_impl = std::make_unique<AqualinkAutomate::Developer::MockSerialPortImpl>();
					serial_port = std::make_shared<AqualinkAutomate::Serial::SerialPort>(std::move(serial_port_impl), hub_locator);
					serial_port->open(developer_settings.replay_file);
				}
				else if (serial_settings.UsingPhysicalSerialPort())
				{
					LogDebug(Channel::Main, std::format("Using a physical serial port ({})", serial_settings.serial_port));

					std::unique_ptr<Interfaces::ISerialPortImpl> serial_port_impl = std::make_unique<AqualinkAutomate::Serial::PortTypes::PhysicalSerialPortImpl>(executor);

					serial_port_impl = install_recording_decorator(std::move(serial_port_impl));

					serial_port = std::make_shared<AqualinkAutomate::Serial::SerialPort>(std::move(serial_port_impl), hub_locator);

					if (!AqualinkAutomate::Serial::Initialise(settings, serial_port))
					{
						LogFatal(Channel::Serial, std::format("Failed to initialise serial port '{}'; cannot continue", serial_settings.serial_port));
						return EXIT_FAILURE;
					}
				}
				else if (serial_settings.UsingRemoteSerialPort())
				{
					LogDebug(Channel::Main, std::format("Using a remote serial port ({})", serial_settings.remote_serial_port));

					// Raw TCP when either --rawtcp or --no-rfc2217 is set; otherwise the
					// RFC2217 telnet transport (the default). --rawtcp leaves use_rfc2217
					// at its true default, so both flags must be considered here.
					const bool use_rfc2217 = serial_settings.use_rfc2217 && !serial_settings.use_rawtcp;

					std::unique_ptr<Interfaces::ISerialPortImpl> serial_port_impl = std::make_unique<AqualinkAutomate::Serial::PortTypes::NetworkSerialPortImpl>(executor, use_rfc2217);

					serial_port_impl = install_recording_decorator(std::move(serial_port_impl));

					serial_port = std::make_shared<AqualinkAutomate::Serial::SerialPort>(std::move(serial_port_impl), hub_locator);

					if (!AqualinkAutomate::Serial::Initialise(settings, serial_port))
					{
						LogFatal(Channel::Serial, std::format("Failed to initialise remote serial port '{}'; cannot continue", serial_settings.remote_serial_port));
						return EXIT_FAILURE;
					}
				}
				else
				{
					LogFatal(Channel::Serial, "No serial port configured (physical, remote, or developer mode)");
					return EXIT_FAILURE;
				}
			}
		}

		//---------------------------------------------------------------------
		// PROTOCOL HANDLER
		//---------------------------------------------------------------------

		LogInfo(Channel::Main, "Starting AqualinkAutomate::ProtocolHandler...");

		// Register protocol-specific message generators before starting the handler.
		// Both are registered so Jandy and Pentair traffic can be auto-detected on
		// the same serial stream (Jandy frames on DLE/STX, Pentair on a 0xFF/0xA5
		// preamble).  The registry tries generators in priority order; the Pentair
		// generator registers at priority 0 (it is non-destructive and defers on
		// non-Pentair buffers) ahead of Jandy at priority 1.
		Jandy::Protocol::RegisterMessageGenerator();
		Pentair::Protocol::RegisterMessageGenerator();

		// Capture-replay pacing: when replaying a capture file in developer mode the
		// application's frame loop (below) steps at a fixed processing period
		// (replay_frame_period_ms, scaled by replay_speed) and the protocol task
		// reads one serial chunk per frame, so frames are delivered at roughly the
		// bus's natural inter-frame rate instead of as fast as the parser will accept
		// them.  Stays unpaced (free-running, as for real ports) when not replaying
		// or when --replay-frame-period is 0.
		std::chrono::microseconds replay_frame_period{ 0 };
		if (auto developer_settings_result = settings.Get<Options::Developer::DeveloperSettings>(); developer_settings_result)
		{
			const auto& developer_settings = developer_settings_result.value().get();
			if (developer_settings.dev_mode_enabled && !developer_settings.replay_file.empty() && (developer_settings.replay_frame_period_ms > 0))
			{
				if (developer_settings.replay_speed > 0.0)
				{
					const double effective_us = (static_cast<double>(developer_settings.replay_frame_period_ms) * 1000.0) / developer_settings.replay_speed;
					replay_frame_period = std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(effective_us));
					LogInfo(Channel::Main, std::format("Capture replay pacing enabled: {:.3g} ms/frame (period {} ms, speed {:.3g})",
						static_cast<double>(replay_frame_period.count()) / 1000.0, developer_settings.replay_frame_period_ms, developer_settings.replay_speed));
				}
				else
				{
					LogWarning(Channel::Main, std::format("Ignoring invalid --replay-speed {:.3g} (must be > 0); capture replay will be unpaced", developer_settings.replay_speed));
				}
			}
		}
		const bool replay_paced = (replay_frame_period > std::chrono::microseconds::zero());

		auto protocol_task = std::make_shared<AqualinkAutomate::Protocol::ProtocolTask>(serial_port, statistics_hub, replay_paced);
		protocol_task->ConnectWriteSignal<Messages::JandyMessage_Ack>();
		protocol_task->ConnectWriteSignal<Messages::IAQMessage_ControlDataResponse>();

		//---------------------------------------------------------------------
		// SUPPORTED EQUIPMENT
		//---------------------------------------------------------------------

		// Read-only snapshot of the controller's own internal schedules. Always
		// present (independent of --schedules-file) so the route can report a
		// status; the OneTouch/IAQ Program page processors fill it in once a
		// capture has been decoded. Registered BEFORE the equipment is configured so
		// a statically-created emulated device can resolve and populate it in its
		// constructor (auto-startup devices are created later, on the io_context).
		auto controller_schedule_store = std::make_shared<Scheduling::ControllerScheduleStore>();
		hub_locator.Register<Scheduling::ControllerScheduleStore>(controller_schedule_store);

		Jandy::Configure(hub_locator, settings);
		Pentair::Configure(hub_locator, settings);

		// Jandy auto-startup: detect the controller type/revision from the bus and stand up the
		// emulation dynamically (in place of the static --jandy-device-type set). Driven on the
		// io_context, so it advances as the run loop polls below.
		std::shared_ptr<Jandy::Startup::JandyStartupService> jandy_startup_service;
		if (auto jandy_result = settings.Get<Jandy::Options::JandySettings>(); jandy_result && jandy_result.value().get().auto_startup)
		{
			jandy_startup_service = std::make_shared<Jandy::Startup::JandyStartupService>(io_context, hub_locator, std::chrono::seconds{ jandy_result.value().get().chlorinator_setpoint_refresh_interval });
			jandy_startup_service->Start();
		}

		//---------------------------------------------------------------------
		// WEB SERVER
		//---------------------------------------------------------------------

		LogInfo(Channel::Main, "Starting AqualinkAutomate::HttpServer...");

		AqualinkAutomate::Developer::FirewallUtils::CheckAndConfigureExceptions();

		//---------------------------------------------------------------------
		// PREFERENCES SERVICE (user/admin settings persistence)
		//---------------------------------------------------------------------
		// Constructed first: it seeds the PreferencesHub from the effective CLI
		// values (so deployments behave identically) then loads any persisted
		// file (overriding the seed). The services below read the hub live.
		namespace Preferences = AqualinkAutomate::Preferences;

		std::shared_ptr<Preferences::PreferencesService> preferences_service;
		{
			Options::Preferences::PreferencesSettings prefs_settings;
			if (auto r = settings.Get<Options::Preferences::PreferencesSettings>(); r) { prefs_settings = r.value().get(); }

			Options::Alerting::AlertingSettings seed_alerting;
			if (auto r = settings.Get<Options::Alerting::AlertingSettings>(); r) { seed_alerting = r.value().get(); }
			Options::History::HistorySettings seed_history;
			if (auto r = settings.Get<Options::History::HistorySettings>(); r) { seed_history = r.value().get(); }

			preferences_service = std::make_shared<Preferences::PreferencesService>(hub_locator, prefs_settings);
			preferences_service->Seed(seed_alerting.salt_low_ppm, seed_alerting.comms_timeout_seconds, seed_alerting.webhook_url, seed_history.retention_days);
			preferences_service->Start();

			LogInfo(Channel::Main, prefs_settings.preferences_file.empty()
				? std::string{ "Preferences are in-memory only (no --preferences-file)" }
				: std::format("Preferences persisted to {}", prefs_settings.preferences_file));
		}

		//---------------------------------------------------------------------
		// EQUIPMENT CACHE (instant dashboard on restart)
		//---------------------------------------------------------------------
		// Loaded BEFORE the protocol task starts discovery so the DataHub already
		// holds last-known devices/config; live discovery then merges by label.
		namespace EquipmentCache = AqualinkAutomate::EquipmentCache;

		std::shared_ptr<EquipmentCache::EquipmentCacheService> equipment_cache_service;
		{
			Options::Equipment::EquipmentSettings eq_settings;
			if (auto r = settings.Get<Options::Equipment::EquipmentSettings>(); r) { eq_settings = r.value().get(); }

			equipment_cache_service = std::make_shared<EquipmentCache::EquipmentCacheService>(io_context, hub_locator, eq_settings);
			equipment_cache_service->Load();
			equipment_cache_service->Start();

			if (!eq_settings.equipment_cache_file.empty())
			{
				LogInfo(Channel::Main, std::format("Equipment cache file: {}", eq_settings.equipment_cache_file));
			}
		}

		//---------------------------------------------------------------------
		// HISTORY SERVICE (SQLite time-series persistence)
		//---------------------------------------------------------------------
		// Constructed before the routes so WebRoute_History can hold it. When
		// --history-db is unset the service stays null and the route returns 503.

		// Block-scope alias: bare `History` is otherwise ambiguous between the
		// AqualinkAutomate::History service namespace and
		// AqualinkAutomate::Options::History (both visible via using-directives).
		namespace History = AqualinkAutomate::History;

		std::shared_ptr<History::HistoryService> history_service;
		if (auto history_settings_result = settings.Get<Options::History::HistorySettings>(); history_settings_result)
		{
			const auto& history_settings = history_settings_result.value().get();
			if (!history_settings.db_path.empty())
			{
				try
				{
					history_service = std::make_shared<History::HistoryService>(io_context, hub_locator, history_settings);
					history_service->Start();
					LogInfo(Channel::Main, std::format("History service enabled (db: {})", history_settings.db_path));
				}
				catch (const std::exception& ex)
				{
					LogError(Channel::Main, std::format("History service failed to start (continuing without it): {}", ex.what()));
					history_service.reset();
				}
			}
		}

		//---------------------------------------------------------------------
		// SCHEDULER SERVICE (time-based automation)
		//---------------------------------------------------------------------
		// Block-scope alias: bare `Scheduling` is otherwise ambiguous between the
		// AqualinkAutomate::Scheduling service namespace and
		// AqualinkAutomate::Options::Scheduling.
		namespace Scheduling = AqualinkAutomate::Scheduling;

		std::shared_ptr<Scheduling::SchedulerService> scheduler_service;
		if (auto scheduling_settings_result = settings.Get<Options::Scheduling::SchedulingSettings>(); scheduling_settings_result)
		{
			const auto& scheduling_settings = scheduling_settings_result.value().get();
			if (!scheduling_settings.schedules_file.empty())
			{
				scheduler_service = std::make_shared<Scheduling::SchedulerService>(io_context, hub_locator, scheduling_settings);
				scheduler_service->Start();
				LogInfo(Channel::Main, std::format("Scheduler enabled (file: {})", scheduling_settings.schedules_file));
			}
		}

		auto web_settings_result = settings.Get<Options::Web::WebSettings>();

		std::unique_ptr<HTTP::HttpServer> http_server;
		std::unique_ptr<HTTP::HttpServer> https_server;
		boost::asio::ssl::context ssl_context(boost::asio::ssl::context::tls_server);

		// The identity-system stack MUST be declared at this (server) scope, NOT
		// inside the web-settings block below: the web routes registered there
		// hold these by REFERENCE (SessionService&, OffloadPool&, the stores),
		// and they have to outlive the frame loop that services requests.  A
		// tighter scope would free them at the block's end, leaving the routes
		// with dangling references (a use-after-free the moment a login/setup
		// request is served).  Fully-qualified to sidestep the block-scope
		// `Auth`/`Preferences` aliases used further down.
		std::shared_ptr<AqualinkAutomate::Preferences::UserPreferencesStore> user_preferences_store{};
		std::shared_ptr<AqualinkAutomate::Auth::UserStore> auth_users{};
		std::shared_ptr<AqualinkAutomate::Auth::GroupStore> auth_group_store{};
		std::shared_ptr<AqualinkAutomate::Auth::SessionStore> auth_sessions{};
		std::shared_ptr<AqualinkAutomate::Auth::ApiKeyStore> auth_api_keys{};
		std::shared_ptr<AqualinkAutomate::Auth::JwtCodec> auth_codec{};
		std::shared_ptr<AqualinkAutomate::Auth::AuditLog> auth_audit{};
		std::shared_ptr<AqualinkAutomate::Utility::OffloadPool> auth_offload{};
		std::shared_ptr<AqualinkAutomate::Auth::SessionService> auth_session_service{};
		std::shared_ptr<AqualinkAutomate::Auth::KioskStore> auth_kiosk{};
		std::shared_ptr<AqualinkAutomate::Auth::KioskService> auth_kiosk_service{};

		if (web_settings_result)
		{
			const auto& web_settings = web_settings_result.value().get();

			// Wire the opt-in control-plane security policy from the Web settings.
			// AuthToken unset (the default) leaves authentication disabled, so the
			// historical no-auth behaviour is preserved exactly unless the operator
			// passes --api-auth-token. The Origin allow-list (--api-allowed-origin) and
			// CSRF-header requirement (--api-require-csrf-header) are likewise off until
			// explicitly enabled.
			HTTP::Routing::SecurityConfig security_config{
				.AuthToken = web_settings.ApiAuthToken,
				.AllowedOrigins = web_settings.ApiAllowedOrigins,
				.RequireCsrfHeader = web_settings.ApiRequireCsrfHeader
			};

			// Identity system (--auth-mode) — docs/auth-redesign.md.  Disabled (the
			// default) preserves historical behaviour exactly: no subject resolution,
			// every policy decision is Permit by posture.  Enabled: the auth stores
			// are loaded from the hardened state directory, requests resolve to a
			// Subject (anonymous == the Guest group, deny-by-default; session JWTs
			// and API keys authenticate), routes are gated by the PolicyEngine, and
			// the login/refresh/logout flows come online; the legacy
			// --api-auth-token folds in as a bootstrap API key.
			//
			// Declared at this scope: the routes registered below hold references,
			// so the whole stack must live for the application lifetime.
			//
			// Block-scope alias: bare `Auth` is otherwise ambiguous between the
			// AqualinkAutomate::Auth subsystem and AqualinkAutomate::Options::Auth
			// (same collision, and same fix, as `Alerting` further down).
			namespace Auth = AqualinkAutomate::Auth;

			// The identity-system stack (user_preferences_store + auth_*) is
			// declared at the SERVER scope above so it outlives the frame loop;
			// here we only assign into it.  See the note at its declaration.
			if (auto auth_settings_result = settings.Get<Options::Auth::AuthSettings>(); auth_settings_result)
			{
				const auto& auth_settings = auth_settings_result.value().get();

				if (auth_settings.auth_mode_enabled)
				{
					security_config.AuthModeEnabled = true;

					// Resolve (and harden) the auth state directory: an explicit
					// --auth-state-dir wins, otherwise the platform's secure state
					// directory candidates (same posture as the TLS private key).
					std::filesystem::path auth_state_dir;

					if (!auth_settings.auth_state_dir.empty())
					{
						auth_state_dir = auth_settings.auth_state_dir;
						Application::PrepareSecureDirectory(auth_state_dir);
					}
					else
					{
						for (const auto& candidate : Application::SecureRuntimeStateDirectories())
						{
							if (Application::PrepareSecureDirectory(candidate / "auth"))
							{
								auth_state_dir = candidate / "auth";
								break;
							}
						}
					}

					if (auth_state_dir.empty())
					{
						LogFatal(Channel::Main, "No usable secure state directory for authentication state (--auth-state-dir)");
						return EXIT_FAILURE;
					}

					auth_users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(auth_state_dir / "users.json"));
					auth_group_store = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(auth_state_dir / "groups.json"));
					user_preferences_store = std::make_shared<Preferences::UserPreferencesStore>(Preferences::UserPreferencesStore::Load(auth_state_dir / "user_preferences.json"));
					auth_sessions = std::make_shared<Auth::SessionStore>(Auth::SessionStore::Load(auth_state_dir / "sessions.json"));
					auth_api_keys = std::make_shared<Auth::ApiKeyStore>(Auth::ApiKeyStore::Load(auth_state_dir / "api-keys.json"));
					auth_kiosk = std::make_shared<Auth::KioskStore>(Auth::KioskStore::Load(auth_state_dir / "kiosk.json"));

					// Legacy shared token keeps working as a system.admin machine key.
					if (web_settings.ApiAuthToken.has_value())
					{
						auth_api_keys->SeedBootstrapKey(*web_settings.ApiAuthToken);
					}

					auto key_store = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(auth_state_dir / "jwt-signing.key"));

					Auth::JwtCodec::Config codec_config;
					codec_config.AccessTokenTtl = std::chrono::minutes{ auth_settings.jwt_access_ttl_minutes };

					auth_codec = std::make_shared<Auth::JwtCodec>(std::move(key_store), std::move(codec_config));

					// Audit trail: OS-native sink + owner-only JSONL in the state dir.
					auth_audit = std::make_shared<Auth::AuditLog>(Auth::AuditLog::Config{ .JsonlFile = auth_state_dir / "audit.jsonl" });
					Auth::RegisterAuditOsSink();

					// argon2 runs here, never on the kernel thread.
					auth_offload = std::make_shared<Utility::OffloadPool>(1);

					auth_session_service = std::make_shared<Auth::SessionService>(
						auth_users, auth_group_store, auth_sessions, auth_codec, *auth_offload, *auth_audit, Auth::SessionService::Config{});

					// Kiosk PIN elevation (guest mode): shares the offload pool,
					// codec, group and session stores with local sessions.
					auth_kiosk_service = std::make_shared<Auth::KioskService>(
						auth_kiosk, auth_group_store, auth_sessions, auth_codec, *auth_offload, *auth_audit, Auth::KioskService::Config{});

					HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
						.Groups = auth_group_store->SharedRegistry(),
						.Codec = auth_codec,
						.Users = auth_users,
						.ApiKeys = auth_api_keys,
						.Kiosk = auth_kiosk }));

					// Headless first-admin bootstrap (--bootstrap-admin).  The kernel
					// loop is not running yet, so the synchronous argon2 hash here is
					// acceptable.  Idempotent: with any user on file it is a no-op.
					if (!auth_settings.bootstrap_admin_username.empty())
					{
						if (!auth_users->Empty())
						{
							LogDebug(Channel::Main, "--bootstrap-admin ignored: users already exist (setup is complete)");
						}
						else
						{
							std::string bootstrap_password;

							if (!auth_settings.bootstrap_admin_password_file.empty())
							{
								std::ifstream password_file(auth_settings.bootstrap_admin_password_file);
								std::getline(password_file, bootstrap_password);
							}
							else if (const char* env_password = std::getenv("AQUALINK_BOOTSTRAP_ADMIN_PASSWORD"); nullptr != env_password)
							{
								bootstrap_password = env_password;
							}

							// Trim trailing CR/whitespace (CRLF password files).
							while (!bootstrap_password.empty() && (('\r' == bootstrap_password.back()) || (' ' == bootstrap_password.back()) || ('\t' == bootstrap_password.back())))
							{
								bootstrap_password.pop_back();
							}

							std::string bootstrap_error;

							if (bootstrap_password.empty())
							{
								LogWarning(Channel::Main, "--bootstrap-admin supplied but no password found (--bootstrap-admin-password-file or AQUALINK_BOOTSTRAP_ADMIN_PASSWORD); no administrator created");
							}
							else if (!Auth::BootstrapAdmin(*auth_users, auth_settings.bootstrap_admin_username, bootstrap_password, Auth::PasswordHasher::Params{}, *auth_audit, bootstrap_error).has_value())
							{
								LogWarning(Channel::Main, [&] { return std::format("Bootstrap administrator was not created: {}", bootstrap_error); });
							}
							else
							{
								LogInfo(Channel::Main, [&] { return std::format("Bootstrap administrator '{}' created", auth_settings.bootstrap_admin_username); });
							}
						}
					}

					LogInfo(Channel::Main, [&] { return std::format("Identity system enabled (auth-mode=enabled); auth state in '{}'; {} user(s) on file", auth_state_dir.string(), auth_users->Size()); });
				}
			}

			// Open-control-plane guard: the equipment-control API actuates pumps,
			// heaters and chlorinators. Binding a non-loopback interface with NO auth
			// token exposes that to anyone on the network. We do not change behaviour
			// (non-breaking), but we MUST make the operator aware: emit a prominent
			// warning unless they bound loopback, enabled some security, or explicitly
			// acknowledged the open posture with --insecure-no-auth.
			{
				boost::system::error_code addr_ec;
				const auto bound = boost::asio::ip::make_address(web_settings.bind_address, addr_ec);
				const bool non_loopback = addr_ec ? (web_settings.bind_address != "127.0.0.1" && web_settings.bind_address != "::1")
												  : !bound.is_loopback();

				if (non_loopback && !security_config.IsEnabled())
				{
					if (web_settings.InsecureNoAuthAck)
					{
						LogInfo(Channel::Web, [&] { return std::format(
							"API bound to non-loopback address '{}' without authentication (acknowledged via --insecure-no-auth)", web_settings.bind_address); });
					}
					else
					{
						LogWarning(Channel::Web, [&] { return std::format(
							"SECURITY: the equipment-control API is bound to non-loopback address '{}' with NO authentication - "
							"anyone who can reach this host can actuate pool equipment. Set --api-auth-token (and serve over HTTPS), "
							"or pass --insecure-no-auth to acknowledge this is intentional (e.g. behind a trusted reverse proxy).",
							web_settings.bind_address); });
					}
				}

				// Weak-token guard: the auth comparison is constant-time, but a short or
				// low-entropy token is still feasible to brute-force online (there is no
				// per-IP lockout). Nudge the operator toward a long random secret. The
				// token value itself is never logged.
				if (web_settings.ApiAuthToken.has_value() && web_settings.ApiAuthToken->size() < 16)
				{
					LogWarning(Channel::Web, "SECURITY: --api-auth-token is shorter than 16 characters; use a long, random token (e.g. 32+ chars) to resist online brute-force guessing");
				}
			}

			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthCheck>());
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthMe>(
				[users = auth_users]() { return (nullptr != users) && users->Empty(); },
				[kiosk = auth_kiosk]() { return (nullptr != kiosk) && kiosk->Enabled(); }));

			// Session flows exist only under the identity system: with auth-mode
			// disabled there are no accounts to log into and the routes would
			// answer 503-shaped errors; not registering them keeps the legacy
			// surface byte-identical.
			if (auth_session_service)
			{
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogin>(*auth_session_service, io_context.get_executor()));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthRefresh>(*auth_session_service));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthLogout>(*auth_session_service));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthSetup>(*auth_users, *auth_audit, *auth_offload, Auth::PasswordHasher::Params{}, io_context.get_executor()));

				// Kiosk PIN elevation (guest mode, D16): the public PIN-login
				// endpoint and the system.admin config surface.
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_AuthPin>(*auth_kiosk_service, io_context.get_executor()));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Kiosk>(*auth_kiosk_service, io_context.get_executor()));

				// The admin/user-management surface (docs/auth-redesign.md §6-§7):
				// users, groups, entitlement vocabulary, API keys and sessions.
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Users>(*auth_users, *auth_audit, *auth_offload, Auth::PasswordHasher::Params{}, io_context.get_executor()));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_User>(*auth_users, *auth_group_store, *auth_session_service, *auth_sessions, *auth_audit, user_preferences_store.get()));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_UserPassword>(*auth_users, *auth_group_store, *auth_sessions, *auth_audit, *auth_offload, Auth::PasswordHasher::Params{}, io_context.get_executor()));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Groups>(*auth_group_store, *auth_users, *auth_audit));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Group>(*auth_group_store, *auth_users, *auth_audit));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Entitlements>());
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKeys>(*auth_api_keys, *auth_audit));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKey>(*auth_api_keys, *auth_audit));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Sessions>(*auth_sessions));
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Session>(*auth_sessions, *auth_audit));
			}
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Devices>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Mqtt>(hub_locator));
			{
				// The Matter stack runs in a sidecar; this route proxies its status/QR.
				bool matter_enabled{ true };
				uint16_t matter_status_port{ 8099 };
				if (auto matter_settings_result = settings.Get<Options::Matter::MatterSettings>(); matter_settings_result)
				{
					const auto& matter_settings = matter_settings_result.value().get();
					matter_enabled = matter_settings.enabled;
					matter_status_port = matter_settings.status_port;
				}
				HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Matter>(matter_enabled, matter_status_port));
			}
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_ActualDevices>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Logging>());
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Options>());
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Profiling>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Diagnostics_Recording>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Button>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Buttons>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Chlorinator>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_IAQ>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Heater>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Circulation>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Devices>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Setpoints>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_SpasideRemotes>(hub_locator, preferences_service));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Equipment_Version>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Health>());
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_HealthDetailed>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_History>(history_service));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Metrics>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Preferences>(preferences_service, user_preferences_store));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Schedule>(scheduler_service, data_hub));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Schedules>(scheduler_service, data_hub));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ControllerSchedules>(controller_schedule_store));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Version>());

			HTTP::Routing::Add(std::make_unique<HTTP::WebSocket_Equipment>(hub_locator));
			HTTP::Routing::Add(std::make_unique<HTTP::WebSocket_Equipment_Stats>(hub_locator));

			// Honour --disable-content: when set, serve ONLY the APIs and register no
			// static-file handler.  Static assets are served UNauthenticated by design
			// (so a login page can load before a token is supplied), so an operator who
			// hardens an API-only deployment with --disable-content must actually get an
			// empty doc-root surface -- previously this flag was parsed but never read,
			// silently leaving the whole doc-root world-readable.
			if (!web_settings.http_content_is_disabled)
			{
				HTTP::Routing::StaticHandler(HTTP::StaticFileHandler("/", web_settings.doc_root));
			}
			else
			{
				LogInfo(Channel::Web, "Static content serving disabled (--disable-content); only API routes are registered");
			}

			if (web_settings.https_server_is_enabled)
			{
				ssl_context.set_options(
					boost::asio::ssl::context::default_workarounds |
					boost::asio::ssl::context::no_sslv2 |
					boost::asio::ssl::context::no_sslv3 |
					boost::asio::ssl::context::no_tlsv1 |
					boost::asio::ssl::context::no_tlsv1_1 |
					boost::asio::ssl::context::single_dh_use |
					static_cast<long>(SSL_OP_CIPHER_SERVER_PREFERENCE)
				);

				Certificates::LoadSslCertificates(web_settings, ssl_context);

				auto endpoint = boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address(web_settings.bind_address), web_settings.https_port };
				https_server = std::make_unique<HTTP::HttpServer>(io_context, endpoint, std::ref(ssl_context), security_config);
				https_server->Start();
			}

			if (web_settings.http_server_is_enabled)
			{
				auto endpoint = boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address(web_settings.bind_address), web_settings.http_port };
				http_server = std::make_unique<HTTP::HttpServer>(io_context, endpoint, std::nullopt, security_config);
				http_server->Start();
			}
		}

		//---------------------------------------------------------------------
		// MQTT SERVICE
		//---------------------------------------------------------------------

		std::shared_ptr<AqualinkAutomate::Mqtt::MqttIntegration> mqtt_integration;

		auto mqtt_settings_result = settings.Get<Options::Mqtt::MqttSettings>();
		if (mqtt_settings_result)
		{
			const auto& mqtt_settings = mqtt_settings_result.value().get();

			if (mqtt_settings.enabled)
			{
				LogInfo(Channel::Main, "Starting AqualinkAutomate::MqttService...");

				mqtt_integration = std::make_shared<AqualinkAutomate::Mqtt::MqttIntegration>(io_context, mqtt_settings);
				mqtt_integration->ConnectHubs(data_hub, equipment_hub, statistics_hub);
				mqtt_integration->Start();

				// Expose to the HTTP layer so /api/diagnostics/mqtt can read live status.
				hub_locator.Register(mqtt_integration);

				LogInfo(Channel::Main, std::format("MQTT service enabled, publishing to {}:{}", mqtt_settings.broker_host, mqtt_settings.broker_port));
			}
			else
			{
				LogInfo(Channel::Main, "MQTT service is disabled");
			}
		}
		else
		{
			LogInfo(Channel::Main, "MQTT service is disabled");
		}

		//---------------------------------------------------------------------
		// FRAME LOOP
		//---------------------------------------------------------------------

		// ----- Alert monitor (fault detection + alerting) --------------------
		// Block-scope alias: bare `Alerting` is otherwise ambiguous between the
		// AqualinkAutomate::Alerting subsystem and AqualinkAutomate::Options::Alerting
		// (both visible via `using namespace AqualinkAutomate`). This local
		// declaration takes lookup precedence over the using-directive names.
		namespace Alerting = AqualinkAutomate::Alerting;

		Options::Alerting::AlertingSettings alerting_settings;
		if (auto alerting_settings_result = settings.Get<Options::Alerting::AlertingSettings>(); alerting_settings_result)
		{
			alerting_settings = alerting_settings_result.value().get();
		}

		// Webhook sink — declared before the monitor so it outlives it. It reads
		// the URL LIVE from preferences (seeded from --alert-webhook-url), so the
		// webhook can be enabled/changed at runtime; an empty URL is a no-op.
		auto webhook_sink = std::make_unique<Alerting::WebhookSink>(io_context,
			[preferences_hub]() -> std::string { return preferences_hub ? preferences_hub->AlertWebhookUrl : std::string{}; });

		Alerting::AlertMonitor alert_monitor(io_context, hub_locator, alerting_settings);

		// UI sink (always): broadcast every transition to /ws/equipment clients.
		alert_monitor.AddSink([equipment_hub](const Alerting::AlertTransition& transition)
		{
			equipment_hub->AlertTransitionSignal(transition.condition, transition.raised, transition.ts, transition.detail, transition.params);
		});

		// Webhook sink (always added; a no-op while the preference URL is empty).
		alert_monitor.AddSink([&webhook_sink](const Alerting::AlertTransition& transition)
		{
			webhook_sink->Post(transition);
		});

		// MQTT / Home Assistant sink: publish the consolidated alert state
		// (retained) that the HA problem binary_sensors read.  Only when MQTT is
		// active; the binary_sensor discovery itself is emitted by HA discovery.
		if (mqtt_integration)
		{
			if (auto mqtt_hub = mqtt_integration->GetMqttHub(); mqtt_hub)
			{
				if (auto mqtt_client = mqtt_hub->GetMqttClient(); mqtt_client)
				{
					const auto alert_topic = mqtt_client->BuildTopic(std::string{ Alerting::AlertStateSubtopic });

					alert_monitor.AddSink([&alert_monitor, mqtt_client, alert_topic](const Alerting::AlertTransition&)
					{
						mqtt_client->Publish(alert_topic, alert_monitor.BuildStateJson().dump(), /*retain=*/true);
					});

					// Seed the retained state so the HA entities start with a value.
					mqtt_client->Publish(alert_topic, alert_monitor.BuildStateJson().dump(), /*retain=*/true);
				}
			}
		}

		alert_monitor.Start();

		LogInfo(Channel::Main, "Starting AqualinkAutomate...");
		profiler.Get()->Message("Application starting", static_cast<uint32_t>(Profiling::UnitColours::Green));

		using clock = std::chrono::steady_clock;
		// One application frame/step.  Real-time operation steps at ~1 ms with an
		// adaptive skip (see below) for low response latency; capture replay steps
		// at the fixed processing period so playback runs at the bus's natural rate.
		const auto frame_period = replay_paced ? replay_frame_period : std::chrono::microseconds(1000);

		bool shutdown = false;
		boost::asio::signal_set shutdown_signals(io_context, SIGINT, SIGTERM);
		shutdown_signals.async_wait([&shutdown](const boost::system::error_code&, int signum)
		{
			LogInfo(Channel::Main, std::format("Received shutdown signal ({})", signum));
			shutdown = true;
		});

		// Bridge an OS service-manager stop (the Windows SCM control handler, which runs
		// on a different thread) into the SAME ordered shutdown as SIGINT/SIGTERM: it
		// posts the shutdown flag onto the io_context so the write happens on this thread
		// at the next poll(), mirroring the signal_set handler exactly. The RAII guard
		// clears the published requester on EVERY exit path (normal return and exception
		// unwind) BEFORE io_context is destroyed, so a late stop can never post into a
		// dead io_context. No-op in console mode (PublishStopRequester is empty).
		struct StopRequesterGuard
		{
			const Application::AppHostHooks& hooks;
			~StopRequesterGuard() { if (hooks.PublishStopRequester) { hooks.PublishStopRequester(nullptr); } }
		} stop_requester_guard{ hooks };

		if (hooks.PublishStopRequester)
		{
			hooks.PublishStopRequester([&io_context, &shutdown]()
			{
				boost::asio::post(io_context, [&shutdown]() { shutdown = true; });
			});
		}

		profiler.Get()->EmitFrameMark("MainLoop");

		// Startup complete and now serving: tell the service manager we are RUNNING
		// (no-op in console mode).
		if (hooks.OnRunning)
		{
			hooks.OnRunning();
		}

		while (!shutdown)
		{
			auto frame_start = clock::now();

			// Each per-iteration subsystem step below is wrapped in its own named
			// profiling zone so a "MainLoop" frame can be decomposed by subsystem
			// (io / protocol / watchdogs / http / https / mqtt) in the profiler.

			// Process any Asio handlers already pending at frame start (signal_set,
			// inbound HTTP/WS reads, MQTT socket completions, etc.).  io_context::poll
			// returns the number of handlers it ran this call.
			std::size_t handlers_run = 0;
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> io_poll", std::source_location::current());
				handlers_run = io_context.poll();
			}

			// Advance subsystems
			bool had_work = false;
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> protocol_poll", std::source_location::current());
				had_work = protocol_task->Poll();
			}

			// Drive per-device watchdog deadline checks.
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> watchdogs", std::source_location::current());
				Devices::Capabilities::Restartable::PollAll();
			}

			// The HTTP/WS/MQTT Poll() calls queue fresh async work (notably
			// WebSocket outbound writes).  Drain it now so egress is not capped at
			// one message per frame, and so its handler count contributes to the
			// idle decision below.
			if (http_server)
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> http_poll", std::source_location::current());
				http_server->Poll();
			}

			if (https_server)
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> https_poll", std::source_location::current());
				https_server->Poll();
			}

			if (mqtt_integration)
			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> mqtt_poll", std::source_location::current());
				mqtt_integration->Poll();
			}

			{
				auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("main -> io_poll_drain", std::source_location::current());
				handlers_run += io_context.poll();
			}

			// Any Asio handler that ran (HTTP/WebSocket/MQTT activity) counts as
			// work for pacing, so network activity keeps the loop hot rather than
			// being throttled to one frame period per message.
			had_work = had_work || (handlers_run > 0);

			// Pace the frame.  Capture replay always sleeps to the processing
			// period (a fixed step, so playback runs at the bus rate).  Real-time
			// operation sleeps only when fully idle — when the protocol task or any
			// subsystem had work we loop straight back for near-zero response
			// latency under load.
			if (replay_paced || !had_work)
			{
				std::this_thread::sleep_until(frame_start + frame_period);
			}

			// Plot the per-frame Asio handler count so idle vs busy frames are
			// visible alongside the MainLoop frame marker on the profiler timeline.
			profiler.Get()->PlotValue("Main Loop Handlers", static_cast<int64_t>(handlers_run));
			profiler.Get()->EmitFrameMark("MainLoop");
		}

		//---------------------------------------------------------------------
		// STOP APPLICATION
		//---------------------------------------------------------------------

		// Ordered shutdown begins (console Ctrl-C or a service stop): tell the service
		// manager we are STOP_PENDING before the teardown below (no-op in console mode).
		if (hooks.OnStopPending)
		{
			hooks.OnStopPending();
		}

		LogInfo(Channel::Main, "Stopping AqualinkAutomate...");
		profiler.Get()->Message("Application shutting down", static_cast<uint32_t>(Profiling::UnitColours::Orange));

		// 1. Cancel serial port to unblock the protocol task read loop.
		if (serial_port && serial_port->is_open())
		{
			boost::system::error_code cancel_ec;
			serial_port->cancel(cancel_ec);
		}

		// 2. Release protocol task resources.
		protocol_task.reset();

		// Stop the alert monitor (cancels its timer + disconnects hub signals)
		// before tearing down the MQTT client its sink may reference.
		alert_monitor.Stop();

		// Flush + close the history database (also done by its destructor).
		if (history_service)
		{
			history_service->Stop();
			history_service.reset();
		}

		// Stop the Jandy auto-startup timer.
		if (jandy_startup_service)
		{
			jandy_startup_service->Stop();
			jandy_startup_service.reset();
		}

		// Stop the scheduler timer.
		if (scheduler_service)
		{
			scheduler_service->Stop();
			scheduler_service.reset();
		}

		// Cancel the cache timer and write a final equipment snapshot.
		if (equipment_cache_service)
		{
			equipment_cache_service->Stop();
			equipment_cache_service.reset();
		}

		// 3. Stop MQTT integration
		if (mqtt_integration)
		{
			LogInfo(Channel::Main, "Stopping MQTT service...");
			mqtt_integration->Stop();
			mqtt_integration.reset();
		}

		// 4. Stop HTTP servers
		if (https_server)
		{
			https_server->Stop();
			https_server.reset();
		}
		if (http_server)
		{
			http_server->Stop();
			http_server.reset();
		}

		// 5. Clear HTTP routing tables (destroys the diagnostics-recording route,
		//    which cached the non-owning IRecordingController handle).
		LogInfo(Channel::Main, "Clearing HTTP routing tables...");
		HTTP::Routing::Clear();

		// 6. Drop the non-owning IRecordingController handle and tear the serial
		//    chain down deterministically.  The handle registered with the
		//    HubLocator is a null-deleter shared_ptr aliasing a SerialPort-owned
		//    RecordingSerialPortImpl, so it MUST be removed before the SerialPort
		//    (and the object it aliases) is destroyed — otherwise the HubLocator
		//    would briefly hold a dangling alias during scope-exit destruction.
		hub_locator.Unregister<Interfaces::IRecordingController>();
		hub_locator.Unregister<Interfaces::IProfilingController>();
		serial_port.reset();

		// 7. Clear message generator registry
		LogInfo(Channel::Main, "Clearing message generator registry...");
		Protocol::MessageGeneratorRegistry::Instance().Clear();

		// 8. Stop profiling last
		profiler.Get()->StopProfiling();

		// 9. Flush and remove logging sinks last of all, so every prior shutdown
		//    message is delivered (and any async sink frontend drained).
		LogInfo(Channel::Main, "Shutting down logging...");
		Logging::Shutdown();

		return_value = EXIT_SUCCESS;
	}
	catch (const Exceptions::OptionsHelpOrVersion&)
	{
		return_value = EXIT_SUCCESS;
	}
	catch (const Exceptions::OptionParsingFailed&)
	{
		return_value = EXIT_SUCCESS;
	}
	catch (const Exceptions::GenericAqualinkException& ex_gae)
	{
		const auto& sl = ex_gae.Where();
		const auto& st = ex_gae.StackTrace();

		LogFatal(
			Channel::Main,
			std::format(
				"Unknown exception occurred at {} ({}:{})...terminating!  Message: {}",
				sl.file_name(),
				sl.line(),
				sl.column(),
				ex_gae.What()
			)
		);

		for (const auto& frame : st)
		{
			LogDebug(Channel::Main, std::format("{}, {}({})", frame.name(), frame.source_file().empty() ? "Unknown File" : frame.source_file(), frame.source_line()));
		}
	}
	catch (const boost::system::system_error& err)
	{
		LogFatal(Channel::Main, std::format("Unknown exception occurred...terminating!  Message: {}", err.what()));
	}
	catch (const std::exception& err)
	{
		LogFatal(Channel::Main, std::format("Unknown exception occurred...terminating!  Message: {}", err.what()));

		const auto trace = boost::stacktrace::stacktrace();
		for (const auto& frame : trace)
		{
			LogDebug(Channel::Main, std::format("{}, {}({})", frame.name(), frame.source_file().empty() ? "Unknown File" : frame.source_file(), frame.source_line()));
		}

	}

	return return_value;
}

int main(int argc, char* argv[])
{
	// Run the application body, wrapped in the platform's service host where one exists.
	// On Windows this attempts Service Control Manager dispatch and, when the process was
	// NOT launched by the SCM, falls through to a direct console run. On POSIX it calls
	// RunApplication directly with empty hooks. The console path is unchanged from before
	// this seam was introduced.
	return AqualinkAutomate::Application::RunHosted(argc, argv, &RunApplication);
}
