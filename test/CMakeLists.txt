#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

add_executable(testaqualink-automate)

target_sources( 
	testaqualink-automate
	PRIVATE
	unit/test_main.cpp
	unit/application/test_application_defaults.cpp
	unit/application/test_secure_runtime_paths.cpp
	unit/auth/test_audit_log.cpp
	unit/auth/test_auth_stores.cpp
	unit/auth/test_bootstrap.cpp
	unit/auth/test_kiosk.cpp
	unit/http/test_access_enforcement.cpp
	unit/http/test_http_admin_routes.cpp
	unit/http/test_http_auth_routes.cpp
	unit/http/test_http_preferences_subject.cpp
	unit/http/test_http_diagnostics_options_redaction.cpp
	unit/http/test_routing_authz.cpp
	unit/auth/test_entitlement.cpp
	unit/auth/test_jwt.cpp
	unit/auth/test_mock_idp.cpp
	unit/auth/test_password_hasher.cpp
	unit/auth/test_policy_engine.cpp
	unit/auth/test_session.cpp
	unit/auth/test_subject_resolver.cpp
	unit/auth/test_auth_branches.cpp
	unit/utility/test_offload_pool.cpp
	unit/auxillaries/test_jandy_auxillary_id.cpp
	unit/auxillaries/test_jandy_auxillary_span.cpp
	unit/auxillaries/test_jandy_auxillary_presence_override.cpp
	unit/auxillaries/test_jandy_auxillary_id_branches.cpp
	unit/concepts/test_concept_is_c_array.cpp
	unit/certificates/test_certificate_generation.cpp
	unit/certificates/test_certificate_management_branches.cpp
	unit/equipment_cache/test_equipment_cache_service.cpp
	unit/equipment_cache/test_equipment_cache_branches.cpp
	unit/errors/test_error_categories.cpp
	unit/devices/test_devices_id.cpp
	unit/devices/test_devices_onetouch_pageprocessors.cpp 
	unit/devices/test_devices_screendatapage.cpp
	unit/devices/test_devices_screendatapage_processor.cpp
	unit/devices/test_devices_screendatapage_updater.cpp
	unit/devices/test_devices_screenscraping.cpp 
	unit/devices/test_devices_types.cpp 
	unit/exceptions/test_genericaqualinkexception.cpp
	unit/exceptions/test_exception_message_forwarding.cpp
	unit/formatters/test_formatters.cpp
	unit/factories/test_factories_jandy_auxillary.cpp	 	
	unit/factories/test_factories_jandy_messages.cpp
	unit/generators/test_jandy_message_generator.cpp
	unit/fuzz/test_protocol_fuzz_smoke.cpp
	unit/http/test_api_equipment.cpp
	unit/http/test_api_metrics.cpp
	unit/http/test_display_label.cpp
	unit/http/test_http_chlorinator.cpp
	unit/http/test_http_circulation.cpp
	unit/http/test_http_controller_schedules.cpp
	unit/http/test_http_heater.cpp
	unit/http/test_http_schedule_promote.cpp
	unit/http/test_http_diagnostics_devices.cpp
	unit/http/test_http_diagnostics_captures.cpp
	unit/http/test_http_diagnostics_profiling.cpp
	unit/http/test_http_diagnostics_recording.cpp
	unit/http/test_html_escape.cpp
	unit/http/test_http_error_responses.cpp
	unit/http/test_http_health_detailed.cpp
	unit/http/test_http_security.cpp
	unit/http/test_http_make_response.cpp
	unit/http/test_http_sessions.cpp
	unit/http/test_http_setpoints.cpp
	unit/http/test_http_static_file.cpp
	unit/http/test_routing_matches.cpp
	unit/http/test_routing_node_bounds.cpp
	unit/http/test_ws_equipment.cpp
	unit/http/test_ws_keepalive.cpp
	unit/kernel/test_auxillary_traits.cpp
	unit/kernel/test_auxillary_traits_status.cpp
	unit/logging/test_logging_validator_jandyemulateddevicetype.cpp 
	unit/logging/test_logging_validator_jandydeviceid.cpp 
	unit/logging/test_logging_validator_profiler.cpp 
	unit/logging/test_logging_validator_severity.cpp 
	unit/logging/test_logging_severity_filter_defaults.cpp
	unit/logging/test_logging_filter_and_helpers.cpp
	unit/logging/test_severity_mappings.cpp
	unit/logging/test_log_environment.cpp
	unit/logging/test_sink_console_and_registry.cpp
	unit/logging/test_sink_json_and_file.cpp
	unit/logging/test_audit_separation.cpp
	unit/alerting/test_alert_monitor.cpp
	unit/alerting/test_webhook_tls.cpp
	unit/alerting/test_webhook_sink.cpp
	unit/alerting/test_alert_monitor_branches.cpp
	unit/history/test_history_service.cpp
	unit/history/test_history_service_branches.cpp
	unit/options/test_option_metadata.cpp
	unit/options/test_options_app.cpp
	unit/options/test_options_alerting.cpp
	unit/options/test_options_auth.cpp
	unit/options/test_options_config_file.cpp
	unit/options/test_options_developer.cpp
	unit/options/test_options_equipment.cpp
	unit/options/test_options_history.cpp
	unit/options/test_options_jandy.cpp
	unit/options/test_options_logging.cpp
	unit/options/test_options_pipeline.cpp
	unit/options/test_options_preferences.cpp
	unit/options/test_options_scheduling.cpp
	unit/options/test_options_serial.cpp
	unit/options/test_options_validators.cpp
	unit/preferences/test_preferences_service.cpp
	unit/preferences/test_user_preferences_store.cpp
	unit/preferences/test_preferences_branches.cpp
	unit/scheduling/test_controller_schedule.cpp
	unit/scheduling/test_promotion_constraints.cpp
	unit/scheduling/test_schedule_authorization.cpp
	unit/scheduling/test_schedule.cpp
	unit/scheduling/test_scheduler_service.cpp
	unit/scheduling/test_scheduler_service_branches.cpp
	unit/startup/test_jandy_revision_capabilities.cpp
	unit/startup/test_jandy_startup_planner.cpp
	unit/startup/test_jandy_startup_coordinator.cpp
	unit/startup/test_jandy_startup_environment.cpp
	unit/startup/test_jandy_startup_service.cpp
	unit/options/test_options_web.cpp
	unit/options/test_options_matter.cpp
	unit/messages/test_aquarite_message_getid.cpp
	unit/messages/test_aquarite_message_percent.cpp
	unit/messages/test_aquarite_message_setpercent.cpp
	unit/messages/test_aquarite_message_ppm.cpp
	unit/messages/test_message.cpp 
	unit/messages/test_message_ack.cpp 
	unit/messages/test_message_message.cpp 
	unit/messages/test_message_message_long.cpp 	 
	unit/messages/test_message_pda_highlight.cpp 
	unit/messages/test_message_probe.cpp 
	unit/messages/test_message_status.cpp
	unit/messages/test_message_structure.cpp
	unit/messages/test_message_dle_stuffing.cpp
	unit/messages/test_message_unknown.cpp
	unit/messages/test_to_master_decode.cpp
	unit/mocks/mock_beast_basicstream_with_timeout.cpp
	unit/mocks/mock_chronoclocks.cpp 
	unit/mocks/mock_generator_messagetorawdata.cpp 
	unit/mocks/mock_generator_rawdatatomessage.cpp 	
	unit/mocks/mock_mqtt_broker.cpp
	unit/mocks/mock_oidc_provider.cpp
	unit/mocks/mock_testserialportimpl.cpp
	unit/mqtt/test_mqtt_client.cpp
	unit/mqtt/test_mqtt_ha_discovery.cpp
	unit/mqtt/test_mqtt_hub.cpp
	unit/mqtt/test_mqtt_integration.cpp
	unit/mqtt/test_mqtt_options.cpp
	unit/mqtt/test_mqtt_payload_parsing.cpp
	unit/mqtt/test_mqtt_topic_scheme.cpp
	unit/mqtt/test_mqtt_client_branches.cpp
	unit/mqtt/test_mqtt_ha_discovery_branches.cpp
	unit/mqtt/test_mqtt_hub_branches.cpp
	unit/mqtt/test_mqtt_integration_branches.cpp
	unit/mqtt/test_mqtt_client_branches2.cpp
	unit/protocol/test_protocol_handler.cpp
	unit/protocol/test_mock_replay_validation.cpp
	unit/protocol/test_replay_pacing.cpp
	unit/protocol/test_record_replay_roundtrip.cpp
	unit/protocol/test_record_runtime_toggle.cpp
	unit/protocol/test_pentair_message_generator.cpp
	unit/statuses/test_statusupdate_equipmentanddevices.cpp
	unit/support/unit_test_circularbuffermaker.cpp
	unit/support/unit_test_httprequestresponse.cpp
	unit/support/unit_test_hublocatorinjector.cpp
	unit/support/unit_test_loadfixture.cpp
	unit/support/unit_test_mockreplayharness.cpp
	unit/support/unit_test_onetouchdevice.cpp
	unit/support/unit_test_ostream_support.cpp 	
	unit/support/unit_test_protocolmessagebuilder.cpp
	unit/support/unit_test_wsrequestresponse.cpp
	unit/units/test_flow_rate.cpp 
	unit/units/test_units_orp.cpp 
	unit/units/test_units_ph.cpp 
	unit/utility/test_utility_auxillarystate.cpp
	unit/utility/test_utility_bandwidthutilisation.cpp
	unit/utility/test_utility_caseinsensitivecomparison.cpp
	unit/utility/test_utility_chemistry.cpp
	unit/utility/test_utility_spa_switch_assignment.cpp
	unit/utility/test_utility_endpointparser.cpp
	unit/utility/test_utility_jandychecksum.cpp
	unit/utility/test_utility_latencytracker.cpp
	unit/utility/test_utility_signallingstatscounter.cpp
	unit/utility/test_utility_temperature.cpp
	unit/utility/test_utility_temperature_conversion.cpp
	unit/utility/test_utility_timeoutdurationstringconverter.cpp
	unit/utility/test_utility_tonumber.cpp
	unit/utility/test_utility_valuedebouncer.cpp
	unit/navigation/test_navigation_menu_model.cpp
	unit/navigation/test_navigation_navigator_edgecases.cpp
	unit/navigation/test_navigation_visit_policies.cpp
	unit/navigation/test_spider_engine.cpp
	unit/navigation/test_navigation_aux_scrolling_discovery.cpp
	unit/navigation/test_navigation_navigator_branches.cpp
	unit/messages/test_chemlink_message_response.cpp
	unit/messages/test_pentair_message_base.cpp
	unit/devices/test_devices_aquarite.cpp
	unit/devices/test_chlorinator_setpoint_refresh.cpp
	unit/devices/test_devices_capabilities_command_history.cpp
	unit/devices/test_devices_capabilities_restartable.cpp
	unit/devices/test_devices_chemlink.cpp
	unit/devices/test_devices_command_dispatcher.cpp
	unit/devices/test_devices_emulated_factory.cpp
	unit/devices/test_devices_epump.cpp
	unit/devices/test_devices_heater.cpp
	unit/devices/test_devices_iaq.cpp
	unit/devices/test_devices_iaq_aquapure_writer.cpp
	unit/devices/test_devices_iaq_spaswitch_config.cpp
	unit/devices/test_devices_onetouch_spaswitch_config.cpp
	unit/devices/test_jandy_iaq_page_model.cpp
	unit/devices/test_jandy_iaq_page_registry.cpp
	unit/devices/test_jandy_iaq_schedule_parser.cpp
	unit/devices/test_jandy_iaq_schedule_read.cpp
	unit/devices/test_jandy_iaq_schedule_write.cpp
	unit/devices/test_jandy_onetouch_schedule_parser.cpp
	unit/devices/test_jandy_onetouch_screen_reader.cpp
	unit/devices/test_jandy_onetouch_schedule_read.cpp
	unit/devices/test_jandy_onetouch_schedule_write.cpp
	unit/devices/test_pentair_vsp_pump.cpp
	unit/devices/test_pentair_chlorinator.cpp
	unit/devices/test_pentair_controller.cpp
	unit/devices/test_pentair_equipment.cpp
	unit/messages/test_iaq_messages.cpp
	unit/messages/test_iaq_message_device_id.cpp
	unit/messages/test_iaq_message_main_status.cpp
	unit/messages/test_jandy_message_text_helpers.cpp
	unit/formatters/test_formatters_jandy_messages.cpp
	unit/formatters/test_formatters_jandy_devices.cpp
	unit/formatters/test_formatters_screen_data_page.cpp
	unit/formatters/test_formatters_stats_counter.cpp
	unit/formatters/test_formatters_chemistry.cpp
	unit/serial/test_serial_port.cpp
	unit/messages/test_epump_messages.cpp
	unit/messages/test_heater_messages.cpp
	unit/messages/test_light_message_status.cpp
	unit/messages/test_pda_messages.cpp
	unit/messages/test_serial_adapter_messages.cpp
	unit/devices/test_devices_onetouch.cpp
	unit/devices/test_devices_pda.cpp
	unit/devices/test_devices_keypad.cpp
	unit/devices/test_devices_lights.cpp
	unit/devices/test_devices_protocol_coverage.cpp
	unit/devices/test_devices_serial_adapter.cpp
	unit/devices/test_devices_spaside_remote.cpp
	unit/devices/test_devices_spaside_controller.cpp
	unit/devices/test_devices_onetouch_goals.cpp
	unit/devices/test_devices_onetouch_lifecycle.cpp
	unit/devices/test_devices_serial_adapter_branches.cpp
	unit/devices/test_devices_iaq_branches.cpp
	unit/devices/test_devices_iaq_branches2.cpp
	unit/devices/test_devices_onetouch_pageprocessors_more.cpp
	unit/devices/test_devices_onetouch_startup_survey.cpp
	unit/devices/test_devices_onetouch_statusprocessors.cpp
	unit/devices/test_devices_command_dispatcher_branches.cpp
	unit/devices/test_devices_onetouch_message_router.cpp
	unit/devices/test_devices_spaside_branches.cpp
	unit/devices/test_devices_misc_branches.cpp
	unit/equipment/test_jandy_equipment.cpp
	unit/kernel/test_kernel_hub_locator.cpp
	unit/kernel/test_kernel_device_graph.cpp
	unit/kernel/test_kernel_equipment_hub.cpp
	unit/kernel/test_kernel_equipment_versions.cpp
	unit/kernel/test_kernel_body_of_water.cpp
	unit/kernel/test_kernel_data_hub_circulation.cpp
	unit/kernel/test_kernel_data_hub_telemetry.cpp
	unit/kernel/test_kernel_status_conversions.cpp
	unit/kernel/test_kernel_system_boards.cpp
	unit/utility/test_utility_string_manipulation.cpp
	unit/utility/test_utility_pool_configuration_decoder.cpp
	unit/utility/test_jandy_equipment_validator.cpp
	unit/utility/test_utility_auxillary_state_converter.cpp
	unit/http/test_http_server_fields.cpp
	unit/http/test_http_apikeys_branches.cpp
	unit/http/test_http_auth_branches.cpp
	unit/http/test_http_auxslots.cpp
	unit/http/test_http_chlorinator_branches.cpp
	unit/http/test_http_controller_schedules_branches.cpp
	unit/http/test_http_diagnostics_branches.cpp
	unit/http/test_http_equipment_buttons.cpp
	unit/http/test_http_equipment_iaq.cpp
	unit/http/test_http_setpoints_branches.cpp
	unit/http/test_http_spaside_remotes.cpp
	unit/http/test_http_server_branches.cpp
	unit/http/test_routing_branches.cpp
	unit/profiling/test_profiling_noop.cpp
	unit/profiling/test_profiling_factories.cpp
	unit/profiling/test_profiling_controller.cpp
	unit/options/test_options_settings.cpp
	unit/options/test_options_registry_branches.cpp
	unit/platform/test_aligned_alloc.cpp
	unit/serial/test_mock_serial_port_impl.cpp
	unit/serial/test_mock_serial_replay_format.cpp
	unit/serial/test_rfc2217_inbound_filter.cpp
	unit/serial/test_test_serial_port_impl.cpp
	unit/serial/test_mock_serial_port_impl_branches.cpp
	unit/serial/test_network_serial_port_impl.cpp
	unit/serial/test_recording_serial_port_impl.cpp
)

