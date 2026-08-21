cmake_minimum_required(VERSION 3.19)
project(pt C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Must be set before FetchContent pulls in ghostty. With no build type CMake
# passes no -O flag to the C compiler, and ghostty's CMake only maps a build
# type to -Doptimize when one is set (there is no fallback on that path), so
# zig builds libghostty-vt in its Debug default. That costs ~1.8ms per line of
# VT output — seconds to print a directory listing — versus microseconds when
# the parser is optimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

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

# pt ships ghostty's terminfo entry rather than trusting the machine to have
# ghostty installed, so it needs the terminfo compiler at build time.
find_program(PT_TIC tic)
if(NOT PT_TIC)
  message(FATAL_ERROR
    "tic not found. It is the terminfo compiler and comes with ncurses "
    "(the `ncurses` package on Fedora and Arch, `ncurses-bin` on Debian).")
endif()

# Staged inside the build tree as well as installed, because ctest and ./dev.sh
# run an uninstalled build: without this the tests and dev runs would resolve
# xterm-ghostty from whatever ghostty the developer happens to have installed
# system-wide, and a pt install that shipped no entry at all would still look
# fine here. tic writes both x/xterm-ghostty and g/ghostty, since the entry
# declares `ghostty` as an alias, so both are listed as outputs: pt resolves
# either name and a build that produced only one is a broken build.
set(PT_TERMINFO_DIR ${CMAKE_BINARY_DIR}/share/pt/terminfo)
add_custom_command(
  OUTPUT ${PT_TERMINFO_DIR}/x/xterm-ghostty ${PT_TERMINFO_DIR}/g/ghostty
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PT_TERMINFO_DIR}
  COMMAND ${PT_TIC} -x -o ${PT_TERMINFO_DIR}
          ${CMAKE_SOURCE_DIR}/share/terminfo/xterm-ghostty.src
  DEPENDS share/terminfo/xterm-ghostty.src
  COMMENT "Compiling terminfo")
add_custom_target(pt-terminfo ALL
  DEPENDS ${PT_TERMINFO_DIR}/x/xterm-ghostty ${PT_TERMINFO_DIR}/g/ghostty)

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
          src/icons/scalable/actions/pt-zed-symbolic.svg
          src/icons/scalable/actions/pt-panel-right-symbolic.svg
          src/icons/scalable/actions/pt-git-branch-symbolic.svg
  COMMENT "Compiling GResources")

add_executable(pt src/main.c src/pt-style.c src/pt-window.c src/pt-sidebar.c src/pt-info-panel.c src/pt-tab-strip.c src/pt-statusline.c src/pt-project-bar.c src/pt-pane-grid.c src/pt-command-palette.c src/pt-settings.c src/pt-overlay.c src/pt-rowlist.c src/pt-terminal.c src/pt-keymap.c src/pt-git-monitor.c src/pt-agent-monitor.c ${PT_RES_C})
target_link_libraries(pt PRIVATE ptcore PkgConfig::GTK PkgConfig::ADW)
# The running binary is how pt locates its own data, so a build tree pt has to
# find build/share/pt/terminfo beside it.
add_dependencies(pt pt-terminfo)

add_library(ptcore STATIC
  src/pt-agent-latch.c
  src/pt-agent.c
  src/pt-agent-history.c
  src/pt-agent-report.c
  src/pt-agent-session.c
  src/pt-claude-usage.c
  src/pt-codex-usage.c
  src/pt-config.c
  src/pt-fuzzy.c
  src/pt-git-parse.c
  src/pt-integration.c
  src/pt-json-read.c
  src/pt-link.c
  src/pt-path.c
  src/pt-usage.c
  src/pt-status-parse.c
  src/pt-style-css.c
  src/pt-split-tree.c
  src/pt-sprite.c
  src/pt-theme.c
  src/pt-search.c
  src/pt-session.c
  src/pt-term-core.c
  src/pt-workspace.c
  src/pt-zoom-state.c)
target_include_directories(ptcore PUBLIC src)
# Static: the shared imported target leaves the binary with a RUNPATH into
# the build tree and a runtime dependency no user machine can satisfy.
target_link_libraries(ptcore PUBLIC ghostty-vt-static PkgConfig::JSON PkgConfig::GLIB util m)

