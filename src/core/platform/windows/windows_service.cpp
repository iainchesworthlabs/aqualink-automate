#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "application/log_source_registration.h"
#include "application/service_host.h"

//
// Windows Service Control Manager (SCM) host for the application. OS-specific, so it
// lives in the platform/ tree (wired by if(WIN32) in src/core/CMakeLists.txt) rather
// than behind an #ifdef in a shared source. It contains NO application includes: the
// application body is handed in as an AqualinkAutomate::Application::AppEntry callback,
// so this translation unit depends only on the shared service_host.h + Win32.
//
// Run-mode detection: StartServiceCtrlDispatcher blocks for the whole service lifetime
// and fails immediately with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT when the process
// was NOT launched by the SCM (i.e. a console run). That failure is the console/service
// discriminator (RunHosted below).
//

namespace AqualinkAutomate::Application
{

	namespace
	{
		// The service key name; MUST match the Event Log source registered by
		// RegisterLogSource and named by the runtime Event Log sink (native_log_sink.cpp).
		const wchar_t* const SERVICE_NAME{ L"Aqualink-Automate" };
		const wchar_t* const SERVICE_DISPLAY_NAME{ L"Aqualink Automate" };
		const wchar_t* const SERVICE_DESCRIPTION{ L"Bridges a Jandy/Zodiac AquaLink RS pool controller to MQTT/Home Assistant and a local web UI." };

		// Startup can take a while (serial probe, TLS, MQTT connect) and runs
		// synchronously with no SCM pumping until RUNNING is reported; give the SCM a
		// generous hint so it does not consider the start hung.
		constexpr DWORD START_WAIT_HINT_MS{ 30000 };
		constexpr DWORD STOP_WAIT_HINT_MS{ 30000 };

		//
		// The single rendezvous between the SCM callbacks (ServiceMain / the control
		// handler, which run on SCM-dispatched threads) and the application body.
		// ServiceMain is a C callback with no user-data parameter, so a file-scope holder
		// is required. All fields are guarded by `mutex`.
		//
		struct ServiceControl
		{
			std::mutex mutex;
			SERVICE_STATUS_HANDLE status_handle{ nullptr };
			DWORD current_state{ SERVICE_STOPPED };
			DWORD checkpoint{ 0 };

			// The application body + the ORIGINAL process argv (the service binary path,
			// which carries --config). ServiceMain uses these, NOT its own argv (which
			// holds only StartService arguments).
			const AppEntry* entry{ nullptr };
			int argc{ 0 };
			char** argv{ nullptr };
			int exit_code{ EXIT_FAILURE };

			// Published by the application body once its io_context exists; invoked by the
			// control handler to trigger the same ordered shutdown as a console Ctrl-C.
			std::function<void()> request_stop;
		};

		ServiceControl& Control()
		{
			static ServiceControl control;
			return control;
		}

		// Report a service state to the SCM. Pending states advance the checkpoint and
		// carry a wait hint; steady states reset it. Accepts stop/shutdown only while
		// RUNNING. Caller must hold ServiceControl::mutex.
		void ReportStatusLocked(ServiceControl& control, DWORD state, DWORD wait_hint = 0, DWORD win32_exit_code = NO_ERROR)
		{
			control.current_state = state;

			SERVICE_STATUS status{};
			status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
			status.dwCurrentState = state;
			status.dwControlsAccepted = (SERVICE_RUNNING == state) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
			status.dwWin32ExitCode = win32_exit_code;
			status.dwServiceSpecificExitCode = 0;
			status.dwWaitHint = wait_hint;

			const bool is_pending = (SERVICE_START_PENDING == state) || (SERVICE_STOP_PENDING == state);
			status.dwCheckPoint = is_pending ? ++control.checkpoint : (control.checkpoint = 0);

			if (nullptr != control.status_handle)
			{
				::SetServiceStatus(control.status_handle, &status);
			}
		}

