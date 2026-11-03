# rsync for Classic Mac OS: openrsync + macposix + the console front end.
set(O ${OPENRSYNC_DIR})
set(OPENRSYNC_SOURCES
    ${O}/batch.c ${O}/blocks.c ${O}/cleanup.c ${O}/client.c ${O}/compats.c
    ${O}/copy.c ${O}/daemon.c ${O}/daemon_cfg.c ${O}/daemon_misc.c
    ${O}/downloader.c ${O}/fargs.c ${O}/flist.c ${O}/fmap.c ${O}/hardlinks.c
    ${O}/hash.c ${O}/ids.c ${O}/io.c ${O}/log.c ${O}/main.c ${O}/md4.c
    ${O}/misc.c ${O}/mkpath.c ${O}/mktemp.c ${O}/netatalk.c ${O}/platform.c
    ${O}/portable.c ${O}/receiver.c ${O}/rmatch.c ${O}/rules.c ${O}/sender.c
    ${O}/server.c ${O}/session.c ${O}/socket.c ${O}/strmode.c
    ${O}/symlinks.c ${O}/uploader.c
    ${O}/zlib/adler32.c ${O}/zlib/compress.c ${O}/zlib/crc32.c
    ${O}/zlib/deflate.c ${O}/zlib/inffast.c ${O}/zlib/inflate.c
    ${O}/zlib/inftrees.c ${O}/zlib/trees.c ${O}/zlib/zutil.c)

add_library(openrsync STATIC ${OPENRSYNC_SOURCES} register.c)
target_link_libraries(openrsync PUBLIC macposix)
# config.h for 68k lives here.  openrsync's own (macOS) config.h sits next
# to the sources and would be found first by #include "config.h"; forcing
# ours in first makes that one a no-op (same include guard).
target_include_directories(openrsync BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(openrsync PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/config.h")
# exit() returns to the prompt; main() is called by the front end.
target_compile_definitions(openrsync PRIVATE exit=rsync_exit)
set_source_files_properties(${O}/main.c PROPERTIES
    COMPILE_DEFINITIONS main=rsync_main)
target_compile_options(openrsync PRIVATE -w)

add_application(rsync front.c rsync.r CONSOLE CREATOR "rSnc")
target_link_libraries(rsync openrsync macposix)
target_link_options(rsync PRIVATE "-Wl,--wrap=_consolewrite"
    "-Wl,--wrap=abort" "-Wl,--wrap=_exit")
set_target_properties(rsync PROPERTIES LINKER_LANGUAGE CXX)
