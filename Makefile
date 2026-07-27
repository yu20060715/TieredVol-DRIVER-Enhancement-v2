CC=gcc
CFLAGS=-D_GNU_SOURCE -Wall -Wextra -Wpedantic -std=gnu11 -O2
PREFIX=/usr/local

SCHED_OBJS=src/tiered_partition.o src/tiered_mapper.o \
           src/tiered_metadata.o src/tiered_benchmark.o src/warmup.o

SETUP_OBJS=src/exec_helper.o src/setup_discover.o src/setup_bench.o src/cmd_create.o src/cmd_remove.o src/cmd_status.o src/cmd_scheduler.o

all: tiered_setup

tiered_setup: src/main.c src/tiered_common.h src/tiered_types.h src/version.h src/setup_discover.h src/setup_bench.h src/exec_helper.h src/cmd_create.h src/cmd_remove.h $(SCHED_OBJS) $(SETUP_OBJS)
	$(CC) $(CFLAGS) -o $@ src/main.c $(SCHED_OBJS) $(SETUP_OBJS) -lm

src/tiered_partition.o: src/tiered_partition.c src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_mapper.o: src/tiered_mapper.c src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_metadata.o: src/tiered_metadata.c src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tiered_benchmark.o: src/tiered_benchmark.c src/tiered_types.h src/warmup.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/warmup.o: src/warmup.c src/warmup.h src/tiered_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/exec_helper.o: src/exec_helper.c src/exec_helper.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/setup_discover.o: src/setup_discover.c src/setup_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/setup_bench.o: src/setup_bench.c src/setup_bench.h src/setup_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_create.o: src/cmd_create.c src/cmd_create.h src/tiered_types.h src/setup_discover.h src/setup_bench.h src/exec_helper.h src/version.h src/tiered_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_remove.o: src/cmd_remove.c src/cmd_remove.h src/cmd_create.h src/tiered_types.h src/setup_discover.h src/exec_helper.h src/tiered_common.h src/cmd_status.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_status.o: src/cmd_status.c src/cmd_status.h src/tiered_types.h src/exec_helper.h src/tiered_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_scheduler.o: src/cmd_scheduler.c src/cmd_scheduler.h src/cmd_create.h src/tiered_types.h src/setup_discover.h src/setup_bench.h src/exec_helper.h src/version.h src/tiered_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests (no io_uring dependency — pure logic)
test_common: tests/test_common.c src/tiered_common.h
	$(CC) $(CFLAGS) -o $@ $<

test_mapper: tests/test_mapper.c src/tiered_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test_partition: tests/test_partition.c src/tiered_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test_metadata: tests/test_metadata.c src/tiered_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test: test_common test_mapper test_partition test_metadata
	@echo "=== test_common ===" && ./test_common && \
	echo "=== test_mapper ===" && ./test_mapper && \
	echo "=== test_partition ===" && ./test_partition && \
	echo "=== test_metadata ===" && ./test_metadata

test-full: test
	@echo "=== Integration tests (DM messages) ==="
	@echo "  Requires: sudo + running dm target. Run:"
	@echo "  sudo ./tests/test_dm_messages.sh <volume_name>"

# Kernel module targets
module:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules

module_install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules_install
	depmod -a

module_clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver clean

install: all
	install -m 755 tiered_setup $(DESTDIR)$(PREFIX)/bin/tiered_setup
	mkdir -p $(DESTDIR)/etc/tieredvol
	mkdir -p $(DESTDIR)/etc/systemd/system
	@echo ""
	@echo "Installed:"
	@echo "  $(DESTDIR)$(PREFIX)/bin/tiered_setup"
	@echo ""

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tiered_setup

clean:
	rm -f tiered_setup test_common test_mapper test_partition test_metadata
	rm -f src/*.o

.PHONY: all install uninstall clean test test-full module module_install module_clean