		void ReportStatus(DWORD state, DWORD wait_hint = 0, DWORD win32_exit_code = NO_ERROR)
		{
			auto& control = Control();
			std::lock_guard<std::mutex> lock(control.mutex);
			ReportStatusLocked(control, state, wait_hint, win32_exit_code);
		}

		DWORD WINAPI ServiceControlHandler(DWORD control_code, DWORD /*event_type*/, LPVOID /*event_data*/, LPVOID /*context*/)
		{
			switch (control_code)
			{
			case SERVICE_CONTROL_STOP:
			case SERVICE_CONTROL_SHUTDOWN:
			{
				// Move to STOP_PENDING immediately, then trigger the app's ordered
				// shutdown. Copy the requester out from under the lock before invoking it
				// (it does a thread-safe boost::asio::post; keep the lock scope tight).
				ReportStatus(SERVICE_STOP_PENDING, STOP_WAIT_HINT_MS);

				std::function<void()> stopper;
				{
					auto& control = Control();
					std::lock_guard<std::mutex> lock(control.mutex);
					stopper = control.request_stop;
				}
				if (stopper)
				{
					stopper();
				}
				return NO_ERROR;
			}

			case SERVICE_CONTROL_INTERROGATE:
				// SCM asks us to re-report our current state.
				{
					auto& control = Control();
					std::lock_guard<std::mutex> lock(control.mutex);
					ReportStatusLocked(control, control.current_state);
				}
				return NO_ERROR;

			default:
				return ERROR_CALL_NOT_IMPLEMENTED;
			}
		}

