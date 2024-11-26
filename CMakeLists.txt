cmake_minimum_required(VERSION 3.5.0)

project(libmidimonster VERSION 0.1.0 DESCRIPTION "libmidimonster dependency" )
add_library(libmidimonster SHARED
    core/backend.c
    core/config.c
    core/core.c
    core/plugin.c
    core/routing.c
)
target_include_directories(libmidimonster PRIVATE .)
add_compile_options(-mno-ms-bitfields)



project(midimonster VERSION 0.1.0 LANGUAGES C)

add_executable(midimonster midimonster.c)
target_include_directories(libmidimonster PRIVATE .)

project(midimonster_gui VERSION 0.1.0 LANGUAGES C)
add_executable(midimonster_gui midimonster_gui.c)