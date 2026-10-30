# MPDM desktop GUI (Qt6 Widgets). Built only when -DMPDM_BUILD_GUI=ON and Qt6 is
# available. Links the shared mpdm_core backend.
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

set(MPDM_GUI_SOURCES
    main.cpp
    mainwindow.cpp
    dialogs.cpp
    popup.cpp
    fileicon.cpp
    updater.cpp
    fonts.qrc
)
if(WIN32)
    list(APPEND MPDM_GUI_SOURCES app.rc)  # embeds app.ico (Windows only)
endif()

qt_add_executable(mpdm_gui ${MPDM_GUI_SOURCES})
target_include_directories(mpdm_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpdm_gui PRIVATE mpdm_core Qt6::Widgets Qt6::Network)
# Version shown in the app and compared against GitHub releases by the updater.
target_compile_definitions(mpdm_gui PRIVATE MPDM_VERSION="${MPDM_VERSION}")

if(WIN32)
    set_target_properties(mpdm_gui PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Linux: install the GUI binary, its .desktop launcher and icon (used by CPack).
if(UNIX AND NOT APPLE)
    install(TARGETS mpdm_gui RUNTIME DESTINATION bin)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/mpdm.desktop
            DESTINATION share/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/mpdm.png
            DESTINATION share/icons/hicolor/256x256/apps)
endif()
