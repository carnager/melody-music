# SPDX-License-Identifier: GPL-3.0-only

add_library(melody_status_ui STATIC status_icon.cpp status_icon.hpp)
target_include_directories(melody_status_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(melody_status_ui PUBLIC Trackknife::Protocol Qt6::Concurrent Qt6::Widgets
    PRIVATE Qt6::Svg trackknife_project_options trackknife_project_warnings)
qt_add_resources(melody_status_ui status_icons PREFIX /melody/status
    BASE ${PROJECT_SOURCE_DIR}/packaging/icons
    FILES ${PROJECT_SOURCE_DIR}/packaging/icons/melody.svg
          ${PROJECT_SOURCE_DIR}/packaging/icons/melody-symbolic.svg)

qt_add_executable(melody_status main.cpp)
set_target_properties(melody_status PROPERTIES OUTPUT_NAME melody-status)
target_link_libraries(melody_status PRIVATE melody_status_ui trackknife_project_options
    trackknife_project_warnings)
if(APPLE)
    target_sources(melody_status PRIVATE macos_status.mm)
    target_link_libraries(melody_status PRIVATE "-framework AppKit")
endif()
install(TARGETS melody_status RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT status-icon)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/autostart/melody-status.desktop
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg/autostart COMPONENT status-icon)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/icons/melody-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status COMPONENT status-icon)
if(APPLE)
    install(FILES ${PROJECT_SOURCE_DIR}/packaging/launchd/org.melody.status.plist
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melody COMPONENT status-icon)
endif()
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    qt_add_executable(melody_status_tests ${PROJECT_SOURCE_DIR}/tests/status_icon_test.cpp)
    target_link_libraries(melody_status_tests PRIVATE melody_status_ui Qt6::Test Qt6::Network
        trackknife_project_options trackknife_project_warnings)
    add_test(NAME status-icon COMMAND melody_status_tests)
    set_tests_properties(status-icon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()
