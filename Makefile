# Build for this machine (Linux):   make
# Build for QNX 8.0 x86_64 (QEMU):  source ~/qnx800/qnxsdp-env.sh && make TARGET=qnx
# Build for QNX 8.0 on Pi (arm64):  make TARGET=qnx QNX_ARCH=aarch64le
# Run host unit tests:              make test

TARGET   ?= host
QNX_ARCH ?= x86_64

ifeq ($(TARGET),qnx)
  CC      = qcc
  ifeq ($(QNX_ARCH),aarch64le)
    CCVER = -Vgcc_ntoaarch64le
  else
    CCVER = -Vgcc_ntox86_64
  endif
  CFLAGS  = $(CCVER) -std=gnu11 -O2 -Wall -Wextra -Iinclude
  LDLIBS  = -lm
  BUILD   = build/qnx-$(QNX_ARCH)
else
  CC     ?= cc
  CFLAGS  = -std=gnu11 -O2 -Wall -Wextra -Iinclude
  LDLIBS  = -lpthread -lrt -lm
  BUILD   = build/host
endif

COMMON = src/common.c src/actuator.c src/control.c src/vehicle.c
BINS   = bbw_plant bbw_v1 bbw_v2 bbw_comparer bbw_hog bbw_sweep bbw_guard bbw_runas
OUT    = $(addprefix $(BUILD)/,$(BINS))

all: $(OUT)

$(BUILD)/bbw_plant:    src/plant.c    $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/plant.c $(COMMON) $(LDLIBS)
$(BUILD)/bbw_v1:       src/v1.c       $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/v1.c $(COMMON) $(LDLIBS)
$(BUILD)/bbw_v2:       src/v2.c       $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/v2.c $(COMMON) $(LDLIBS)
$(BUILD)/bbw_comparer: src/comparer.c $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/comparer.c $(COMMON) $(LDLIBS)
$(BUILD)/bbw_hog:      src/hog.c      $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/hog.c $(COMMON) $(LDLIBS)

$(BUILD)/bbw_guard:    src/guard.c    $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/guard.c $(COMMON) $(LDLIBS)

$(BUILD)/bbw_runas:    src/runas.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/runas.c

$(BUILD)/bbw_sweep:    src/sweep.c    $(COMMON) include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/sweep.c $(COMMON) $(LDLIBS)

$(BUILD):
	mkdir -p $@

test: build/host/test_ring build/host/test_control
	build/host/test_ring && build/host/test_control && python3 tests/test_gate.py

build/host/test_ring: tests/test_ring.c include/bbw.h
	mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Iinclude -o $@ tests/test_ring.c -lpthread
build/host/test_control: tests/test_control.c src/control.c include/*.h
	mkdir -p build/host
	cc -std=gnu11 -O2 -Wall -Wextra -Iinclude -o $@ tests/test_control.c src/control.c -lm

# Copy everything the target needs into one folder for scp/sftp
bundle: all
	mkdir -p $(BUILD)/bundle/data
	cp $(OUT) $(BUILD)/bundle/
	cp scripts/run_demo.sh $(BUILD)/bundle/
	cp data/*.csv $(BUILD)/bundle/data/ 2>/dev/null || true
	@echo "Bundle ready in $(BUILD)/bundle"

clean:
	rm -rf build

.PHONY: all test clean bundle
