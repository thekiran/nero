cmake_minimum_required(VERSION 3.21)
project(nero_development LANGUAGES C)

# Additive, offline targets only. The legacy Linux Makefile remains authoritative.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(NERO_SANITIZERS "Enable ASan and UBSan for offline development targets" OFF)
option(NERO_STATIC_ANALYSIS "Enable MSVC code analysis for foundation targets" OFF)
# Source baseline hashes are an optional audit, not a quality gate. Enabling
# this registers the non-gating source-baseline-audit test; it is OFF by
# default so a legitimate source change never fails the default test run.
option(NERO_SOURCE_AUDIT "Register the non-gating source baseline hash audit" OFF)
include(CTest)

include(cmake/NeroTargets.cmake)

set(NERO_CLI_SUPPORT_SOURCES
    src/cli/parser/parser.c
    src/cli/input.c
    src/cli/render.c
    src/cli/operation_ui.c
    src/cli/commands.c
    src/cli/ascii.c
)

add_executable(nero-platform-info src/modules/platform_info.c src/core/platform.c)
nero_target(nero-platform-info)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME foundation COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/tests/integration/test_foundation.py" $<TARGET_FILE:nero-platform-info>)
    # Non-gating: only registered when explicitly requested, so the default
    # ctest run (the CI quality gate) never includes the source hash audit.
    if(NERO_SOURCE_AUDIT)
        add_test(NAME source-baseline-audit COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/audit/source_baseline_audit.py")
    endif()
endif()

if(WIN32)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        # MinGW-w64 leg: interactive CLI with TCP diagnostics and the port
        # scanner. The raw packet engine (-Syn) is excluded; Windows blocks
        # raw TCP packet injection for desktop applications.
        message(STATUS "Windows MinGW leg: Nero CLI, interface listing, TCP diagnostics, port scanner")
        find_package(Threads REQUIRED)
        add_executable(nero
            src/main.c
            src/cli/terminal.c
            ${NERO_CLI_SUPPORT_SOURCES}
            src/modules/portscanner.c
            src/platform/windows/select_interface_windows.c)
        nero_target(nero)
        target_compile_definitions(nero PRIVATE UNICODE_SUPPORT NERO_SYN_UNAVAILABLE)
        target_link_libraries(nero PRIVATE Threads::Threads ws2_32 iphlpapi)
        target_link_options(nero PRIVATE -static) # self-contained exe (winpthread/libgcc)
        if(BUILD_TESTING)
            add_executable(win-interfaces tests/interfaces.c
                src/platform/windows/select_interface_windows.c)
            nero_target(win-interfaces)
            target_link_libraries(win-interfaces PRIVATE ws2_32 iphlpapi)
            target_link_options(win-interfaces PRIVATE -static)
            add_test(NAME windows-native-cli COMMAND ${Python3_EXECUTABLE}
                "${PROJECT_SOURCE_DIR}/tests/windows/test_cli.py"
                $<TARGET_FILE:nero> $<TARGET_FILE:win-interfaces>)
            set_tests_properties(windows-native-cli PROPERTIES TIMEOUT 120)
        endif()
    else()
        message(STATUS "Windows foundation only: Nero CLI and interface listing are Unsupported")
    endif()
elseif(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(nero-safe src/main.c src/cli/terminal.c ${NERO_CLI_SUPPORT_SOURCES})
    nero_target(nero-safe)
    target_compile_definitions(nero-safe PRIVATE UNICODE_SUPPORT NERO_SAFE_TEST NERO_NETWORK_UNAVAILABLE)
    target_link_libraries(nero-safe PRIVATE Threads::Threads)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(nero-safe PRIVATE _POSIX_C_SOURCE=200809L)
        set(interface_source src/platform/linux/select_interface.c)
    else()
        set(interface_source src/platform/macos/select_interface_macos.c)
    endif()
    if(BUILD_TESTING)
        add_executable(simulation tests/simulation.c tests/mocks/network_handler.c)
        nero_target(simulation)
        add_test(NAME simulation COMMAND simulation)
        add_executable(interfaces tests/interfaces.c ${interface_source})
        nero_target(interfaces)
        if(APPLE)
            add_executable(interfaces-mock tests/interfaces_mock_macos.c)
            nero_target(interfaces-mock)
            find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
            find_library(SYSTEMCONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
            foreach(interface_target interfaces interfaces-mock)
                target_link_libraries(${interface_target} PRIVATE
                    ${COREFOUNDATION_FRAMEWORK} ${SYSTEMCONFIGURATION_FRAMEWORK})
            endforeach()
        endif()
        foreach(mock_target nero-mock nero-faults)
            if(mock_target STREQUAL "nero-faults")
                set(terminal_source tests/mocks/terminal_faults.c)
            else()
                set(terminal_source src/cli/terminal.c)
            endif()
            add_executable(${mock_target} src/main.c ${terminal_source} ${NERO_CLI_SUPPORT_SOURCES} tests/mocks/network_handler.c)
            nero_target(${mock_target})
            target_compile_definitions(${mock_target} PRIVATE UNICODE_SUPPORT NERO_SAFE_TEST)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_compile_definitions(${mock_target} PRIVATE _POSIX_C_SOURCE=200809L)
            endif()
            target_link_libraries(${mock_target} PRIVATE Threads::Threads)
        endforeach()
        add_test(NAME terminal-quality COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_terminal_quality.py" $<TARGET_FILE:nero-mock>)
        add_executable(operation-layout tests/operation_layout_fixture.c src/cli/operation_ui.c)
        nero_target(operation-layout)
        add_test(NAME operation-layout COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_operation_layout.py" $<TARGET_FILE:operation-layout>)
        set_tests_properties(operation-layout PROPERTIES TIMEOUT 30)
        set_tests_properties(terminal-quality PROPERTIES TIMEOUT 120)
        add_test(NAME legacy-safe-regression COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_safe.py" $<TARGET_FILE_DIR:nero-safe>)
        set_tests_properties(legacy-safe-regression PROPERTIES TIMEOUT 120)
    endif()
else()
    message(FATAL_ERROR "Unsupported operating system")
endif()
