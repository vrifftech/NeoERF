cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)

set(NEOSHARED_ROOT "" CACHE PATH
    "Path to the neoshared source repository; defaults to ../neoshared")

if(NOT NEOSHARED_ROOT AND DEFINED ENV{NEOSHARED_ROOT}
   AND NOT "$ENV{NEOSHARED_ROOT}" STREQUAL "")
    set(NEOSHARED_ROOT "$ENV{NEOSHARED_ROOT}" CACHE PATH
        "Path to the neoshared source repository" FORCE)
endif()

if(NOT NEOSHARED_ROOT)
    get_filename_component(NEOSHARED_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../neoshared" ABSOLUTE)
else()
    get_filename_component(NEOSHARED_ROOT "${NEOSHARED_ROOT}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(NEOSHARED_ROOT "${NEOSHARED_ROOT}" CACHE PATH
    "Path to the neoshared source repository" FORCE)

if(NOT EXISTS "${NEOSHARED_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "neoshared was not found at '${NEOSHARED_ROOT}'. "
        "Clone the neoshared repository beside this repository, or configure "
        "with -DNEOSHARED_ROOT=/path/to/neoshared.")
endif()

include("${NEOSHARED_ROOT}/cmake/NeoVersion.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
neo_read_version_header(NEOERF_VERSION
    "${CMAKE_CURRENT_LIST_DIR}/src/core/Version.hpp"
    NEOERF_VERSION_STRING)
project(NeoERFCpp VERSION ${NEOERF_VERSION} LANGUAGES CXX)

if(MSVC)
    foreach(_neo_runtime_flag_var IN ITEMS
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_neo_runtime_flag_var})
            string(REGEX REPLACE "(^|[ /-])MDd?($|[ ]|/)" "\\1MT\\2" ${_neo_runtime_flag_var} "${${_neo_runtime_flag_var}}")
        endif()
    endforeach()
endif()


if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "Release compiler flags" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(neo_force_static_msvc_runtime target_name)
    if(MSVC AND TARGET ${target_name})
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/MTd>
            $<$<NOT:$<CONFIG:Debug>>:/MT>
        )
    endif()
endfunction()

function(neo_force_static_msvc_runtime_for_directory_targets)
    if(MSVC)
        get_property(_neo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_neo_target IN LISTS _neo_targets)
            get_target_property(_neo_type ${_neo_target} TYPE)
            if(NOT _neo_type STREQUAL "INTERFACE_LIBRARY" AND NOT _neo_type STREQUAL "UTILITY")
                neo_force_static_msvc_runtime(${_neo_target})
            endif()
        endforeach()
    endif()
endfunction()

include(GNUInstallDirs)

message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()



find_package(ZLIB REQUIRED)

