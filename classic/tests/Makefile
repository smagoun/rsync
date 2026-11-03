# Host-side tests for the Classic Mac OS port.  Run: make -C classic/tests
OPENRSYNC_DIR = ../../openrsync
OPENRSYNC ?= $(OPENRSYNC_DIR)/openrsync
CFLAGS += -Wall -Wextra -I$(OPENRSYNC_DIR)

all: check

hfsname_test: hfsname_test.c $(OPENRSYNC_DIR)/hfsname.c $(OPENRSYNC_DIR)/hfsname.h
	$(CC) $(CFLAGS) -o $@ hfsname_test.c $(OPENRSYNC_DIR)/hfsname.c

printf_test: printf_test.c ../macposix/printf.c
	$(CC) $(CFLAGS) -DMP_PRINTF_HOSTTEST -o $@ printf_test.c ../macposix/printf.c

check: hfsname_test printf_test
	./hfsname_test
	./printf_test
	./netatalk_test.sh $(OPENRSYNC)

clean:
	rm -f hfsname_test printf_test

.PHONY: all check clean
