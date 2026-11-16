cmake_minimum_required(VERSION 3.22)
set(PLATFORM_SWITCH ON CACHE BOOL "Build Nintendo Switch components")
set(USE_DEKO3D ON CACHE BOOL "Use deko3d graphics")
set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/borealis/library)
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)
project(SwitchMusic VERSION 0.1.0 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_path(GLM_INCLUDE_DIR glm/vec2.hpp REQUIRED)
include_directories(${GLM_INCLUDE_DIR})
add_subdirectory(third_party/borealis/library)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
include(${CMAKE_SOURCE_DIR}/toolchain/AudioFFmpeg.cmake)
add_library(switchmusic_common STATIC common/src/client.cpp)
target_include_directories(switchmusic_common PUBLIC common/include third_party/json)
target_link_libraries(switchmusic_common PUBLIC PkgConfig::CURL ${CURL_STATIC_LIBRARIES} nx)
add_executable(SwitchMusic nro/source/main.cpp third_party/borealis/library/lib/platforms/switch/switch_wrapper.c)
target_link_libraries(SwitchMusic PRIVATE switchmusic_common borealis)
file(COPY ${CMAKE_SOURCE_DIR}/third_party/borealis/resources/ DESTINATION ${CMAKE_BINARY_DIR}/romfs)
file(GLOB_RECURSE SWITCHMUSIC_UI_XML CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/nro/resources/xml/*.xml)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/switchmusic-ui.stamp
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/nro/resources ${CMAKE_BINARY_DIR}/romfs
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/switchmusic-ui.stamp
    DEPENDS ${SWITCHMUSIC_UI_XML})
add_custom_target(nro-ui DEPENDS ${CMAKE_BINARY_DIR}/switchmusic-ui.stamp)
add_dependencies(SwitchMusic nro-ui)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/romfs/certs)
configure_file(resources/certs/cacert.pem ${CMAKE_BINARY_DIR}/romfs/certs/cacert.pem COPYONLY)
configure_file(resources/certs/LICENSE.txt ${CMAKE_BINARY_DIR}/romfs/certs/LICENSE.txt COPYONLY)
gen_dksh(${CMAKE_BINARY_DIR}/romfs/shaders)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/SwitchMusic.nro
    COMMAND ${NX_NACPTOOL_EXE} --create "SwitchMusic" "SwitchMusic contributors" "0.1.0" ${CMAKE_BINARY_DIR}/SwitchMusic.nacp
    COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:SwitchMusic> ${CMAKE_BINARY_DIR}/SwitchMusic.nro --nacp=${CMAKE_BINARY_DIR}/SwitchMusic.nacp --romfsdir=${CMAKE_BINARY_DIR}/romfs
    DEPENDS SwitchMusic ${CMAKE_BINARY_DIR}/switchmusic-ui.stamp ${CMAKE_BINARY_DIR}/romfs/certs/cacert.pem ${CMAKE_BINARY_DIR}/romfs/certs/LICENSE.txt)
add_custom_target(nro ALL DEPENDS ${CMAKE_BINARY_DIR}/SwitchMusic.nro)
find_program(NX_BUILD_ROMFS_EXE build_romfs HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/switchmusic-sys.romfs
    COMMAND ${NX_BUILD_ROMFS_EXE} ${CMAKE_SOURCE_DIR}/resources ${CMAKE_BINARY_DIR}/switchmusic-sys.romfs
    DEPENDS ${CMAKE_SOURCE_DIR}/resources/certs/cacert.pem ${CMAKE_SOURCE_DIR}/resources/certs/LICENSE.txt ${CMAKE_SOURCE_DIR}/resources/certs/README.md)
add_custom_target(sysmodule-romfs ALL DEPENDS ${CMAKE_BINARY_DIR}/switchmusic-sys.romfs)
set(SWITCHMUSIC_HEAP_MB 8 CACHE STRING "Sysmodule heap in MiB")
add_executable(switchmusic-sys sysmodule/source/main.cpp sysmodule/source/http_input.cpp sysmodule/source/audio_decoder.cpp)
target_include_directories(switchmusic-sys PRIVATE sysmodule/include)
target_compile_definitions(switchmusic-sys PRIVATE SWITCHMUSIC_HEAP_MB=${SWITCHMUSIC_HEAP_MB})
target_link_libraries(switchmusic-sys PRIVATE switchmusic_common SwitchMusic::AudioFFmpeg)
target_link_options(switchmusic-sys PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/switchmusic-sys.map")
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/switchmusic-sys.nsp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/exefs
    COMMAND ${NX_ELF2NSO_EXE} $<TARGET_FILE:switchmusic-sys> ${CMAKE_BINARY_DIR}/exefs/main
    COMMAND ${NX_NPDMTOOL_EXE} ${CMAKE_SOURCE_DIR}/sysmodule/sysmodule.json ${CMAKE_BINARY_DIR}/exefs/main.npdm
    COMMAND ${NX_BUILD_PFS0_EXE} ${CMAKE_BINARY_DIR}/exefs ${CMAKE_BINARY_DIR}/switchmusic-sys.nsp
    DEPENDS switchmusic-sys ${CMAKE_SOURCE_DIR}/sysmodule/sysmodule.json)
add_custom_target(sysmodule ALL DEPENDS ${CMAKE_BINARY_DIR}/switchmusic-sys.nsp)
add_dependencies(sysmodule sysmodule-romfs)
file(GLOB ULTRA_SRC CONFIGURE_DEPENDS third_party/libultrahand/common/*.cpp third_party/libultrahand/common/*.c third_party/libultrahand/libultra/source/*.cpp third_party/libultrahand/libtesla/source/*.cpp)
add_executable(switchmusic-overlay overlay/source/main.cpp ${ULTRA_SRC})
set_target_properties(switchmusic-overlay PROPERTIES CXX_STANDARD 26)
target_include_directories(switchmusic-overlay PRIVATE third_party/libultrahand/common third_party/libultrahand/libultra/include third_party/libultrahand/libtesla/include)
target_compile_definitions(switchmusic-overlay PRIVATE UI_OVERRIDE_PATH="/config/switchmusic/" APP_VERSION="0.1.0")
target_link_libraries(switchmusic-overlay PRIVATE switchmusic_common minizip z nx)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/switchmusic.ovl
    COMMAND ${NX_NACPTOOL_EXE} --create "SwitchMusic" "SwitchMusic contributors" "0.1.0" ${CMAKE_BINARY_DIR}/switchmusic-overlay.nacp
    COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:switchmusic-overlay> ${CMAKE_BINARY_DIR}/switchmusic.ovl --nacp=${CMAKE_BINARY_DIR}/switchmusic-overlay.nacp
    COMMAND ${CMAKE_COMMAND} -DFILE=${CMAKE_BINARY_DIR}/switchmusic.ovl -P ${CMAKE_SOURCE_DIR}/scripts/overlay-signature.cmake
    DEPENDS switchmusic-overlay)
add_custom_target(overlay ALL DEPENDS ${CMAKE_BINARY_DIR}/switchmusic.ovl)