include(GNUInstallDirs)
install(TARGETS pt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/applications/dev.hdprajwal.pt.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/icons/hicolor/scalable/apps/dev.hdprajwal.pt.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(DIRECTORY share/prompt/ DESTINATION ${CMAKE_INSTALL_DATADIR}/pt/prompt)
install(DIRECTORY ${PT_TERMINFO_DIR}/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pt/terminfo)

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

add_executable(test-path tests/test-path.c)
target_link_libraries(test-path PRIVATE ptcore)
add_test(NAME path COMMAND test-path)

add_executable(test-link tests/test-link.c)
target_link_libraries(test-link PRIVATE ptcore)
add_test(NAME link COMMAND test-link)

add_executable(test-split-tree tests/test-split-tree.c)
target_link_libraries(test-split-tree PRIVATE ptcore)
add_test(NAME split-tree COMMAND test-split-tree)

add_executable(test-session tests/test-session.c)
target_link_libraries(test-session PRIVATE ptcore)
add_test(NAME session COMMAND test-session)

add_executable(test-term-core tests/test-term-core.c)
target_link_libraries(test-term-core PRIVATE ptcore)
# The terminfo tests read the entry this build compiled, so they need its path
# and the build has to produce it first.
target_compile_definitions(test-term-core PRIVATE
  PT_TERMINFO_BUILD_DIR="${PT_TERMINFO_DIR}")
add_dependencies(test-term-core pt-terminfo)
add_test(NAME term-core COMMAND test-term-core)

add_executable(test-search tests/test-search.c)
target_link_libraries(test-search PRIVATE ptcore)
add_test(NAME search COMMAND test-search)

add_executable(test-config tests/test-config.c)
target_link_libraries(test-config PRIVATE ptcore)
add_test(NAME config COMMAND test-config)

add_executable(test-theme tests/test-theme.c)
target_link_libraries(test-theme PRIVATE ptcore)
add_test(NAME theme COMMAND test-theme)

add_executable(test-style tests/test-style.c)
target_link_libraries(test-style PRIVATE ptcore)
add_test(NAME style COMMAND test-style)

add_executable(test-sprite tests/test-sprite.c)
target_link_libraries(test-sprite PRIVATE ptcore)
add_test(NAME sprite COMMAND test-sprite)

add_executable(test-osc-scan tests/test-osc-scan.c)
target_link_libraries(test-osc-scan PRIVATE ptcore)
add_test(NAME osc-scan COMMAND test-osc-scan)

add_executable(test-workspace tests/test-workspace.c)
target_link_libraries(test-workspace PRIVATE ptcore)
add_test(NAME workspace COMMAND test-workspace)

add_executable(test-agent tests/test-agent.c)
target_link_libraries(test-agent PRIVATE ptcore)
add_test(NAME agent COMMAND test-agent)

add_executable(test-agent-latch tests/test-agent-latch.c)
target_link_libraries(test-agent-latch PRIVATE ptcore)
add_test(NAME agent-latch COMMAND test-agent-latch)

add_executable(test-agent-history tests/test-agent-history.c)
target_link_libraries(test-agent-history PRIVATE ptcore)
add_test(NAME agent-history COMMAND test-agent-history)

add_executable(test-agent-session tests/test-agent-session.c)
target_link_libraries(test-agent-session PRIVATE ptcore)
# The reporter tests spawn `pt agent-report` for real, so the test needs pt's
# path and the build has to produce it first.
target_compile_definitions(test-agent-session PRIVATE
  PT_AGENT_REPORT_BIN="$<TARGET_FILE:pt>")
add_dependencies(test-agent-session pt)
add_test(NAME agent-session COMMAND test-agent-session)

add_executable(test-integration tests/test-integration.c)
target_link_libraries(test-integration PRIVATE ptcore)
add_test(NAME integration COMMAND test-integration)

add_executable(test-usage tests/test-usage.c)
target_link_libraries(test-usage PRIVATE ptcore)
add_test(NAME usage COMMAND test-usage)

add_executable(test-codex-usage tests/test-codex-usage.c)
target_link_libraries(test-codex-usage PRIVATE ptcore)
add_test(NAME codex-usage COMMAND test-codex-usage)

add_executable(test-claude-usage tests/test-claude-usage.c)
target_link_libraries(test-claude-usage PRIVATE ptcore)
add_test(NAME claude-usage COMMAND test-claude-usage)

add_executable(test-keymap tests/test-keymap.c src/pt-keymap.c)
target_include_directories(test-keymap PRIVATE src)
target_link_libraries(test-keymap PRIVATE PkgConfig::GTK ghostty-vt PkgConfig::GLIB)
add_test(NAME keymap COMMAND test-keymap)

add_executable(test-zoom-state tests/test-zoom-state.c)
target_link_libraries(test-zoom-state PRIVATE ptcore)
add_test(NAME zoom-state COMMAND test-zoom-state)
