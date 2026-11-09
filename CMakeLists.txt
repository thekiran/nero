cmake_minimum_required(VERSION 3.21)
project(nero_development LANGUAGES C)

# Additive, offline targets only. The legacy Linux Makefile remains authoritative.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(NERO_SANITIZERS "Enable ASan and UBSan for offline development targets" OFF)
set(NERO_RELEASE_VERSION "" CACHE STRING "Tag to stamp into the native Windows release CLI")
if(NERO_RELEASE_VERSION AND NOT NERO_RELEASE_VERSION MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$")
    message(FATAL_ERROR "NERO_RELEASE_VERSION must be a vMAJOR.MINOR.PATCH tag")
endif()
option(NERO_STATIC_ANALYSIS "Enable MSVC code analysis for foundation targets" OFF)
# Source baseline hashes are an optional audit, not a quality gate. Enabling
# this registers the non-gating source-baseline-audit test; it is OFF by
# default so a legitimate source change never fails the default test run.
option(NERO_SOURCE_AUDIT "Register the non-gating source baseline hash audit" OFF)
include(CTest)

include(cmake/NeroTargets.cmake)

set(NERO_CLI_SUPPORT_SOURCES
    src/core/log.c
    src/cli/parser/parser.c
    src/cli/input.c
    src/cli/history.c
    src/cli/render.c
    src/cli/operation_ui.c
    src/cli/commands.c
    src/cli/ascii.c
)

add_executable(nero-platform-info src/modules/platform_info.c src/core/platform.c)
nero_target(nero-platform-info)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(parser-boundaries tests/parser_boundaries.c src/cli/parser/parser.c)
    nero_target(parser-boundaries)
    add_test(NAME parser-boundaries COMMAND parser-boundaries)
    if(UNIX)
        add_test(NAME installation-lifecycle COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_installation.py")
    endif()
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
            src/modules/scan_session.c
            src/platform/windows/select_interface_windows.c)
        nero_target(nero)
        target_compile_definitions(nero PRIVATE UNICODE_SUPPORT NERO_SYN_UNAVAILABLE)
        if(NERO_RELEASE_VERSION)
            target_compile_definitions(nero PRIVATE "NERO_BUILD_VERSION=\"${NERO_RELEASE_VERSION}\"")
        endif()
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
            add_test(NAME windows-global-install COMMAND powershell.exe -NoProfile -NonInteractive
                -File "${PROJECT_SOURCE_DIR}/tests/windows/global_install.ps1"
                -BinaryPath $<TARGET_FILE:nero>)
            set_tests_properties(windows-global-install PROPERTIES TIMEOUT 90)
            add_test(NAME history-persistence COMMAND ${Python3_EXECUTABLE}
                "${PROJECT_SOURCE_DIR}/tests/test_history.py" $<TARGET_FILE:nero>)
            set_tests_properties(history-persistence PROPERTIES TIMEOUT 120)
            if(NERO_RELEASE_VERSION)
                set_tests_properties(windows-native-cli windows-global-install PROPERTIES
                    ENVIRONMENT "NERO_EXPECTED_VERSION=${NERO_RELEASE_VERSION}")
            endif()
            foreach(fixture compat_boundaries console_restore socket_loopback)
                add_executable(windows-${fixture} tests/windows/${fixture}.c)
                nero_target(windows-${fixture})
                target_link_libraries(windows-${fixture} PRIVATE ws2_32)
                target_link_options(windows-${fixture} PRIVATE -static)
                add_test(NAME windows-${fixture} COMMAND windows-${fixture})
                set_tests_properties(windows-${fixture} PROPERTIES TIMEOUT 30)
            endforeach()
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
            set(mock_support_sources ${NERO_CLI_SUPPORT_SOURCES})
            if(mock_target STREQUAL "nero-faults")
                list(REMOVE_ITEM mock_support_sources src/cli/input.c)
                list(APPEND mock_support_sources tests/mocks/input_faults.c)
            endif()
            add_executable(${mock_target} src/main.c ${terminal_source} ${mock_support_sources} tests/mocks/network_handler.c)
            nero_target(${mock_target})
            target_compile_definitions(${mock_target} PRIVATE UNICODE_SUPPORT NERO_SAFE_TEST)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_compile_definitions(${mock_target} PRIVATE _POSIX_C_SOURCE=200809L)
            endif()
            target_link_libraries(${mock_target} PRIVATE Threads::Threads)
        endforeach()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(interfaces-mock tests/interfaces.c tests/mocks/interfaces_linux.c src/platform/linux/select_interface.c)
            nero_target(interfaces-mock)
            target_link_options(interfaces-mock PRIVATE -Wl,--wrap=opendir,--wrap=getifaddrs,--wrap=freeifaddrs)
            add_executable(interface-fallback tests/linux/interface_fallback.c src/platform/linux/select_interface.c)
            nero_target(interface-fallback)
            target_link_options(interface-fallback PRIVATE -Wl,--wrap=opendir,--wrap=getifaddrs,--wrap=freeifaddrs)
            add_test(NAME interface-fallback COMMAND interface-fallback)
        endif()
        add_test(NAME terminal-quality COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_terminal_quality.py" $<TARGET_FILE:nero-mock>)
        add_executable(operation-layout tests/operation_layout_fixture.c src/cli/operation_ui.c)
        nero_target(operation-layout)
        add_test(NAME operation-layout COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_operation_layout.py" $<TARGET_FILE:operation-layout>)
        set_tests_properties(operation-layout PROPERTIES TIMEOUT 30)
        set_tests_properties(terminal-quality PROPERTIES TIMEOUT 120)
        add_test(NAME history-persistence COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_history.py" $<TARGET_FILE:nero-safe>)
        set_tests_properties(history-persistence PROPERTIES TIMEOUT 120)
        add_test(NAME legacy-safe-regression COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/test_safe.py" $<TARGET_FILE_DIR:nero-safe>)
        set_tests_properties(legacy-safe-regression PROPERTIES TIMEOUT 120)
    endif()
else()
    message(FATAL_ERROR "Unsupported operating system")
endif()

# This fixture substitutes socket syscalls in the production scanner. It never
# creates real sockets, and uses native pthreads to exercise shared work claims.
if(BUILD_TESTING AND NOT MSVC)
    find_package(Threads REQUIRED)
    add_executable(scan-session tests/scan_session.c src/modules/scan_session.c src/core/log.c)
    nero_target(scan-session)
    target_link_libraries(scan-session PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(scan-session PRIVATE ws2_32)
        target_link_options(scan-session PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(scan-session PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    add_test(NAME scan-session COMMAND scan-session)
    set_tests_properties(scan-session PROPERTIES TIMEOUT 60)
    add_executable(portscanner-reliability tests/portscanner_reliability.c src/modules/portscanner.c src/modules/scan_session.c src/cli/operation_ui.c src/core/log.c)
    nero_target(portscanner-reliability)
    target_compile_definitions(portscanner-reliability PRIVATE UNICODE_SUPPORT)
    target_link_libraries(portscanner-reliability PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(portscanner-reliability PRIVATE ws2_32)
        target_link_options(portscanner-reliability PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(portscanner-reliability PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    add_test(NAME portscanner-reliability COMMAND portscanner-reliability)
    set_tests_properties(portscanner-reliability PROPERTIES TIMEOUT 60)
    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Drives real 127.0.0.1 sockets, ptys and the menu lifecycle; a
        # connect() guard aborts on any non-loopback destination.
        add_executable(scanner-runtime tests/linux/scanner_runtime.c src/modules/portscanner.c src/modules/scan_session.c src/cli/operation_ui.c src/core/log.c)
        nero_target(scanner-runtime)
        target_compile_definitions(scanner-runtime PRIVATE UNICODE_SUPPORT)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_compile_definitions(scanner-runtime PRIVATE _POSIX_C_SOURCE=200809L)
        endif()
        target_link_libraries(scanner-runtime PRIVATE Threads::Threads)
        add_test(NAME scanner-runtime COMMAND scanner-runtime)
        set_tests_properties(scanner-runtime PROPERTIES TIMEOUT 300)

        # Deterministic PTY smoke test for live early port selection. Mock
        # sockets only (no real network I/O); a frozen mock clock plus the
        # scanner's test-only NERO_SCAN_TEST_HOOKS signalling make it reliable,
        # never time-based. macOS/Linux only; Windows native console behaviour
        # is covered by the tests/windows fixtures above, not here.
        add_executable(live-select-pty tests/linux/live_select_pty.c src/modules/portscanner.c src/modules/scan_session.c src/cli/operation_ui.c src/core/log.c)
        nero_target(live-select-pty)
        target_compile_definitions(live-select-pty PRIVATE UNICODE_SUPPORT NERO_SCAN_TEST_HOOKS)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_compile_definitions(live-select-pty PRIVATE _POSIX_C_SOURCE=200809L)
        endif()
        target_link_libraries(live-select-pty PRIVATE Threads::Threads)
        add_test(NAME live-select-pty COMMAND live-select-pty)
        set_tests_properties(live-select-pty PROPERTIES TIMEOUT 120)
    endif()
endif()

if(BUILD_TESTING AND NOT MSVC)
    add_executable(logging-fixture tests/logging_fixture.c src/core/log.c)
    nero_target(logging-fixture)
    target_link_libraries(logging-fixture PRIVATE Threads::Threads)
    if(WIN32)
        target_link_options(logging-fixture PRIVATE -static)
        set(logging_cli_target nero)
    else()
        target_compile_definitions(logging-fixture PRIVATE _POSIX_C_SOURCE=200809L)
        set(logging_cli_target nero-safe)
    endif()
    add_test(NAME logging COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/tests/test_logging.py" $<TARGET_FILE:${logging_cli_target}> $<TARGET_FILE:logging-fixture>)
    set_tests_properties(logging PROPERTIES TIMEOUT 120)
endif()

if(BUILD_TESTING)
    get_property(nero_test_names DIRECTORY PROPERTY TESTS)
    # Append rather than overwrite per-test release-version expectations.
    set_property(TEST ${nero_test_names} APPEND PROPERTY ENVIRONMENT "NERO_LOG_FILE=:memory:")
endif()
