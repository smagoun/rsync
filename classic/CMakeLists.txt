# Classic Mac OS (68k) build of the rsync port, using Retro68:
#
#   cmake -S classic -B build-classic \
#     -DCMAKE_TOOLCHAIN_FILE=<Retro68>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   make -C build-classic
cmake_minimum_required(VERSION 3.12)
project(RsyncClassic C CXX)

set(OPENRSYNC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../openrsync)

# The POSIX layer.  Every target that uses it gets macposix.h forced in.
add_library(macposix STATIC
    macposix/path.c macposix/fd.c macposix/ops.c macposix/dir.c
    macposix/meta.c macposix/net.c macposix/misc.c macposix/time.c
    macposix/printf.c
    ${OPENRSYNC_DIR}/hfsname.c)
target_include_directories(macposix PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/macposix/include
    ${CMAKE_CURRENT_SOURCE_DIR}/rsync
    ${OPENRSYNC_DIR})
target_compile_definitions(macposix PUBLIC __MACOS_CLASSIC__ _GNU_SOURCE)
target_compile_options(macposix PUBLIC -std=gnu99
    -Wno-pointer-sign -Wno-discarded-qualifiers)
# Users of the layer see POSIX names routed to it (macposix.h's macros).
target_compile_options(macposix INTERFACE "SHELL:-include macposix.h")
# newlib lacks C99/long long printf formats; use macposix/printf.c instead.
foreach(f printf vprintf fprintf vfprintf sprintf vsprintf snprintf vsnprintf
        asprintf vasprintf dprintf vdprintf)
    target_link_options(macposix INTERFACE "-Wl,--wrap=${f}")
endforeach()
# Retro68 stubs getentropy() (ENOSYS), which kills arc4random(); see misc.c.
target_link_options(macposix INTERFACE "-Wl,--wrap=_getentropy_r")

add_subdirectory(nettest)
add_subdirectory(mptest)
add_subdirectory(rsync)