add_library(neoerf_core
    src/erf/Archive.cpp
    src/erf/Utils.cpp
)
target_include_directories(neoerf_core PUBLIC src)
target_compile_features(neoerf_core PUBLIC cxx_std_17)
target_link_libraries(neoerf_core PUBLIC ZLIB::ZLIB)
if(MSVC)
    target_compile_options(neoerf_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(neoerf_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(neoerf_patcher
    src/erf/ErfPatcher.cpp
)
target_include_directories(neoerf_patcher PUBLIC src)
target_compile_features(neoerf_patcher PUBLIC cxx_std_17)
target_link_libraries(neoerf_patcher PUBLIC neoerf_core neoshared::tslpatcher)
if(MSVC)
    target_compile_options(neoerf_patcher PRIVATE /W4 /permissive-)
else()
    target_compile_options(neoerf_patcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(NEOERF_BUILD_CLI "Build the portable CLI validation harness" ON)
if(NEOERF_BUILD_CLI)
    add_executable(neoerf_cli src/cli/main.cpp)
    target_link_libraries(neoerf_cli PRIVATE neoerf_core neoerf_patcher)
    if(MSVC)
        target_compile_options(neoerf_cli PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neoerf_cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS neoerf_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_NEOERF_TESTS_DEFAULT ON)
else()
    set(_NEOERF_TESTS_DEFAULT OFF)
endif()
option(NEOERF_BUILD_TESTS "Build NeoERF regression tests" ${_NEOERF_TESTS_DEFAULT})
unset(_NEOERF_TESTS_DEFAULT)

if(NEOERF_BUILD_TESTS)
    enable_testing()
    add_executable(neoerf-resource-profile-tests tests/resource_profile_tests.cpp)
    target_link_libraries(neoerf-resource-profile-tests PRIVATE neoerf_core)
    if(MSVC)
        target_compile_options(neoerf-resource-profile-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neoerf-resource-profile-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME neoerf-resource-profile-mapping
             COMMAND neoerf-resource-profile-tests)
endif()


option(NEOERF_BUILD_WX_GUI "Build the wxWidgets desktop GUI when wxWidgets is available" ON)
option(NEOERF_REQUIRE_WX_GUI "Fail configure if wxWidgets is unavailable while NEOERF_BUILD_WX_GUI is ON" OFF)


function(neoerf_link_wx target_name)
    target_compile_definitions(${target_name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    set(_wx_targets)
    foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wx::aui wxWidgets::wxWidgets)
        if(TARGET ${_wx_target})
            list(APPEND _wx_targets ${_wx_target})
        endif()
    endforeach()

    if(_wx_targets)
        target_link_libraries(${target_name} PRIVATE ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PRIVATE ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PRIVATE ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PRIVATE ${wxWidgets_LIBRARIES})
    endif()

    neo_force_static_msvc_runtime(${target_name})
endfunction()


if(NEOERF_BUILD_WX_GUI)
    set(NEOERF_WX_COMPONENTS core base adv aui)
    # vcpkg provides CMake package targets such as wx::core and wx::base.
    # Prefer CONFIG mode on Windows/vcpkg, but keep FindwxWidgets/wx-config as
    # the first choice on Unix-like systems with traditional wxWidgets installs.
    set(_neo_prefer_wx_config_package OFF)
    if(WIN32 OR DEFINED VCPKG_TARGET_TRIPLET OR CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
        set(_neo_prefer_wx_config_package ON)
    endif()

    if(_neo_prefer_wx_config_package)
        find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOERF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOERF_WX_COMPONENTS})
        endif()
    else()
        find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOERF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOERF_WX_COMPONENTS})
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
    endif()

    if(wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets)
        set(NEOERF_WX_SOURCES src/wx/main.cpp)
        if(WIN32)
            neo_configure_windows_version_resource(
                NEOERF_WX_SOURCES_RC
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neoerf.rc.in"
                "neoerf.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neoerf.ico")
            list(APPEND NEOERF_WX_SOURCES "${NEOERF_WX_SOURCES_RC}")
        endif()

        if(WIN32)
            add_executable(NeoERF WIN32 ${NEOERF_WX_SOURCES})
        elseif(APPLE)
            add_executable(NeoERF MACOSX_BUNDLE ${NEOERF_WX_SOURCES})
        else()
            add_executable(NeoERF ${NEOERF_WX_SOURCES})
        endif()
        neo_configure_macos_bundle(NeoERF
            NAME "NeoERF"
            IDENTIFIER "com.vrifftech.neoerf"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neoerf.icns"
        )
        target_link_libraries(NeoERF PRIVATE neoerf_core neoerf_patcher)
        target_include_directories(NeoERF PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
        target_link_libraries(NeoERF PRIVATE neoshared::wx)
        neoerf_link_wx(NeoERF)
        target_compile_features(NeoERF PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(NeoERF PRIVATE /W4 /permissive-)
        else()
            target_compile_options(NeoERF PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        install(TARGETS NeoERF
            BUNDLE DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        if(UNIX AND NOT APPLE)
            foreach(NEO_ICON_SIZE 16 24 32 48 64 128 256)
                install(FILES resources/icons/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/neoerf.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/apps
                    RENAME neoerf.png
                )
            endforeach()
            configure_file(resources/neoerf.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/neoerf.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neoerf.desktop
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
            )
        endif()
    elseif(NEOERF_REQUIRE_WX_GUI)
        message(FATAL_ERROR "wxWidgets was not found, but NEOERF_REQUIRE_WX_GUI is ON. On Windows, install the pinned wxWidgets 3.3.3 overlay with ../neoshared/scripts/install-wxwidgets.ps1 and configure with the vcpkg toolchain. On Debian or Ubuntu, install libwxgtk3.2-dev.")
    else()
        message(WARNING "wxWidgets was not found. Core and CLI targets will still build; install wxWidgets to build NeoERF.")
    endif()
endif()


neo_force_static_msvc_runtime_for_directory_targets()
neo_apply_minimal_release_to_directory_targets()