		void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
		{
			auto& control = Control();

			const SERVICE_STATUS_HANDLE handle = ::RegisterServiceCtrlHandlerExW(SERVICE_NAME, &ServiceControlHandler, nullptr);
			if (nullptr == handle)
			{
				// Cannot talk to the SCM; nothing more we can do here.
				return;
			}

			AppEntry entry_copy;
			int argc = 0;
			char** argv = nullptr;
			{
				std::lock_guard<std::mutex> lock(control.mutex);
				control.status_handle = handle;
				control.checkpoint = 0;
				if (nullptr != control.entry)
				{
					entry_copy = *control.entry;
				}
				argc = control.argc;
				argv = control.argv;
			}

			ReportStatus(SERVICE_START_PENDING, START_WAIT_HINT_MS);

			AppHostHooks hooks;
			hooks.RunningAsManagedService = true;
			hooks.OnRunning = [] { ReportStatus(SERVICE_RUNNING); };
			hooks.OnStopPending = [] { ReportStatus(SERVICE_STOP_PENDING, STOP_WAIT_HINT_MS); };
			hooks.PublishStopRequester = [](std::function<void()> request_stop)
			{
				auto& c = Control();
				std::lock_guard<std::mutex> lock(c.mutex);
				c.request_stop = std::move(request_stop);
			};

			int exit_code = EXIT_FAILURE;
			if (entry_copy)
			{
				// Runs the whole application body on this (SCM-dispatched) thread. The
				// body reports RUNNING via hooks.OnRunning once it is serving, and
				// STOP_PENDING via hooks.OnStopPending as teardown begins.
				exit_code = entry_copy(argc, argv, hooks);
			}

			{
				std::lock_guard<std::mutex> lock(control.mutex);
				control.exit_code = exit_code;
				control.request_stop = nullptr;
			}

			ReportStatus(SERVICE_STOPPED, 0, (EXIT_SUCCESS == exit_code) ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
		}

		// --- Install / uninstall helpers -------------------------------------------

		std::string Narrow(const std::wstring& text)
		{
			if (text.empty())
			{
				return {};
			}

			const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring ModuleFilePath()
		{
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (0 == length)
				{
					return {};
				}
				if (length < buffer.size())
				{
					buffer.resize(length);
					return buffer;
				}
				buffer.resize(buffer.size() * 2);  // ERROR_INSUFFICIENT_BUFFER: grow and retry.
			}
		}

		std::wstring EnvironmentVariable(const wchar_t* name)
		{
			const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
			if (0 == needed)
			{
				return {};
			}
			std::wstring value(needed, L'\0');
			const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
			value.resize(written);
			return value;
		}

		// Quote an argument for a command line if it contains whitespace or quotes.
		std::wstring QuoteArg(const std::wstring& arg)
		{
			if (!arg.empty() && (std::wstring::npos == arg.find_first_of(L" \t\"")))
			{
				return arg;
			}

			std::wstring quoted{ L'"' };
			for (auto it = arg.begin(); ; ++it)
			{
				size_t backslashes = 0;
				while ((it != arg.end()) && (L'\\' == *it))
				{
					++it;
					++backslashes;
				}

				if (it == arg.end())
				{
					quoted.append(backslashes * 2, L'\\');  // escape trailing backslashes before the closing quote
					break;
				}
				else if (L'"' == *it)
				{
					quoted.append(backslashes * 2 + 1, L'\\');
					quoted.push_back(L'"');
				}
				else
				{
					quoted.append(backslashes, L'\\');
					quoted.push_back(*it);
				}
			}
			quoted.push_back(L'"');
			return quoted;
		}

		// The remaining process arguments with the exe and the install/uninstall action
		// flags stripped -- i.e. exactly the runtime flags the operator supplied alongside
		// --install-service, to be baked into the service binary path.
		std::vector<std::wstring> PassthroughArgs()
		{
			std::vector<std::wstring> args;

			int argc = 0;
			LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
			if (nullptr == argv)
			{
				return args;
			}

			for (int i = 1; i < argc; ++i)  // skip argv[0] (the executable)
			{
				const std::wstring arg{ argv[i] };
				if ((L"--install-service" == arg) || (L"--uninstall-service" == arg))
				{
					continue;
				}
				args.push_back(arg);
			}

			::LocalFree(argv);
			return args;
		}

		bool HasConfigArg(const std::vector<std::wstring>& args)
		{
			for (const auto& arg : args)
			{
				if ((L"--config" == arg) || (L"-c" == arg) || arg.starts_with(L"--config="))
				{
					return true;
				}
			}
			return false;
		}

		// %ProgramData%\Aqualink Automate -- the machine-wide, LocalService-readable
		// config/state home (the Windows analogue of /etc/aqualink-automate).
		std::filesystem::path ProgramDataDirectory()
		{
			std::wstring program_data = EnvironmentVariable(L"ProgramData");
			if (program_data.empty())
			{
				program_data = EnvironmentVariable(L"ALLUSERSPROFILE");
			}
			if (program_data.empty())
			{
				return {};
			}
			return std::filesystem::path{ program_data } / L"Aqualink Automate";
		}

		// Ensure a default config exists at the given path (an all-comment starter, which
		// parses to zero options == pure defaults, so the service starts cleanly and the
		// operator has a file to edit). Never overwrites an existing file.
		void EnsureStarterConfig(const std::filesystem::path& config_path)
		{
			std::error_code ec;
			if (std::filesystem::exists(config_path, ec))
			{
				return;
			}

			std::filesystem::create_directories(config_path.parent_path(), ec);

			std::ofstream file(config_path);
			if (file)
			{
				file <<
					"# Aqualink Automate - Windows service configuration\n"
					"#\n"
					"# Keys are option long-names (flat INI, no sections); see docs/configuration.md\n"
					"# or run: aqualink-automate --help\n"
					"#\n"
					"# Example:\n"
					"#   serial-port = COM3\n"
					"#   http-port   = 80\n"
					"#   mqtt-broker-host = 192.168.1.10\n";
			}
		}
	}
	// namespace (anonymous)

