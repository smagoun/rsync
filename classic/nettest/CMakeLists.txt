# Phase 0 network check: connect to an rsync daemon over MacTCP and log the exchange.


add_application(NetTest nettest.c CONSOLE)
target_include_directories(NetTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../macposix/include)
