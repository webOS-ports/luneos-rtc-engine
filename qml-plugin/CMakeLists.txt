# QML module LuneOS.Foreign: export an item's rectangle through
# wl_webos_foreign for external video pipelines to render into.

find_package(Qt6 REQUIRED COMPONENTS Gui Quick)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

set(CMAKE_AUTOMOC ON)

add_library(luneosforeignplugin SHARED
    plugin.cpp
    foreignexportedregion.cpp
    rtcvideofactory.cpp
)
target_include_directories(luneosforeignplugin PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(luneosforeignplugin
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    ${WAYLAND_LIBRARIES}
    wayland-webos-client
)

# The QtMultimedia GStreamer SPI headers install under the versioned
# include dir, which only the Private target puts on the include path.
# Present when qtmultimedia was built with -DFEATURE_gstreamer_qt_api=ON;
# RtcVideoFactory compiles to a stub otherwise (__has_include).
find_package(Qt6 QUIET COMPONENTS Multimedia MultimediaPrivate)
if(TARGET Qt6::MultimediaPrivate)
    target_link_libraries(luneosforeignplugin Qt6::Multimedia Qt6::MultimediaPrivate)
endif()

install(TARGETS luneosforeignplugin DESTINATION lib/qml/LuneOS/Foreign)
install(FILES qmldir DESTINATION lib/qml/LuneOS/Foreign)
