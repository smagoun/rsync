# The rsync application with windows: bookmarks, editor, console, server.
add_application(rsyncgui
    app.c bookmarks.c console.c editor.c glue.c ../rsync/engine.c gui.r
    CREATOR "rSnc")
target_include_directories(rsyncgui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../rsync)
target_link_libraries(rsyncgui openrsync macposix)
set_target_properties(rsyncgui PROPERTIES LINKER_LANGUAGE CXX)
target_link_options(rsyncgui PRIVATE "-Wl,--wrap=abort" "-Wl,--wrap=_exit")
