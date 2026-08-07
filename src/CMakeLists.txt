#------------------------------------------------------------------------------
#
#  APPLICATION COMPONENTS
#  ======================
#
#------------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(jandy)
add_subdirectory(pentair)

#------------------------------------------------------------------------------
#
#  APPLICATION EXECUTABLE
#  ======================
#
#------------------------------------------------------------------------------

add_executable(aqualink-automate)

target_sources(
	aqualink-automate
	PRIVATE
	aqualink-automate.cpp
)

#------------------------------------------------------------------------------
#
#  WINDOWS EVENT LOG MESSAGE TABLE
#  ==============================
#
#  Compile the message-table resource (.mc -> mc.exe -> .rc/.bin) and link it into
#  the executable so Event Viewer renders the runtime Event Log sink's entries
#  verbatim, with no "The description for Event ID ... cannot be found" wrapper. The
#  message IDs match Boost.Log's simple_event_log_backend (see the .mc). mc.exe/rc.exe
#  ship with the Windows SDK and are on PATH in the VS Dev Shell; CMake/MSVC compiles a
#  .rc added to target_sources and links the resulting resource automatically.
#
#------------------------------------------------------------------------------

if(WIN32)
	set(_aq_mc_in  "${CMAKE_CURRENT_SOURCE_DIR}/core/platform/windows/eventlog/aqualink_eventlog.mc")
	set(_aq_mc_dir "${CMAKE_CURRENT_BINARY_DIR}/eventlog")
	add_custom_command(
		OUTPUT "${_aq_mc_dir}/aqualink_eventlog.rc"
		BYPRODUCTS "${_aq_mc_dir}/aqualink_eventlog.h" "${_aq_mc_dir}/MSG00409.bin"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aq_mc_dir}"
		COMMAND mc.exe -U -h "${_aq_mc_dir}" -r "${_aq_mc_dir}" "${_aq_mc_in}"
		DEPENDS "${_aq_mc_in}"
		COMMENT "Compiling Windows Event Log message table (mc.exe)"
		VERBATIM
	)
	target_sources(aqualink-automate PRIVATE "${_aq_mc_dir}/aqualink_eventlog.rc")
endif()

target_link_libraries(
	aqualink-automate 
	PRIVATE
	libaqualink-automate
	libaqualink-jandy
	libaqualink-pentair
)

#------------------------------------------------------------------------------
#
#  APPLICATION WEB SERVER ASSETS
#  =============================
#
#  Track web and SSL asset files so changes trigger a copy during incremental
#  builds. The CONFIGURE_DEPENDS option causes CMake to re-run when files are
#  added or removed.
#
#  The approach uses OBJECT_DEPENDS to make the executable's source file depend
#  on the asset files. When any asset changes, the executable is "rebuilt"
#  (relinked), which triggers the POST_BUILD copy commands.
#
#------------------------------------------------------------------------------

# Collect all web asset files for dependency tracking
file(GLOB_RECURSE WEB_ASSET_FILES
	CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/assets/web/*"
)

# Collect all SSL asset files for dependency tracking
file(GLOB_RECURSE SSL_ASSET_FILES
	CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/assets/ssl/*"
)

# Collect the Home Assistant companion package files for dependency tracking (see
# the POST_BUILD copy below). Excludes test-harness/, which is CI-only fixture
# content that is never copied into the served web root.
file(GLOB_RECURSE HA_COMPANION_ASSET_FILES
	CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/homeassistant/companion/*"
)
list(FILTER HA_COMPANION_ASSET_FILES EXCLUDE REGEX "/test-harness/")

# Make the main source file depend on all asset files.
# When any asset changes, this triggers a relink of the executable,
# which in turn runs the POST_BUILD commands.
set_property(
	SOURCE aqualink-automate.cpp
	APPEND PROPERTY OBJECT_DEPENDS ${WEB_ASSET_FILES} ${SSL_ASSET_FILES} ${HA_COMPANION_ASSET_FILES}
)

# Copy web assets after the executable is built
add_custom_command(
	TARGET aqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying web assets to build folder..."
	# Copy beside the executable into web/ to match the runtime default doc-root
	# (Application::DOC_ROOT = <exe_dir>/web) and the packaged layout (bin/web).
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/assets/web" "$<TARGET_FILE_DIR:aqualink-automate>/web"
	# Stamp the service-worker cache name with the build version: a unique cache
	# per build whose old offline shells purge on activate (see cmake/stamp_sw_version.cmake).
	COMMAND ${CMAKE_COMMAND} -D "SW_JS=$<TARGET_FILE_DIR:aqualink-automate>/web/sw.js" -D "AQ_VERSION=${PROJECT_VERSION}" -P "${CMAKE_SOURCE_DIR}/cmake/stamp_sw_version.cmake"
	COMMENT "Syncing web assets"
)

# Copy the Home Assistant companion package (blueprints, helpers package, dashboard)
# into the served web root, so a running instance can offer it for download without
# needing GitHub reachability (e.g. an air-gapped install) — see
# docs/homeassistant-companion.md. Served by the same generic StaticFileHandler as
# the rest of web/, with no C++ changes (same mechanism as api/swagger.yaml).
# test-harness/ is CI-only fixture content; copy_directory_if_different has no
# exclude option, so it is stripped immediately after the copy.
add_custom_command(
	TARGET aqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying HA companion package to build folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/homeassistant/companion" "$<TARGET_FILE_DIR:aqualink-automate>/web/homeassistant"
	COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:aqualink-automate>/web/homeassistant/test-harness"
	COMMENT "Syncing HA companion package"
)

# Copy SSL assets after the executable is built
add_custom_command(
	TARGET aqualink-automate
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "Copying SSL assets to build folder..."
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/assets/ssl" "$<TARGET_FILE_DIR:aqualink-automate>/ssl"
	COMMENT "Syncing SSL assets"
)

# Also expose asset files in IDE project views for easy editing
add_custom_target(web-assets SOURCES ${WEB_ASSET_FILES})
add_custom_target(ssl-assets SOURCES ${SSL_ASSET_FILES})
