cmake_minimum_required(VERSION 3.21)
project(figo-convert LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The figo core library (parse -> layout -> ThorVG raster) comes from the
# sibling figo repo — the converters bake sprites via Renderer::renderOverlay,
# pixel-identical to the runtime. Override with -DFIGO_ROOT=... if figo lives
# elsewhere.
set(FIGO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../figo" CACHE PATH
    "Path to the sibling figo repo (provides the figo core library)")
if(NOT EXISTS "${FIGO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "figo repo not found at ${FIGO_ROOT}. "
        "Clone https://github.com/nowasm/figo next to this repo, "
        "or pass -DFIGO_ROOT=<path>.")
endif()

# ThorVG was built /MD (meson release default); figo's CMakeLists sets this
# for its own directory scope only, so repeat it here for the converter exes.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# Core-only figo build: no raylib / quickjs / examples — with these OFF the
# figo subdirectory contributes just the `figo` static library (seconds to
# build). ThorVG / fig2json paths are figo cache variables and can be
# overridden from here (-DTHORVG_LIBRARY=... -DTHORVG_INCLUDE_DIR=...).
set(FIGO_BACKEND_RAYLIB OFF CACHE BOOL "" FORCE)
set(FIGO_SCRIPTING OFF CACHE BOOL "" FORCE)
set(FIGO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${FIGO_ROOT}" figo_core)

# ---- the converters: design (.fig / canvas.json / REST) -> engine assets ----
# figo2godot -> Godot 4 project (.tscn + sprites)
# figo2cocos -> Cocos Creator 3.x prefabs (.prefab + textures + .meta)
# figo2unity -> Unity UGUI prefabs (.prefab YAML + textures + .meta)
foreach(tool figo2godot figo2cocos figo2unity)
    add_executable(${tool} apps/${tool}/main.cpp)
    target_link_libraries(${tool} PRIVATE figo)
    target_include_directories(${tool} PRIVATE "${FIGO_ROOT}/third_party")  # nlohmann
endforeach()