	int RunHosted(int argc, char** argv, const AppEntry& entry)
	{
		auto& control = Control();
		{
			std::lock_guard<std::mutex> lock(control.mutex);
			control.entry = &entry;
			control.argc = argc;
			control.argv = argv;
			control.exit_code = EXIT_FAILURE;
		}

		SERVICE_TABLE_ENTRYW table[] =
		{
			{ const_cast<LPWSTR>(SERVICE_NAME), &ServiceMain },
			{ nullptr, nullptr }
		};

		if (::StartServiceCtrlDispatcherW(table))
		{
			// Launched by the SCM: the dispatcher returned after the service stopped.
			std::lock_guard<std::mutex> lock(control.mutex);
			return control.exit_code;
		}

		// Not launched by the SCM (the expected ERROR_FAILED_SERVICE_CONTROLLER_CONNECT,
		// or any other dispatcher failure): run as a normal console process.
		return entry(argc, argv, AppHostHooks{});
	}

	static bool InstallServiceImpl()
	{
		const std::wstring exe_path = ModuleFilePath();
		if (exe_path.empty())
		{
			std::cout << "Failed to install the service: could not determine the executable path.\n";
			return false;
		}

		std::vector<std::wstring> args = PassthroughArgs();

		// Default the config to %ProgramData%\Aqualink Automate\aqualink-automate.conf
		// when the operator did not supply one, mirroring the Linux unit's explicit
		// --config in ExecStart. Create the directory + a starter config so the service
		// starts cleanly. A service's working directory is System32, so paths MUST be
		// absolute.
		if (!HasConfigArg(args))
		{
			const std::filesystem::path config_path = ProgramDataDirectory() / L"aqualink-automate.conf";
			if (!config_path.empty())
			{
				EnsureStarterConfig(config_path);
				args.emplace_back(L"--config");
				args.push_back(config_path.wstring());
			}
		}

		// Compose the service binary path: "<abs exe>" <flags...>
		std::wstring bin_path = QuoteArg(exe_path);
		for (const auto& arg : args)
		{
			bin_path.push_back(L' ');
			bin_path += QuoteArg(arg);
		}

		SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
		if (nullptr == scm)
		{
			const DWORD err = ::GetLastError();
			std::cout << "Failed to install the service"
				<< ((ERROR_ACCESS_DENIED == err) ? " (administrator privileges are required)." : ".")
				<< "\n";
			return false;
		}

		SC_HANDLE service = ::CreateServiceW(
			scm,
			SERVICE_NAME,
			SERVICE_DISPLAY_NAME,
			SERVICE_ALL_ACCESS,
			SERVICE_WIN32_OWN_PROCESS,
			SERVICE_AUTO_START,
			SERVICE_ERROR_NORMAL,
			bin_path.c_str(),
			nullptr,                       // load-order group
			nullptr,                       // tag id
			L"Tcpip\0",                    // depend on the TCP/IP stack (double-NUL terminated)
			L"NT AUTHORITY\\LocalService", // least-privilege built-in account
			nullptr);                      // password (none for LocalService)

		if (nullptr == service)
		{
			const DWORD err = ::GetLastError();
			::CloseServiceHandle(scm);

			if (ERROR_SERVICE_EXISTS == err)
			{
				std::cout << "A service named 'Aqualink-Automate' is already installed; run --uninstall-service first.\n";
			}
			else if (ERROR_ACCESS_DENIED == err)
			{
				std::cout << "Failed to install the service (administrator privileges are required).\n";
			}
			else
			{
				std::cout << "Failed to install the service (error " << err << ").\n";
			}
			return false;
		}

		// Description (cosmetic) + delayed auto-start (start after boot-critical services;
		// the app depends on the network) + failure recovery (restart on crash), matching
		// the systemd unit's Restart=on-failure.
		SERVICE_DESCRIPTIONW description{ const_cast<LPWSTR>(SERVICE_DESCRIPTION) };
		::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

		SERVICE_DELAYED_AUTO_START_INFO delayed{ TRUE };
		::ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);

