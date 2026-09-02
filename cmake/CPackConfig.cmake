set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/packages")
set(CPACK_STRIP_FILES ON)
set(CPACK_THREADS 0)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Runtime-dependency bundling
# ===========================
# The shipped triplets link the vcpkg dependencies (Boost/OpenSSL/sqlite3/zlib)
# DYNAMICALLY (see cmake/vcpkg/triplets/*). Those libraries live in the build
# tree under vcpkg_installed/<triplet>/{lib,bin} and are NOT system packages, so
# every distributable package must carry them or the installed binary fails to
# start ("error while loading shared libraries ...").
#
#   - Windows : fixup_bundle copies the dependency DLLs next to the .exe, and
#               InstallRequiredSystemLibraries adds the MSVC runtime DLLs.
#   - Linux   : the .so files are installed into a PRIVATE lib dir and the
#               executable carries an $ORIGIN-relative RPATH so the loader finds
#               them without polluting the system library path.
#   - macOS   : the .dylib files are installed into the same private lib dir and
#               the executable carries an @loader_path-relative RPATH. (vcpkg
#               builds @rpath-clean dylibs.)
# ---------------------------------------------------------------------------

set(AQ_VCPKG_LIB_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib")
set(AQ_VCPKG_BIN_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")

# Private directory (relative to the install prefix) that holds the bundled
# vcpkg runtime libraries on Linux/macOS. A package-specific subdirectory keeps
# the vendored libraries out of the shared system library path.
set(AQ_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/aqualink-automate")

if("${CMAKE_SYSTEM_NAME}" MATCHES "Windows")

    # Windows is not an FHS platform: keep the package self-contained with the
    # web/ssl assets sitting beside the executable in bin/ (the runtime resolves
    # them relative to the executable — see platform/windows/application_defaults.cpp).
    set(AQ_WEB_DESTINATION "bin/web")
    set(AQ_SSL_DESTINATION "bin/ssl")
    set(AQ_EXAMPLES_DESTINATION "examples")

    # The executable MUST be in the same component (Runtime) as the fixup_bundle
    # install(CODE) below. NSIS does a per-component install, so if the exe were in
    # the default (Unspecified) component, fixup_bundle would run during the Runtime
    # component install with the exe not yet staged -> "fixup_bundle: not a valid
    # bundle". (ZIP is monolithic so it happened to work regardless.)
    install(TARGETS aqualink-automate RUNTIME DESTINATION bin COMPONENT Runtime)

    set(BUNDLE_APPS \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/aqualink-automate${CMAKE_EXECUTABLE_SUFFIX})
    install(
        CODE
        "
            include(BundleUtilities)
            fixup_bundle(
                \"${BUNDLE_APPS}\"
                \"\"
                \"${AQ_VCPKG_BIN_DIR}\"
            )
        "
        COMPONENT Runtime
    )

    # Bundle the MSVC C/C++ runtime DLLs (vcruntime140.dll, msvcp140.dll, ...).
    # fixup_bundle treats these System32 DLLs as "system" and skips them, so a
    # clean machine without the VC++ redistributable would fail to launch.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP FALSE)
    include(InstallRequiredSystemLibraries)

    # Windows installer
    set(CPACK_GENERATOR "ZIP;NSIS" CACHE STRING "Package targets")

    # NSIS-specific configuration
    set(CPACK_NSIS_DISPLAY_NAME "Aqualink Automate")
    set(CPACK_NSIS_PACKAGE_NAME "Aqualink Automate")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")

    # FHS-aligned, relocatable layout. The executable resolves its assets
    # relative to itself (../share/aqualink-automate/...) via boost::dll, so the
    # same install tree works whether unpacked from the .tgz anywhere or
    # installed by the .deb/.rpm under /usr.
    set(AQ_WEB_DESTINATION "${CMAKE_INSTALL_DATADIR}/aqualink-automate/web")
    set(AQ_SSL_DESTINATION "${CMAKE_INSTALL_DATADIR}/aqualink-automate/ssl")
    set(AQ_EXAMPLES_DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples")

    # $ORIGIN is the directory of the executable (bin/); the bundled libraries
    # live in the private lib dir. The literal "$ORIGIN" must reach the linker,
    # so it is NOT a CMake ${} expansion.
    set_target_properties(aqualink-automate PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${AQ_PRIVATE_LIBDIR}")

    install(TARGETS aqualink-automate RUNTIME DESTINATION bin)

    install(DIRECTORY "${AQ_VCPKG_LIB_DIR}/"
        DESTINATION ${AQ_PRIVATE_LIBDIR}
        COMPONENT Runtime
        FILES_MATCHING
            PATTERN "*.so*"
            PATTERN "pkgconfig" EXCLUDE
            PATTERN "cmake" EXCLUDE)

    # Bundle the C++ runtime (libstdc++ / libgcc) alongside the vcpkg libraries.
    # The project is built with a newer gcc than stable distros ship (e.g. gcc-15
    # vs gcc-12 on Debian Bookworm / Raspberry Pi OS), so the binary AND the
    # vcpkg .so files require a newer GLIBCXX/CXXABI than the target system has —
    # without this they fail to load with "version `GLIBCXX_3.4.3x' not found".
    # These libs are NOT under vcpkg_installed; locate the compiler's own copies
    # and drop them into the same private dir, where the executable's $ORIGIN
    # RPATH (and each vcpkg .so's own $ORIGIN RPATH) resolves them. (glibc itself
    # cannot be bundled — that is handled by building on an old-glibc base.)
    foreach(_aq_runtime_lib libstdc++.so.6 libgcc_s.so.1)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${_aq_runtime_lib}"
            OUTPUT_VARIABLE _aq_runtime_lib_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        get_filename_component(_aq_runtime_lib_path "${_aq_runtime_lib_path}" REALPATH)
        if(EXISTS "${_aq_runtime_lib_path}")
            # RENAME to the SONAME so the loader finds it by the name the binary needs.
            install(FILES "${_aq_runtime_lib_path}"
                DESTINATION ${AQ_PRIVATE_LIBDIR}
                COMPONENT Runtime
                RENAME ${_aq_runtime_lib})
        else()
            message(WARNING "Could not locate ${_aq_runtime_lib} to bundle (got '${_aq_runtime_lib_path}')")
        endif()
    endforeach()

    # System installation packages for unix systems
    set(CPACK_GENERATOR "TGZ;DEB;RPM" CACHE STRING "Package targets")

    # Required because install() below writes to an ABSOLUTE destination outside
    # the package prefix (/etc/aqualink-automate, see below). Without this, CPack's
    # archive generator (TGZ) installs straight to that literal path on the BUILD
    # MACHINE while packaging -- DEB/RPM stage through their own tooling regardless,
    # but TGZ does not unless told to. Observed failing on a build container:
    # "CMake Error ... file cannot create directory: /etc/aqualink-automate" during
    # `cpack`, even though the same install() succeeds seconds earlier via
    # `cmake --install --prefix / ` with an explicit DESTDIR (a fresh, non-system
    # path). This is CMake's own documented fix for archive generators combined
    # with absolute-path install() rules.
    set(CPACK_SET_DESTDIR "ON")

    # The package must install under /usr regardless of CMAKE_INSTALL_PREFIX --
    # the systemd unit this package installs to lib/systemd/system (below) is
    # only ever searched by systemd at /usr/lib/systemd/system, so this package
    # was never designed to run under any other prefix. CPACK_PACKAGING_INSTALL_
    # PREFIX looks like the right knob for this, but it does NOT override an
    # already-cached CMAKE_INSTALL_PREFIX for the DEB/RPM/Archive generators
    # (verified directly: the smoke test found `aqualink-automate: command not
    # found` with it set -- the binary was still packaged under the build's own
    # cached prefix). The actual fix is reconfiguring CMAKE_INSTALL_PREFIX=/usr
    # before packaging -- see the release workflow's packaging step.

    # The dependency libraries — including the C++ runtime above — are bundled
    # privately, so dpkg-shlibdeps must NOT try to resolve them against system
    # packages. The ONLY genuine system runtime dependency is glibc. The version
    # floor matches the build base: the release packages are built on a glibc-2.36
    # base (Debian Bookworm / Raspberry Pi OS) so they run there and on every newer
    # distro; keep this in lockstep with the package build environment.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.36), systemd")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    # RPM's automatic dependency scanner sees the bundled .so as both provided
    # (by the private lib dir) and required (by the binary), so it self-resolves.
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

    # -----------------------------------------------------------------------
    # System integration — systemd service, /etc config, service account.
    # (See packaging/README.md.) Installed for every Linux generator; the .deb/
    # .rpm control scripts create the 'aqualink' account + enable the unit, and
    # the relocatable .tgz ships packaging/tarball/install.sh which does the same.
    # -----------------------------------------------------------------------
    set(AQ_PACKAGING_DIR "${CMAKE_SOURCE_DIR}/packaging")

    # systemd vendor unit -> /usr/lib/systemd/system; sysusers declaration ->
    # /usr/lib/sysusers.d (both relative to the /usr package prefix).
    install(FILES "${AQ_PACKAGING_DIR}/systemd/aqualink-automate.service"
        DESTINATION lib/systemd/system COMPONENT Runtime)
    install(FILES "${AQ_PACKAGING_DIR}/sysusers.d/aqualink-automate.conf"
        DESTINATION lib/sysusers.d COMPONENT Runtime)

    # Default config -> /etc (ABSOLUTE: /etc is outside the /usr prefix). Marked
    # as a conffile (deb, via the control file) / %config(noreplace) (rpm, below)
    # so admin edits survive upgrades.
    install(FILES "${AQ_PACKAGING_DIR}/config/aqualink-automate.conf"
        DESTINATION /etc/aqualink-automate COMPONENT Runtime)

    # RS-485 udev naming template, alongside the example configs.
    install(FILES "${AQ_PACKAGING_DIR}/udev/60-aqualink-automate.rules.example"
        DESTINATION ${AQ_EXAMPLES_DESTINATION} COMPONENT ExampleConfigs)

    # Ship the packaging sources inside the archive so the relocatable .tgz's
    # install.sh can place them (deb/rpm ignore these — they use the copies above
    # plus their control scripts), and the .tgz installer/uninstaller at the root.
    install(DIRECTORY
        "${AQ_PACKAGING_DIR}/systemd" "${AQ_PACKAGING_DIR}/config" "${AQ_PACKAGING_DIR}/sysusers.d"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aqualink-automate/packaging COMPONENT Runtime)
    install(PROGRAMS
        "${AQ_PACKAGING_DIR}/tarball/install.sh" "${AQ_PACKAGING_DIR}/tarball/uninstall.sh"
        DESTINATION . COMPONENT Runtime)

    # .deb maintainer scripts (create account, enable unit) + conffiles list.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${AQ_PACKAGING_DIR}/deb/postinst"
        "${AQ_PACKAGING_DIR}/deb/prerm"
        "${AQ_PACKAGING_DIR}/deb/postrm"
        "${AQ_PACKAGING_DIR}/deb/conffiles")

    # .rpm scriptlets + protect the config on upgrade + systemd runtime dep.
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${AQ_PACKAGING_DIR}/rpm/post.sh")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${AQ_PACKAGING_DIR}/rpm/preun.sh")
    set(CPACK_RPM_USER_FILELIST "%config(noreplace) /etc/aqualink-automate/aqualink-automate.conf")
    set(CPACK_RPM_PACKAGE_REQUIRES "systemd")

 else()

    # macOS — relocatable bundle mirroring the Linux layout.
    set(AQ_WEB_DESTINATION "${CMAKE_INSTALL_DATADIR}/aqualink-automate/web")
    set(AQ_SSL_DESTINATION "${CMAKE_INSTALL_DATADIR}/aqualink-automate/ssl")
    set(AQ_EXAMPLES_DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples")

    set_target_properties(aqualink-automate PROPERTIES
        INSTALL_RPATH "@loader_path/../${AQ_PRIVATE_LIBDIR}")

    install(TARGETS aqualink-automate RUNTIME DESTINATION bin)

    install(DIRECTORY "${AQ_VCPKG_LIB_DIR}/"
        DESTINATION ${AQ_PRIVATE_LIBDIR}
        COMPONENT Runtime
        FILES_MATCHING
            PATTERN "*.dylib"
            PATTERN "pkgconfig" EXCLUDE
            PATTERN "cmake" EXCLUDE)

    # macOS installer
    set(CPACK_GENERATOR "TGZ;DragNDrop" CACHE STRING "Package targets")

    # DMG-specific configuration
    set(CPACK_DMG_VOLUME_NAME "Aqualink Automate")
    set(CPACK_DMG_FORMAT "UDZO")

endif()

# ---------------------------------------------------------------------------
# Asset installs (common; destinations chosen per-platform above)
# ---------------------------------------------------------------------------

install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ssl/ DESTINATION ${AQ_SSL_DESTINATION} COMPONENT SslAssets)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/web/ DESTINATION ${AQ_WEB_DESTINATION} COMPONENT WebAssets)

# Stamp the installed service worker's cache name with the build version (mirrors
# the dev-tree POST_BUILD step in src/CMakeLists.txt) so a packaged upgrade purges
# the previous version's offline shell on activate. Declared after the web-asset
# directory install so it runs once those files are staged.
install(CODE "
    execute_process(COMMAND \"${CMAKE_COMMAND}\"
        -D \"SW_JS=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${AQ_WEB_DESTINATION}/sw.js\"
        -D \"AQ_VERSION=${PROJECT_VERSION}\"
        -P \"${CMAKE_SOURCE_DIR}/cmake/stamp_sw_version.cmake\")
" COMPONENT WebAssets)

# Home Assistant companion package (blueprints, helpers package, dashboard) —
# served by the running app for download without needing GitHub reachability
# (e.g. an air-gapped install). Mirrors the dev-tree POST_BUILD copy in
# src/CMakeLists.txt. test-harness/ is CI-only fixture content, excluded from
# the shipped package.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/homeassistant/companion/
    DESTINATION ${AQ_WEB_DESTINATION}/homeassistant
    COMPONENT WebAssets
    PATTERN "test-harness" EXCLUDE)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/
    DESTINATION ${AQ_EXAMPLES_DESTINATION}
    COMPONENT ExampleConfigs
    FILES_MATCHING PATTERN "*.conf")

#------------------------------------------------------------------------------
#
#    PACKAGE METADATA
#
#------------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_CONTACT "Iain Chesworth")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${CMAKE_PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_VENDOR "")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_CHECKSUM SHA512)
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# DEB/RPM architecture and metadata defaults. These come BEFORE the prerelease
# block so a prerelease build can override CPACK_RPM_PACKAGE_RELEASE.
#
# The architecture is DERIVED from the build's target processor (set by the
# toolchain: x86_64 or aarch64) rather than hardcoded, so a native arm64 build
# is labelled arm64/aarch64 — otherwise an arm64 .deb would be stamped amd64 and
# apt would refuse to install it on a Raspberry Pi. DEB-DEFAULT/RPM-DEFAULT embed
# this architecture in the package filename, so the two arches never collide.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(AQ_ARCH_LABEL "arm64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "aarch64")
else()
    set(AQ_ARCH_LABEL "amd64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_RPM_PACKAGE_RELEASE 1)

# Encode any prerelease label (alpha/beta/rc) so prerelease packages do not
# collide with — and sort BEFORE — their eventual final release.
if(PROJECT_VERSION_PRERELEASE)
    # Archive/installer filenames (ZIP/TGZ/NSIS/DMG) carry the label directly.
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${PROJECT_VERSION_PRERELEASE}-${CMAKE_SYSTEM_NAME}")
    # Debian: '~' sorts a prerelease lower than the final (1.0.0~beta.1 < 1.0.0).
    set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}~${PROJECT_VERSION_PRERELEASE}")
    # RPM: encode the prerelease in the Release field (0.<pre>) so it sorts
    # before the final release's Release value (1).
    set(CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RPM_PACKAGE_RELEASE "0.${PROJECT_VERSION_PRERELEASE}")
endif()

# Disambiguate the per-arch Linux archive (TGZ). The DEB/RPM names already embed
# the architecture (via *-DEFAULT + CPACK_*_PACKAGE_ARCHITECTURE) and ignore
# CPACK_PACKAGE_FILE_NAME, but the TGZ name is "<name>-<version>[-<pre>]-Linux"
# with NO arch — so an x64 and an arm64 build would produce identically named
# tarballs that clobber each other when every platform's packages are gathered
# into a single GitHub release. Fold the arch into CPACK_PACKAGE_FILE_NAME on
# Linux (this drives the TGZ name; deb/rpm are unaffected — verified empirically:
# CPACK_ARCHIVE_FILE_NAME is NOT honoured by the archive generator here, the TGZ
# follows CPACK_PACKAGE_FILE_NAME). Windows/macOS keep their single-arch names.
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    if(PROJECT_VERSION_PRERELEASE)
        set(CPACK_PACKAGE_FILE_NAME
            "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${PROJECT_VERSION_PRERELEASE}-${CMAKE_SYSTEM_NAME}-${AQ_ARCH_LABEL}")
    else()
        set(CPACK_PACKAGE_FILE_NAME
            "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${AQ_ARCH_LABEL}")
    endif()
endif()

#------------------------------------------------------------------------------
#
#
#
#
#------------------------------------------------------------------------------

include(CPack)

# Add a custom target to enable CPack to be run under Visual Studio
add_custom_target(
	pack-${PROJECT_NAME}
	COMMAND ${CMAKE_CPACK_COMMAND} -C $<CONFIGURATION> --config ${CPACK_OUTPUT_CONFIG_FILE}
	COMMENT "Running CPack. Please wait..."
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)

set_target_properties(pack-${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
