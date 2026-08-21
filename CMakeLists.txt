cmake_minimum_required(VERSION 3.24)
project(nirisnap VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Test Widgets)
find_package(LayerShellQt REQUIRED)

qt_add_executable(nirisnap
  src/main.cpp
  src/cli-path.cpp
  src/cli-path.hpp
  src/icons.cpp
  src/icons.hpp
  src/instance-lock.cpp
  src/instance-lock.hpp
  src/pin.cpp
  src/pin.hpp
  src/pin-file.cpp
  src/pin-file.hpp
  src/pin-layout.cpp
  src/pin-layout.hpp
  src/capture.cpp
  src/capture.hpp
  src/eyedropper.cpp
  src/eyedropper.hpp
  src/surface-capture.cpp
  src/editor.cpp
  src/editor.hpp
)
qt_add_resources(nirisnap "capture-fonts"
  PREFIX "/fonts"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/Neucha.ttf"
)

target_include_directories(nirisnap PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(nirisnap PRIVATE cxx_std_23)
target_compile_options(nirisnap PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(nirisnap PRIVATE
  NIRISNAP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(nirisnap PRIVATE
  Qt6::Concurrent
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  LayerShellQt::Interface
)


qt_add_executable(nirisnap-smoke
  tests/editor-smoke.cpp
  tests/clipboard-smoke.cpp
  tests/clipboard-smoke.hpp
  src/icons.cpp
  src/icons.hpp
  src/instance-lock.cpp
  src/instance-lock.hpp
  tests/instance-lock-smoke.cpp
  tests/instance-lock-smoke.hpp
  tests/transform-smoke.cpp
  tests/transform-smoke.hpp
  src/cli-path.cpp
  src/cli-path.hpp
  src/capture.cpp
  src/capture.hpp
  src/surface-capture.cpp
  src/editor.cpp
  src/editor.hpp
  src/eyedropper.cpp
  src/eyedropper.hpp
  src/pin-file.cpp
  src/pin-file.hpp
  src/pin-layout.cpp
  src/pin-layout.hpp
  tests/pin-lifecycle-smoke.cpp
  tests/pin-lifecycle-smoke.hpp
  tests/pin-layout-smoke.cpp
  tests/pin-layout-smoke.hpp
)
qt_add_resources(nirisnap-smoke "capture-fonts-smoke"
  PREFIX "/fonts"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/Neucha.ttf"
)
target_include_directories(nirisnap-smoke PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
target_compile_features(nirisnap-smoke PRIVATE cxx_std_23)
target_compile_options(nirisnap-smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nirisnap-smoke PRIVATE
  Qt6::Concurrent
  Qt6::Core
  Qt6::Gui
  Qt6::Test
  Qt6::Widgets
)

install(TARGETS nirisnap
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/nirisnap"
)
install(FILES assets/OFL.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/nirisnap"
  RENAME Neucha-OFL.txt
)
install(FILES nirisnap.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(DIRECTORY plugin/nirisnap
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nirisnap/noctalia-plugin"
)
install(FILES packaging/nirisnap-keybinds.kdl
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nirisnap"
)