		SC_ACTION actions[3] =
		{
			{ SC_ACTION_RESTART, 5000 },   // 1st failure: restart after 5s
			{ SC_ACTION_RESTART, 5000 },   // 2nd failure: restart after 5s
			{ SC_ACTION_NONE,    0 }       // subsequent: no action
		};
		SERVICE_FAILURE_ACTIONSW failure_actions{};
		failure_actions.dwResetPeriod = 86400;  // reset the failure count after a day
		failure_actions.cActions = 3;
		failure_actions.lpsaActions = actions;
		::ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure_actions);

		::CloseServiceHandle(service);
		::CloseServiceHandle(scm);

		// Register the Event Log source in the same elevated action so Event Viewer
		// renders the service's log entries cleanly from the first run.
		const bool log_source_ok = (LogSourceRegistrationResult::Succeeded == RegisterLogSource("Aqualink-Automate"));

		std::cout << "Installed Windows service 'Aqualink-Automate' (account: NT AUTHORITY\\LocalService, start: automatic-delayed, restart-on-failure).\n"
			<< "  Binary path: " << Narrow(bin_path) << "\n"
			<< "  Event Log source: " << (log_source_ok ? "registered" : "NOT registered (continuing)") << "\n"
			<< "  Start it with:  sc start Aqualink-Automate   (or Start-Service Aqualink-Automate)\n";
		return true;
	}

	static bool UninstallServiceImpl()
	{
		SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (nullptr == scm)
		{
			const DWORD err = ::GetLastError();
			std::cout << "Failed to uninstall the service"
				<< ((ERROR_ACCESS_DENIED == err) ? " (administrator privileges are required)." : ".")
				<< "\n";
			return false;
		}

		SC_HANDLE service = ::OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
		if (nullptr == service)
		{
			const DWORD err = ::GetLastError();
			::CloseServiceHandle(scm);

			if (ERROR_SERVICE_DOES_NOT_EXIST == err)
			{
				// Idempotent: still clean up the Event Log source registration.
				UnregisterLogSource("Aqualink-Automate");
				std::cout << "Service 'Aqualink-Automate' is not installed; nothing to do.\n";
				return true;
			}

			std::cout << "Failed to open the service for removal"
				<< ((ERROR_ACCESS_DENIED == err) ? " (administrator privileges are required)." : ".")
				<< "\n";
			return false;
		}

		// Best-effort stop before delete so a running instance shuts down cleanly.
		SERVICE_STATUS status{};
		if (::ControlService(service, SERVICE_CONTROL_STOP, &status))
		{
			for (int i = 0; (i < 30) && (SERVICE_STOPPED != status.dwCurrentState); ++i)
			{
				::Sleep(500);
				if (!::QueryServiceStatus(service, &status))
				{
					break;
				}
			}
		}

		const bool deleted = (0 != ::DeleteService(service));
		const DWORD delete_err = deleted ? NO_ERROR : ::GetLastError();

		::CloseServiceHandle(service);
		::CloseServiceHandle(scm);

		const bool log_source_ok = (LogSourceRegistrationResult::Succeeded == UnregisterLogSource("Aqualink-Automate"));

		if (deleted || (ERROR_SERVICE_MARKED_FOR_DELETE == delete_err))
		{
			std::cout << "Uninstalled Windows service 'Aqualink-Automate'.\n"
				<< "  Event Log source: " << (log_source_ok ? "removed" : "NOT removed (continuing)") << "\n";
			return true;
		}

		std::cout << "Failed to delete the service (error " << delete_err << ").\n";
		return false;
	}

	// Public OS-neutral entry points: the SCM mechanics above stay file-local, and the
	// shared CLI handler branches on ServiceActionResult (never an OS #ifdef). The impls
	// print their own detailed outcome, so only Succeeded/Failed distinction is surfaced.
	ServiceActionResult InstallService()
	{
		return InstallServiceImpl() ? ServiceActionResult::Succeeded : ServiceActionResult::Failed;
	}

	ServiceActionResult UninstallService()
	{
		return UninstallServiceImpl() ? ServiceActionResult::Succeeded : ServiceActionResult::Failed;
	}

}
// namespace AqualinkAutomate::Application
