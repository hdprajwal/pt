cmake_minimum_required(VERSION 3.19)
project(pt C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# libghostty-vt is built by ghostty's CMake wrapper via `zig build lib-vt`.
# Resolve zig from the environment so builds follow the active toolchain.
find_program(PT_ZIG_EXECUTABLE zig HINTS "$ENV{HOME}/.local/opt/zig-0.15.2" REQUIRED NO_CACHE)
set(ZIG_EXECUTABLE "${PT_ZIG_EXECUTABLE}" CACHE FILEPATH "Path to the Zig compiler" FORCE)

FetchContent_Declare(ghostty
  GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
  GIT_TAG ae52f97dcac558735cfa916ea3965f247e5c6e9e
)
FetchContent_MakeAvailable(ghostty)

find_package(PkgConfig REQUIRED)
# 4.16 is the first release with CSS custom properties (:root / var()), which
# src/style.css depends on entirely — older GTK parses it into an unstyled app.
pkg_check_modules(GTK  REQUIRED IMPORTED_TARGET gtk4>=4.16)
pkg_check_modules(ADW  REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(JSON REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)

# Compile CSS into the binary as a GResource.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(PT_RES_C ${CMAKE_BINARY_DIR}/pt-resources.c)
add_custom_command(
  OUTPUT ${PT_RES_C}
  COMMAND ${GLIB_COMPILE_RESOURCES}
          --target=${PT_RES_C} --generate-source
          --sourcedir=${CMAKE_SOURCE_DIR}/src
          ${CMAKE_SOURCE_DIR}/src/pt.gresource.xml
  DEPENDS src/pt.gresource.xml src/style.css
  COMMENT "Compiling GResources")

add_executable(pt src/main.c src/pt-style.c src/pt-window.c src/pt-sidebar.c src/pt-tab-strip.c src/pt-statusline.c src/pt-project-bar.c src/pt-pane-grid.c src/pt-palette.c src/pt-settings.c src/pt-terminal.c src/pt-keymap.c src/pt-git-monitor.c ${PT_RES_C})
target_link_libraries(pt PRIVATE ptcore PkgConfig::GTK PkgConfig::ADW)

add_library(ptcore STATIC
  src/pt-config.c
  src/pt-fuzzy.c
  src/pt-git-parse.c
  src/pt-status-parse.c
  src/pt-split-tree.c
  src/pt-theme.c
  src/pt-session.c
  src/pt-term-core.c)
target_include_directories(ptcore PUBLIC src)
target_link_libraries(ptcore PUBLIC ghostty-vt PkgConfig::JSON PkgConfig::GLIB util m)

enable_testing()

add_executable(test-git-parse tests/test-git-parse.c)
target_link_libraries(test-git-parse PRIVATE ptcore)
add_test(NAME git-parse COMMAND test-git-parse)

add_executable(test-status-parse tests/test-status-parse.c)
target_link_libraries(test-status-parse PRIVATE ptcore)
add_test(NAME status-parse COMMAND test-status-parse)

add_executable(test-fuzzy tests/test-fuzzy.c)
target_link_libraries(test-fuzzy PRIVATE ptcore)
add_test(NAME fuzzy COMMAND test-fuzzy)

add_executable(test-split-tree tests/test-split-tree.c)
target_link_libraries(test-split-tree PRIVATE ptcore)
add_test(NAME split-tree COMMAND test-split-tree)

add_executable(test-session tests/test-session.c)
target_link_libraries(test-session PRIVATE ptcore)
add_test(NAME session COMMAND test-session)

add_executable(test-term-core tests/test-term-core.c)
target_link_libraries(test-term-core PRIVATE ptcore)
add_test(NAME term-core COMMAND test-term-core)

add_executable(test-config tests/test-config.c)
target_link_libraries(test-config PRIVATE ptcore)
add_test(NAME config COMMAND test-config)

add_executable(test-theme tests/test-theme.c)
target_link_libraries(test-theme PRIVATE ptcore)
add_test(NAME theme COMMAND test-theme)

add_executable(test-keymap tests/test-keymap.c src/pt-keymap.c)
target_include_directories(test-keymap PRIVATE src)
target_link_libraries(test-keymap PRIVATE PkgConfig::GTK ghostty-vt PkgConfig::GLIB)
add_test(NAME keymap COMMAND test-keymap)
