# Local setup for Online Charts.
set(OCPN_TEST_REPO "opencpn/online-charts-alpha" CACHE STRING "Development repository")
set(OCPN_BETA_REPO "opencpn/online-charts-beta" CACHE STRING "Beta repository")
set(OCPN_RELEASE_REPO "opencpn/online-charts-prod" CACHE STRING "Release repository")

set(PKG_NAME online_charts_pi)
set(PKG_VERSION 1.6.0)
set(PKG_PRERELEASE "")
set(DISPLAY_NAME OnlineCharts)
set(PLUGIN_API_NAME OnlineCharts)
set(PKG_SUMMARY "Online bathymetry, seamarks and marine weather on OpenCPN charts")
set(PKG_DESCRIPTION [=[Draws public Open Waters bathymetry (depth areas in chart or Topo colours, contours, soundings), OpenSeaMap seamarks and an optional OpenStreetMap or Esri base map on the OpenCPN chart, with an Open-Meteo / MET Norway marine forecast: wind barbs, wave and ocean-current arrows, a Weather window, a 72-hour forecast time bar and offline caches. Includes an Open provider button for i-Boating, C-MAP and Garmin at the boat position, and an AIS demo with live vessels from Open Waters AIS. Experimental display; not a navigation source.]=])
set(PKG_AUTHOR "OpenCPN Online Charts contributors")
set(PKG_IS_OPEN_SOURCE "yes")
set(PKG_HOMEPAGE "https://github.com/Sail-Tech/opencpn-online-charts")
set(PKG_INFO_URL "https://github.com/Sail-Tech/opencpn-online-charts")

set(SRC
    src/online_charts_pi.h
    src/online_charts_pi.cpp
    src/online_charts_server.h
    src/online_charts_server.cpp
    src/online_charts_mvt.h
    src/online_charts_mvt.cpp
    src/online_charts_land.h
    src/online_charts_land.cpp
    src/online_charts_overlay.h
    src/online_charts_overlay.cpp
    src/online_charts_preferences.h
    src/online_charts_preferences.cpp
    src/online_charts_render.h
    src/online_charts_render.cpp
    src/online_charts_geohash.h
    src/online_charts_json.h
    src/online_charts_json.cpp
    src/online_charts_http.h
    src/online_charts_http.cpp
    src/online_charts_aisdata.h
    src/online_charts_aisdata.cpp
    src/online_charts_ais.h
    src/online_charts_ais.cpp
    src/online_charts_depth.h
    src/online_charts_depth.cpp
    src/online_charts_quick_panel.h
    src/online_charts_quick_panel.cpp
    src/online_charts_display.h
    src/online_charts_display.cpp
    src/online_charts_weather.h
    src/online_charts_weather.cpp
    src/online_charts_weather_window.h
    src/online_charts_weather_window.cpp
    libs/civetweb/civetweb.c
)
set(PKG_API_LIB api-18)

macro(late_init)
  target_include_directories(${PACKAGE_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/libs/civetweb")
  target_compile_definitions(${PACKAGE_NAME} PRIVATE USE_WEBSOCKET NO_SSL NO_CGI)
  if (WIN32)
    target_link_libraries(${PACKAGE_NAME} ws2_32)
  endif ()
  if (MSVC)
    # OpenCPN ships an old msvcp140.dll (14.12 in 5.14.2) next to opencpn.exe.
    # Since Visual Studio 2022 17.10 std::mutex has a constexpr constructor
    # that needs runtime 14.40 or later; with the older runtime the first lock
    # is an access violation inside msvcp140 (_Mtx_lock).
    target_compile_definitions(${PACKAGE_NAME} PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    # Read the sources as UTF-8 (MSVC assumes the Windows code page) and keep
    # narrow literals in UTF-8 for wxString::FromUTF8.
    target_compile_options(${PACKAGE_NAME} PRIVATE /utf-8)
  endif ()
  if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(${PACKAGE_NAME} Threads::Threads ${CMAKE_DL_LIBS})
  endif ()
endmacro()

macro(add_plugin_libraries)
  add_subdirectory(opencpn-libs/plugin_dc)
  target_link_libraries(${PACKAGE_NAME} ocpn::plugin-dc)
  # WebP decoder (libwebp 1.6.0, BSD licence, decoder sources only) for the
  # Open Waters elevation raster behind the depth colours. wxWidgets 3.2, as
  # used by OpenCPN, has no WebP support.
  file(GLOB ONLINE_CHARTS_WEBP_SRC "${CMAKE_SOURCE_DIR}/libs/libwebp/src/*/*.c")
  add_library(online_charts_webp STATIC ${ONLINE_CHARTS_WEBP_SRC})
  set_target_properties(online_charts_webp PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  target_include_directories(online_charts_webp PUBLIC "${CMAKE_SOURCE_DIR}/libs/libwebp")
  # Keep libwebp's functions private to the plugin, so they never mix with a
  # system libwebp loaded into OpenCPN (for example by GTK on Linux).
  target_compile_definitions(online_charts_webp PUBLIC "WEBP_EXTERN=extern")
  target_link_libraries(${PACKAGE_NAME} online_charts_webp)
  if (UNIX AND NOT APPLE)
    # Debian's wxWidgets builds disable wxWebRequest; use asynchronous HTTPS
    # through libcurl there. macOS and Windows use the wx native backend.
    find_package(CURL REQUIRED)
    target_link_libraries(${PACKAGE_NAME} CURL::libcurl)
  endif ()
  # Public Open Waters tiles only. No provider credentials or protected
  # endpoints are used, and no access controls are bypassed.
endmacro()
