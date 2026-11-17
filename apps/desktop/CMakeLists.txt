# NERO Desktop (nero-gui) — native Qt 6 / Qt Quick frontend over the shared C core.
# Built only when NERO_BUILD_GUI=ON and a Qt 6 toolchain is available, so the
# default non-Qt build of nero / neroconsole is never affected.

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Gui)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(nero-gui
    main.cpp
    backend/NeroController.cpp
    backend/NeroController.h
    # Shared C11 core sources (no Qt inside the core). nero_doctor() is
    # self-contained, matching the existing doctor-render test target.
    ${CMAKE_SOURCE_DIR}/src/core/doctor.c
)

target_include_directories(nero-gui PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})

if(NERO_RELEASE_VERSION)
    target_compile_definitions(nero-gui PRIVATE "NERO_BUILD_VERSION=\"${NERO_RELEASE_VERSION}\"")
endif()

qt_add_qml_module(nero-gui
    URI Nero
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(nero-gui PRIVATE Qt6::Quick Qt6::Gui)

set_target_properties(nero-gui PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE)
