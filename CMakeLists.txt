cmake_minimum_required(VERSION 3.10)
project(luneos-rtc-engine CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED
    gstreamer-1.0
    gstreamer-video-1.0
    glib-2.0
    luna-service2
    pbnjson_cpp
)
find_package(Threads REQUIRED)

add_executable(luneos-rtc-engine src/rtc_engine.cpp)
target_include_directories(luneos-rtc-engine PRIVATE ${DEPS_INCLUDE_DIRS} src)
# resource_mgr_client (uMediaServer) and camera-window-manager
# (g-camera-pipeline) ship bare shared libraries without pkg-config files.
target_link_libraries(luneos-rtc-engine
    ${DEPS_LIBRARIES}
    resource_mgr_client
    camera-window-manager
    Threads::Threads
)

install(TARGETS luneos-rtc-engine DESTINATION sbin)

option(WITH_QML_PLUGIN "Build the LuneOS.Foreign QML module" ON)
if(WITH_QML_PLUGIN)
    add_subdirectory(qml-plugin)
endif()

set(LS2_DIR share/luna-service2)
install(FILES files/org.webosports.rtcengine.service
        DESTINATION ${LS2_DIR}/services.d)
install(FILES files/org.webosports.rtcengine.role.json
        DESTINATION ${LS2_DIR}/roles.d)
install(FILES files/org.webosports.rtcengine.api.json
        DESTINATION ${LS2_DIR}/api-permissions.d)
install(FILES files/org.webosports.rtcengine.perm.json
        DESTINATION ${LS2_DIR}/client-permissions.d)
install(FILES files/org.webosports.rtcengine.groups.json
        DESTINATION ${LS2_DIR}/groups.d)
install(FILES files/org.webosports.rtcengine.manifest.json
        DESTINATION ${LS2_DIR}/manifests.d)
