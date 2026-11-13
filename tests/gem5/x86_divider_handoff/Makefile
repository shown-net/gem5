# Build only; run under a full-system KVM-to-O3 switch harness.
CC ?= cc
OUT ?= ../../../build/x86-divider-handoff

.PHONY: all
all: $(OUT)/divider-handoff

$(OUT)/divider-handoff: probe.c
	mkdir -p $(OUT)
	$(CC) -std=gnu11 -O2 -static -fno-pie -no-pie -Wall -Wextra $< -o $@
