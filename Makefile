CC=gcc
CFLAGS=-D_GNU_SOURCE -Wall -Wextra -Wpedantic -std=gnu11 -O2 -Icommon
PREFIX=/usr/local

SCHED_OBJS=src/tieredvol_partition.o src/tieredvol_umapper.o \
           src/tieredvol_umeta.o src/tieredvol_benchmark.o src/tieredvol_warmup.o

SETUP_OBJS=src/tieredvol_exec.o src/tieredvol_discover.o src/tieredvol_bench.o src/cmd_create.o src/cmd_remove.o src/cmd_status.o src/cmd_scheduler.o

all: tiered_setup

tiered_setup: src/main.c src/tieredvol_common.h src/tieredvol_types.h src/version.h src/tieredvol_discover.h src/tieredvol_bench.h src/tieredvol_exec.h src/cmd_create.h src/cmd_remove.h $(SCHED_OBJS) $(SETUP_OBJS)
	$(CC) $(CFLAGS) -o $@ src/main.c $(SCHED_OBJS) $(SETUP_OBJS) -lm

src/tieredvol_partition.o: src/tieredvol_partition.c src/tieredvol_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_umapper.o: src/tieredvol_umapper.c src/tieredvol_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_umeta.o: src/tieredvol_umeta.c src/tieredvol_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_benchmark.o: src/tieredvol_benchmark.c src/tieredvol_types.h src/tieredvol_warmup.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_warmup.o: src/tieredvol_warmup.c src/tieredvol_warmup.h src/tieredvol_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_exec.o: src/tieredvol_exec.c src/tieredvol_exec.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_discover.o: src/tieredvol_discover.c src/tieredvol_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/tieredvol_bench.o: src/tieredvol_bench.c src/tieredvol_bench.h src/tieredvol_discover.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_create.o: src/cmd_create.c src/cmd_create.h src/tieredvol_types.h src/tieredvol_discover.h src/tieredvol_bench.h src/tieredvol_exec.h src/version.h src/tieredvol_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_remove.o: src/cmd_remove.c src/cmd_remove.h src/cmd_create.h src/tieredvol_types.h src/tieredvol_discover.h src/tieredvol_exec.h src/tieredvol_common.h src/cmd_status.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_status.o: src/cmd_status.c src/cmd_status.h src/tieredvol_types.h src/tieredvol_exec.h src/tieredvol_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/cmd_scheduler.o: src/cmd_scheduler.c src/cmd_scheduler.h src/cmd_create.h src/tieredvol_types.h src/tieredvol_discover.h src/tieredvol_bench.h src/tieredvol_exec.h src/version.h src/tieredvol_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests (no io_uring dependency — pure logic)
tests/test_common: tests/test_common.c src/tieredvol_common.h
	$(CC) $(CFLAGS) -o $@ $<

tests/test_mapper: tests/test_mapper.c src/tieredvol_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

tests/test_partition: tests/test_partition.c src/tieredvol_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

tests/test_metadata: tests/test_metadata.c src/tieredvol_types.h $(SCHED_OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(SCHED_OBJS)

test: tests/test_common tests/test_mapper tests/test_partition tests/test_metadata
	@echo "=== test_common ===" && ./tests/test_common && \
	echo "=== test_mapper ===" && ./tests/test_mapper && \
	echo "=== test_partition ===" && ./tests/test_partition && \
	echo "=== test_metadata ===" && ./tests/test_metadata

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
	rm -f tiered_setup tests/test_common tests/test_mapper tests/test_partition tests/test_metadata
	rm -f src/*.o

.PHONY: all install uninstall clean test test-full module module_install module_clean
