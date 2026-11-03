# Host-side tests for the Classic Mac OS port.  Run: make -C classic/tests
OPENRSYNC_DIR = ../../openrsync
OPENRSYNC ?= $(OPENRSYNC_DIR)/openrsync
CFLAGS += -Wall -Wextra -I$(OPENRSYNC_DIR)

all: check

hfsname_test: hfsname_test.c $(OPENRSYNC_DIR)/hfsname.c $(OPENRSYNC_DIR)/hfsname.h
	$(CC) $(CFLAGS) -o $@ hfsname_test.c $(OPENRSYNC_DIR)/hfsname.c

check: hfsname_test
	./hfsname_test
	./netatalk_test.sh $(OPENRSYNC)

clean:
	rm -f hfsname_test

.PHONY: all check clean
