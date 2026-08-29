# QML module LuneOS.Foreign: export an item's rectangle through
# wl_webos_foreign for external video pipelines to render into.

find_package(Qt6 REQUIRED COMPONENTS Gui Quick)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

set(CMAKE_AUTOMOC ON)

add_library(luneosforeignplugin SHARED
    plugin.cpp
    foreignexportedregion.cpp
)
target_include_directories(luneosforeignplugin PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(luneosforeignplugin
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    ${WAYLAND_LIBRARIES}
    wayland-webos-client
)

install(TARGETS luneosforeignplugin DESTINATION lib/qml/LuneOS/Foreign)
install(FILES qmldir DESTINATION lib/qml/LuneOS/Foreign)