target_compile_definitions(
	testaqualink-automate 
	PUBLIC 
	-DBOOST_TEST_DYN_LINK=1
	-DMAGIC_ENUM_RANGE_MIN=0
	-DMAGIC_ENUM_RANGE_MAX=255
	$<$<BOOL:${ENABLE_ASIO_TRACKING}>:-DBOOST_ASIO_ENABLE_HANDLER_TRACKING> # Enable ASIO connection tracking to aid debug async operations
)

target_include_directories(
	testaqualink-automate
	PUBLIC
	"${PROJECT_SOURCE_DIR}/test/unit"
	# Shared fuzz frame builders (fuzz/fuzz_frame_builders.h) are exercised by the
	# protocol fuzz smoke/regression test (unit/fuzz/test_protocol_fuzz_smoke.cpp).
	"${PROJECT_SOURCE_DIR}/fuzz"
)

target_link_libraries(
	testaqualink-automate
	PRIVATE
	libaqualink-automate
	libaqualink-jandy
	libaqualink-pentair
	Boost::unit_test_framework
)

add_test(NAME AqualinkAutomateUnitTests COMMAND testaqualink-automate WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# Label so callers can gate suites: `ctest --preset <p> -L unit` (fast, local /
# PR) vs `-L integration` (the slower fixture-replay suite). See docs/releasing.md.
set_tests_properties(AqualinkAutomateUnitTests PROPERTIES LABELS "unit")

if(ENABLE_COVERAGE)

if(MSVC)

setup_target_for_coverage_opencppcoverage(
	NAME coverage-testaqualink-automate
	EXECUTABLE ctest -j ${PROCESSOR_COUNT} -R AqualinkAutomateUnitTests
	DEPENDENCIES testaqualink-automate
	SOURCES
		"${PROJECT_SOURCE_DIR}/src"
	MODULES
		"${PROJECT_BINARY_DIR}"
	EXCLUDE
		"${PROJECT_SOURCE_DIR}/deps"
		"${CMAKE_BINARY_DIR}/vcpkg_installed"
	OUTPUT_FORMAT $<IF:$<BOOL:${USE_SONARQUBE}>,cobertura,html>
)

elseif(USE_SONARQUBE)

# NOTE: this is consumed as a CMake list (each element becomes one VERBATIM
# argument to gcovr). Keep each flag a SEPARATE list element — collapsing them
# into one space-joined string makes gcovr see a single malformed argument.
set(GCOVR_ADDITIONAL_ARGS "--gcov-ignore-parse-errors=negative_hits.warn" "--exclude-throw-branches" "--exclude-unreachable-branches")

setup_target_for_coverage_gcovr_xml(
	NAME coverage-testaqualink-automate
	EXECUTABLE ctest -j ${PROCESSOR_COUNT} -R AqualinkAutomateUnitTests
	DEPENDENCIES testaqualink-automate
	EXCLUDE
		"${PROJECT_SOURCE_DIR}/deps"
		"${CMAKE_BINARY_DIR}/vcpkg_installed"
		"/usr/include"
		"include"
	OUTPUT_FORMAT sonarqube
)

else()

setup_target_for_coverage_gcovr_html(
	NAME coverage-testaqualink-automate
	EXECUTABLE ctest -j ${PROCESSOR_COUNT} -R AqualinkAutomateUnitTests
	DEPENDENCIES testaqualink-automate
	EXCLUDE
		"${PROJECT_SOURCE_DIR}/deps"
		"${CMAKE_BINARY_DIR}/vcpkg_installed"
		"/usr/include"
		"include"
)

endif()

endif(ENABLE_COVERAGE)

if(ENABLE_SANITIZERS)
target_enable_sanitizers(testaqualink-automate)
endif(ENABLE_SANITIZERS)

#------------------------------------------------------------------------------
#
#  Integration Tests
#
#------------------------------------------------------------------------------

add_executable(intaqualink-automate)

target_sources(
	intaqualink-automate
	PRIVATE
	integration/test_main.cpp
	# support
	integration/support/integration_test_pipeline.cpp
	integration/support/integration_test_scenarios.cpp
	# equipment config tests
	integration/equipment/test_equipment_pool_only.cpp
	integration/equipment/test_equipment_spa_only.cpp
	integration/equipment/test_equipment_dual_body_shared.cpp
	integration/equipment/test_equipment_dual_body_dual.cpp
	# flow tests
	integration/flows/test_flow_serial_to_mqtt.cpp
	integration/flows/test_flow_serial_to_web.cpp
	integration/flows/test_flow_mqtt_commands.cpp
	integration/flows/test_flow_web_commands.cpp
	integration/flows/test_flow_fixture_replay.cpp
	integration/flows/test_flow_command_to_wire.cpp
	integration/flows/test_flow_spaside_remote.cpp
	integration/flows/test_flow_web_spaside_remotes.cpp
	integration/flows/test_flow_iaq_actuation.cpp
	integration/flows/test_flow_onetouch_toggle.cpp
	integration/flows/test_flow_onetouch_setpoint.cpp
	integration/flows/test_flow_onetouch_chlorinator.cpp
	integration/flows/test_flow_rssa_presence_gating.cpp
	integration/flows/test_flow_autostartup.cpp
	# IAQ device tests
	integration/devices/test_iaq_device_commands.cpp
	# Serial Adapter device tests
	integration/devices/test_serial_adapter_device_commands.cpp
	# reuse unit test support
	unit/mocks/mock_beast_basicstream_with_timeout.cpp
	unit/mocks/mock_testserialportimpl.cpp
	unit/support/unit_test_hublocatorinjector.cpp
	unit/support/unit_test_loadfixture.cpp
	unit/support/unit_test_mockreplayharness.cpp
	unit/support/unit_test_ostream_support.cpp
	unit/support/unit_test_protocolmessagebuilder.cpp
	unit/support/unit_test_httprequestresponse.cpp
)

target_compile_definitions(
	intaqualink-automate
	PUBLIC
	-DBOOST_TEST_DYN_LINK=1
	-DMAGIC_ENUM_RANGE_MIN=0
	-DMAGIC_ENUM_RANGE_MAX=255
)

target_include_directories(
	intaqualink-automate
	PUBLIC
	"${PROJECT_SOURCE_DIR}/test/unit"
	"${PROJECT_SOURCE_DIR}/test/integration"
)

target_link_libraries(
	intaqualink-automate
	PRIVATE
	libaqualink-automate
	libaqualink-jandy
	libaqualink-pentair
	Boost::unit_test_framework
)

add_test(NAME AqualinkAutomateIntegrationTests COMMAND intaqualink-automate WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(AqualinkAutomateIntegrationTests PROPERTIES LABELS "integration")

if(ENABLE_SANITIZERS)
target_enable_sanitizers(intaqualink-automate)
endif(ENABLE_SANITIZERS)

# Integration tests need web templates too
add_custom_command(
	TARGET intaqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying web assets to integration test folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/assets/web" "${CMAKE_CURRENT_BINARY_DIR}/web"
)

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

if(ENABLE_BENCHMARKS)

add_executable(perfaqualink-automate)

target_sources(
	perfaqualink-automate
	PRIVATE
	performance/test_main.cpp
	performance/test_jandy_message_factory.cpp
	performance/test_latency_tracker.cpp
	performance/test_protocol_handler.cpp
	performance/test_serialization.cpp
	performance/test_mqtt_performance.cpp
	unit/mocks/mock_mqtt_broker.cpp
)

target_include_directories(
	perfaqualink-automate
	PUBLIC
	"${PROJECT_SOURCE_DIR}/test/performance"
)

target_link_libraries(
	perfaqualink-automate
	PRIVATE
	libaqualink-automate
	libaqualink-jandy
	libaqualink-pentair
	benchmark::benchmark
	benchmark::benchmark_main
)

add_test(NAME AqualinkAutomatePerfTests COMMAND perfaqualink-automate WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(AqualinkAutomatePerfTests PROPERTIES LABELS "perf")

if(ENABLE_SANITIZERS)
target_enable_sanitizers(perfaqualink-automate)
endif(ENABLE_SANITIZERS)

endif(ENABLE_BENCHMARKS)

#------------------------------------------------------------------------------
#
#  The various unit tests for HTTP routing require that the templates exist
#  so that the mstch can render the payloads to be validated.
#
#------------------------------------------------------------------------------

add_custom_command(
	TARGET testaqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying web assets to test folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/assets/web" "${CMAKE_CURRENT_BINARY_DIR}/web"
)

#------------------------------------------------------------------------------
#
#  Record/replay capture fixtures must be available next to the test exe so that
#  LoadFixture("fixtures/<name>.cap") resolves against the test working dir.
#
#------------------------------------------------------------------------------

add_custom_command(
	TARGET testaqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying capture fixtures to test folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/test/fixtures" "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
)

# The HA companion entity manifest must sit next to the test exe so that the
# discovery <-> manifest lock test (TestSuite_HaDiscovery_CompanionManifest)
# can read companion/entity-manifest.json from the test working dir.
add_custom_command(
	TARGET testaqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying HA companion entity manifest to test folder..."
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/companion"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/homeassistant/companion/entity-manifest.json" "${CMAKE_CURRENT_BINARY_DIR}/companion/entity-manifest.json"
)

add_custom_command(
	TARGET intaqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying capture fixtures to integration test folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/test/fixtures" "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
)
