cmake_minimum_required(VERSION 3.20)

# Static CRT everywhere, including SokuLib (its own cmake_minimum_required would otherwise
# leave CMP0091 at OLD for that subdirectory and mix /MD into a /MT link).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(PaletteShare VERSION 1.1.0 LANGUAGES CXX ASM_MASM RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "PaletteShare must be built as 32-bit (th123 is an x86 process).")
endif()

# EXCLUDE_FROM_ALL: only build the SokuLib static library we link, not its test targets.
add_subdirectory(thirdparty/SokuLib EXCLUDE_FROM_ALL)

add_library(PaletteShare MODULE
	src/main.cpp
	src/log.hpp
	src/log.cpp
	src/palette.hpp
	src/palette.cpp
	src/palette_game.cpp
	src/palettefile.hpp
	src/palettefile.cpp
	src/hooks.hpp
	src/hooks.cpp
	src/net.hpp
	src/net.cpp
	src/protocol.hpp
	src/protocol.cpp
	src/library.hpp
	src/library.cpp
	src/backup.hpp
	src/backup.cpp
	src/picker.hpp
	src/picker.cpp
	src/ui.hpp
	src/ui.cpp
	src/menu.hpp
	src/menu.cpp
	src/replay.hpp
	src/replay.cpp
	src/replaymeta.hpp
	src/replaymeta.cpp
	src/patch.hpp
	src/patch.cpp
	src/game.hpp
	src/game.cpp
	src/guard.hpp
	src/sha256.hpp
	src/sha256.cpp
	src/win7.cpp
	src/win7.asm
	src/base64.hpp
	src/base64.cpp
	src/metaformat.hpp
	src/metaformat.cpp
	src/original.hpp
	src/original.cpp
	src/saved.hpp
	src/saved.cpp
	src/iniupgrade.hpp
	src/iniupgrade.cpp
	src/sitereport.hpp
	src/sitereport.cpp
	src/PaletteShare.rc
)
target_compile_definitions(PaletteShare PRIVATE
	NOMINMAX _CRT_SECURE_NO_WARNINGS
	_WIN32_WINNT=0x0601 WINVER=0x0601  # Windows 7: no newer Windows API by accident # no WIN32_LEAN_AND_MEAN: SokuLib's headers need winsock from <windows.h>
	PALETTESHARE_VERSION="${PROJECT_VERSION}")
target_include_directories(PaletteShare PRIVATE thirdparty)  # nlohmann/json.hpp (3.9.1, MIT)
# The shipped ini, embedded for the ini upgrade (src/PaletteShare.rc finds it through the project root).
set_property(SOURCE src/PaletteShare.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/PaletteShare.ini")
set_property(SOURCE src/PaletteShare.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
file(STRINGS "${CMAKE_SOURCE_DIR}/PaletteShare.ini" _ps_ini_version REGEX "^IniVersion=")
if(NOT _ps_ini_version STREQUAL "IniVersion=${PROJECT_VERSION}")
	message(WARNING "PaletteShare.ini has '${_ps_ini_version}', expected 'IniVersion=${PROJECT_VERSION}'")
endif()
target_compile_options(PaletteShare PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /utf-8>)
target_link_libraries(PaletteShare PRIVATE SokuLib shlwapi ws2_32 shell32 winhttp)
set_target_properties(PaletteShare PROPERTIES PREFIX "")

# Tests for the parts that don't need the game (tools\test.bat builds and runs them).
add_executable(PaletteShareTests EXCLUDE_FROM_ALL
	tests/tests.cpp
	src/sha256.cpp
	src/base64.cpp
	src/metaformat.cpp
	src/protocol.cpp
	src/palette.cpp
	src/palettefile.cpp
	src/log.cpp
	src/backup.cpp
	src/saved.cpp
	src/iniupgrade.cpp
)
target_include_directories(PaletteShareTests PRIVATE thirdparty)
target_compile_definitions(PaletteShareTests PRIVATE
	NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601 WINVER=0x0601
	PALETTESHARE_VERSION="${PROJECT_VERSION}")
target_compile_options(PaletteShareTests PRIVATE /W3 /utf-8)
