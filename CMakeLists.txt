cmake_minimum_required(VERSION 3.20)

# Static CRT everywhere, including SokuLib (its own cmake_minimum_required would otherwise
# leave CMP0091 at OLD for that subdirectory and mix /MD into a /MT link).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(PaletteShare VERSION 1.0.0 LANGUAGES CXX)

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
	src/hooks.hpp
	src/hooks.cpp
	src/net.hpp
	src/net.cpp
	src/protocol.hpp
	src/protocol.cpp
	src/library.hpp
	src/library.cpp
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
	src/original.hpp
	src/original.cpp
)
target_compile_definitions(PaletteShare PRIVATE
	NOMINMAX _CRT_SECURE_NO_WARNINGS # no WIN32_LEAN_AND_MEAN: SokuLib's headers need winsock from <windows.h>
	PALETTESHARE_VERSION="${PROJECT_VERSION}")
target_include_directories(PaletteShare PRIVATE thirdparty)  # nlohmann/json.hpp (3.9.1, MIT)
target_compile_options(PaletteShare PRIVATE /W3 /utf-8)
target_link_libraries(PaletteShare PRIVATE SokuLib bcrypt shlwapi ws2_32 shell32 ole32)
set_target_properties(PaletteShare PROPERTIES PREFIX "")
