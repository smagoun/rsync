# Self-test of the macposix layer, run inside Mac OS (see classic/emu).
add_application(MPTest mptest.c CONSOLE)
target_link_libraries(MPTest macposix)
set_target_properties(MPTest PROPERTIES LINKER_LANGUAGE CXX)
