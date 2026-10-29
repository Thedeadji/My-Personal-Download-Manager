# MPDM desktop GUI (Qt6 Widgets). Built only when -DMPDM_BUILD_GUI=ON and Qt6 is
# available. Links the shared mpdm_core backend.
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(mpdm_gui
    main.cpp
    mainwindow.cpp
    dialogs.cpp
    popup.cpp
    fonts.qrc
    app.rc        # embeds app.ico as the exe/shortcut/Explorer icon (Windows)
)
target_include_directories(mpdm_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpdm_gui PRIVATE mpdm_core Qt6::Widgets Qt6::Network)
set_target_properties(mpdm_gui PROPERTIES WIN32_EXECUTABLE ON)
