# - Find libsystemd (sd-journal) for the native journald log sink.
# Defines:
#   systemd_FOUND
#   systemd_INCLUDE_DIRS
#   systemd_LIBRARIES
#
# Only present on Linux/systemd hosts; on Windows/macOS the header is absent and
# systemd_FOUND is false, so the journald sink is excluded from the build. The
# SYSTEMD_SUPPORT_ENABLED compile definition (gated on systemd_FOUND) is applied by
# src/core/CMakeLists.txt — this module only sets the result variables.

find_path(systemd_INCLUDE_DIRS systemd/sd-journal.h)
find_library(systemd_LIBRARIES NAMES systemd)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(systemd DEFAULT_MSG systemd_LIBRARIES systemd_INCLUDE_DIRS)

mark_as_advanced(systemd_INCLUDE_DIRS systemd_LIBRARIES)
