# Sly2-Release: Sly 2 as a native source port (see PLAN.md; it was called Sly2-Native until 2026-10-28). Built standalone or from the top-level CMake.
cmake_minimum_required(VERSION 3.20)
project(Sly2Release CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(CANE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

file(GLOB sly2n_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
# (test build: the files' static constructors in reverse order, as MinGW runs them on Windows: its CRT walks the .ctors
# list backwards. A Linux run of it shows a Windows-only static initialization order dependency; PACKAGING.md section 7)
option(SLY2_REVERSE_INIT_ORDER "Link Sly2-Release's sources in reverse order (init-order test build)" OFF)
if(SLY2_REVERSE_INIT_ORDER)
  list(SORT sly2n_sources)
  list(REVERSE sly2n_sources)
endif()
add_executable(Sly2-Release ${sly2n_sources})
target_include_directories(Sly2-Release PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CANE_ROOT}/Libraries/include" "${CANE_ROOT}/platform"
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(Sly2-Release PRIVATE _CRT_SECURE_NO_WARNINGS)
# SafeHouse-Coop: ENet (MIT, Libraries/enet) for online co-op (coop.cpp)
set(sly2_enet_sources callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c)
list(TRANSFORM sly2_enet_sources PREPEND "${CANE_ROOT}/Libraries/enet/")
add_library(sly2_enet STATIC ${sly2_enet_sources})
target_include_directories(sly2_enet PUBLIC "${CANE_ROOT}/Libraries/enet/include")
if(WIN32)
  target_link_libraries(sly2_enet PUBLIC ws2_32 winmm)
else()
  target_compile_definitions(sly2_enet PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)
endif()
target_link_libraries(Sly2-Release PRIVATE sly2_enet)
# Development data: the checkout's build/ folder (the reference's RAM maps, the test memory card), used only by the
# developer options (--loadmaps, the development card's fallback save). A git worktree uses its main checkout's, so
# every working copy reads the same data. SLY2N_DEV_ROOT overrides it.
if(NOT SLY2N_DEV_ROOT)
  set(SLY2N_DEV_ROOT "${CANE_ROOT}")
  find_package(Git QUIET)
  if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-common-dir
                    WORKING_DIRECTORY "${CANE_ROOT}" OUTPUT_VARIABLE sly2n_git_common RESULT_VARIABLE sly2n_git_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(sly2n_git_result EQUAL 0 AND IS_DIRECTORY "${sly2n_git_common}")
      get_filename_component(SLY2N_DEV_ROOT "${sly2n_git_common}" DIRECTORY)
    endif()
  endif()
endif()
target_compile_definitions(Sly2-Release PRIVATE "SLY2N_DEV_ROOT=\"${SLY2N_DEV_ROOT}\"")
# The debug menu's title line (debugmenu.cpp): the top-level project version (CMakeLists.txt's project(ProjectCane
# VERSION ...)) and the checkout's short git hash, regenerated at every build (cmake/sly2-version.cmake).
file(STRINGS "${CANE_ROOT}/CMakeLists.txt" sly2n_project_line REGEX "^project\\(ProjectCane VERSION")
string(REGEX MATCH "VERSION ([0-9.]+)" sly2n_version_match "${sly2n_project_line}")
set(SLY2N_PORT_VERSION "${CMAKE_MATCH_1}")
if(NOT SLY2N_PORT_VERSION)
  set(SLY2N_PORT_VERSION "0.0.0")
endif()
# (a hash from outside the checkout, e.g. the packaging container's: -DCANE_GIT_HASH=<hash>, or the environment
# variable CANE_GIT_HASH at build time; empty: git)
set(CANE_GIT_HASH "" CACHE STRING "The git hash for the Sly 2 port's version string (empty: ask git)")
add_custom_target(sly2_version
  COMMAND ${CMAKE_COMMAND} -DROOT=${CANE_ROOT} -DVERSION=${SLY2N_PORT_VERSION} -DHASH=${CANE_GIT_HASH}
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sly2_version.h -P ${CANE_ROOT}/cmake/sly2-version.cmake
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2_version.h" VERBATIM)
add_dependencies(Sly2-Release sly2_version)
if(NOT MSVC)
  target_compile_options(Sly2-Release PRIVATE -fno-strict-aliasing -Wno-invalid-offsetof -ffp-contract=off)
endif()

# Renderer: GLFW window + OpenGL 4.3+ (glad from Sly1-Release), shaders embedded like Sly1-Release's.
if(NOT TARGET glfw)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  if(NOT FETCHCONTENT_SOURCE_DIR_GLFW AND EXISTS "$ENV{HOME}/Documents/tools/projectcane-deps/glfw-3.4/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_GLFW "$ENV{HOME}/Documents/tools/projectcane-deps/glfw-3.4")
  endif()
  FetchContent_Declare(glfw URL https://codeload.github.com/glfw/glfw/tar.gz/refs/tags/3.4 URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01)
  FetchContent_MakeAvailable(glfw)
endif()
target_sources(Sly2-Release PRIVATE "${CANE_ROOT}/Sly1-Release/glad.c")
# miniz (MIT; Libraries/miniz): inflate for the PS3 HD import's .zip sources (platform/ps3disc.h, ps3hd.cpp)
target_sources(Sly2-Release PRIVATE "${CANE_ROOT}/Libraries/miniz/miniz.c")
target_include_directories(Sly2-Release PRIVATE "${CANE_ROOT}/Libraries/miniz")
target_link_libraries(Sly2-Release PRIVATE glfw ${CMAKE_DL_LIBS})
target_compile_definitions(Sly2-Release PRIVATE GLFW_INCLUDE_NONE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB sly2n_shaders CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/*.frag" "${CMAKE_CURRENT_SOURCE_DIR}/*.geom")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h -P ${CANE_ROOT}/cmake/embed-shaders.cmake
  DEPENDS ${sly2n_shaders} ${CANE_ROOT}/cmake/embed-shaders.cmake VERBATIM)
target_sources(Sly2-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h")
# QoL (qolview.cpp): the launcher's high-quality Sly 2 logo, embedded (it replaces the front end's logo texture)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CANE_ROOT}/launcher/assets/games/sly2-logo.png -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h -DSYMBOL=sly2n_logo_png -P ${CANE_ROOT}/cmake/embed-image.cmake
  DEPENDS ${CANE_ROOT}/launcher/assets/games/sly2-logo.png ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
target_sources(Sly2-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h")

# Sound and movies (sound.cpp, snd989.cpp, spiop.cpp, fmv.cpp): FAudio (XAudio2 on SDL2, as Sly1-Release on Linux),
# FFmpeg's MPEG-2 decoder for the PSS movies, SDL2 for the stand-alone movie window (--play-movie).
if(WIN32)
  if(NOT CANE_FFMPEG_ROOT)
    set(CANE_FFMPEG_ROOT "${CANE_ROOT}/Libraries/ffmpeg/ffmpeg-master-latest-win64-lgpl-shared")
  endif()
  target_include_directories(Sly2-Release PRIVATE "${CANE_FFMPEG_ROOT}/include")
  target_link_directories(Sly2-Release PRIVATE "${CANE_FFMPEG_ROOT}/lib")
  target_link_libraries(Sly2-Release PRIVATE avcodec avutil swscale winmm)   # (winmm: platform/frame_pacer.h's timeBeginPeriod)
else()
  include("${CANE_ROOT}/cmake/ffmpeg.cmake")
  cane_link_ffmpeg(Sly2-Release avcodec avutil swscale)
endif()
if(APPLE)
  target_link_libraries(Sly2-Release PRIVATE cane_gl_metal)
endif()
if(NOT TARGET SDL2::SDL2)
  find_package(SDL2 2.24 CONFIG QUIET)
endif()
if(NOT TARGET FAudio-shared AND NOT TARGET FAudio-static)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  # Offline: -DFETCHCONTENT_SOURCE_DIR_FAUDIO=<FAudio-25.02 sources> (the dev machine's copy is found by itself).
  if(NOT FETCHCONTENT_SOURCE_DIR_FAUDIO AND EXISTS "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_FAUDIO "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02")
  endif()
  FetchContent_Declare(faudio URL https://codeload.github.com/FNA-XNA/FAudio/tar.gz/refs/tags/25.02
                       URL_HASH SHA256=eab667d231e5036b0e5c3c6f89fb4ea792284493d46161bc056a88a6dadc2683)
  FetchContent_MakeAvailable(faudio)
endif()
if(TARGET FAudio-shared)
  target_link_libraries(Sly2-Release PRIVATE FAudio-shared)
else()
  target_link_libraries(Sly2-Release PRIVATE FAudio-static)
endif()
if(TARGET SDL2::SDL2)
  target_link_libraries(Sly2-Release PRIVATE SDL2::SDL2)
else()
  target_include_directories(Sly2-Release PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(Sly2-Release PRIVATE ${SDL2_LIBRARIES})
endif()
# Controller rumble (rumble.cpp, platform/pad_rumble.h): SDL drives the motors only; input stays on GLFW (as Sly1-Release).
target_compile_definitions(Sly2-Release PRIVATE CANE_SDL_RUMBLE)
find_package(Threads REQUIRED)
target_link_libraries(Sly2-Release PRIVATE Threads::Threads)

# Controller prompt atlases (Kenney Input Prompts 1.5A, CC0: licenses/kenney-input-prompts) and SDL_GameControllerDB,
# embedded as cmake/prompts.cmake does for the launcher (platform/button_prompts.h, gamepad_mappings.h): the dynamic
# button icons (promptglyph.cpp) and the bundled pad mappings.
set(sly2n_prompt_source "${CANE_ROOT}/assets/prompts")
set(sly2n_gamecontrollerdb "${CANE_ROOT}/Libraries/gamecontrollerdb/gamecontrollerdb.txt")
if(EXISTS "${sly2n_prompt_source}/prompt_atlas_rects.inc" AND EXISTS "${sly2n_gamecontrollerdb}")
  set(sly2n_prompt_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts")
  file(MAKE_DIRECTORY "${sly2n_prompt_generated}")
  set(sly2n_prompt_headers "")
  foreach(family keyboard playstation xbox nintendo steamdeck steamcontroller ps4 ps5 steamcontroller2026 devices)
    set(image "${sly2n_prompt_source}/prompts_${family}.png")
    set(header "${sly2n_prompt_generated}/prompts_${family}_png.h")
    add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image} -DOUTPUT=${header} -DSYMBOL=cane_prompts_${family}
      -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${image}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
    list(APPEND sly2n_prompt_headers "${header}")
  endforeach()
  set(header "${sly2n_prompt_generated}/gamecontrollerdb_txt.h")
  add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${sly2n_gamecontrollerdb} -DOUTPUT=${header} -DSYMBOL=cane_gamecontrollerdb
    -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${sly2n_gamecontrollerdb}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
  list(APPEND sly2n_prompt_headers "${header}")
  target_sources(Sly2-Release PRIVATE "${sly2n_prompt_source}/prompt_data.cpp" ${sly2n_prompt_headers})
  target_include_directories(Sly2-Release PRIVATE "${sly2n_prompt_source}" "${sly2n_prompt_generated}")
  target_compile_definitions(Sly2-Release PRIVATE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
endif()
