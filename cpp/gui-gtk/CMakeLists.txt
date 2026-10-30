# MPDM desktop GUI (GTK4 + libadwaita, via gtkmm). Built only when
# -DMPDM_BUILD_GTK_GUI=ON. Links the shared mpdm_core backend; lives alongside
# the Qt GUI (gui/) rather than replacing it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0>=4.14)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.6)

add_executable(mpdm_gtk
    main.cpp
    app.cpp
    mainwindow.cpp
    downloadwindow.cpp
    dialogs.cpp
    preferences.cpp
    charts.cpp
    uiprefs.cpp
    util.cpp
)
target_include_directories(mpdm_gtk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpdm_gtk PRIVATE mpdm_core PkgConfig::GTKMM PkgConfig::LIBADWAITA)

# Linux: install the binary, a launcher whose name matches the application ID
# (so GNOME ties windows and notifications to it) and the icon.
if(UNIX AND NOT APPLE)
    install(TARGETS mpdm_gtk RUNTIME DESTINATION bin)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/com.mypersonaldownloadmanager.MPDM.desktop
            DESTINATION share/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/mpdm.png
            DESTINATION share/icons/hicolor/256x256/apps)
endif()
