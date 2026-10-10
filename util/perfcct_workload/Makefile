CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
OBJDUMP := $(CROSS_COMPILE)objdump
LDFLAGS := -march=rv64im -mabi=lp64 -nostdlib -static -no-pie \
	-Wl,--build-id=none -Wl,-e,_start
PROBES := causal pointer-cold pointer-warm stlf-short stlf-long group \
	resource-lines resource-targets scheduling-serial scheduling-independent

.PHONY: all paired clean disasm
all: $(PROBES)
paired: pointer-cold pointer-warm stlf-short stlf-long group

causal: causal.S Makefile
	$(CC) $(LDFLAGS) -o $@ $<

pointer-cold: pointer.S Makefile
	$(CC) $(LDFLAGS) -DWARM=0 -o $@ $<

pointer-warm: pointer.S Makefile
	$(CC) $(LDFLAGS) -DWARM=1 -o $@ $<

stlf-short: stlf.S Makefile
	$(CC) $(LDFLAGS) -DDEPTH=2 -DEXPECTED=0x14835a6279b1581 -o $@ $<

stlf-long: stlf.S Makefile
	$(CC) $(LDFLAGS) -DDEPTH=12 -DEXPECTED=0x14c14f1863d14ba -o $@ $<

group: group.S Makefile
	$(CC) $(LDFLAGS) -o $@ $<

resource-lines: resource.S Makefile
	$(CC) $(LDFLAGS) -DSTRIDE=64 -o $@ $<

resource-targets: resource.S Makefile
	$(CC) $(LDFLAGS) -DSTRIDE=8 -o $@ $<

scheduling-serial: scheduling.S Makefile
	$(CC) $(LDFLAGS) -DSERIAL=1 -o $@ $<

scheduling-independent: scheduling.S Makefile
	$(CC) $(LDFLAGS) -DSERIAL=0 -o $@ $<

disasm: causal
	$(OBJDUMP) -d $<

clean:
	rm -f $(PROBES) request-identity

HOST_CXX ?= c++
.PHONY: check-request-identity
request-identity: request_identity.cc ../../src/mem/request.hh
	$(HOST_CXX) -std=c++17 -I../../src -I../../build/RISCV $< -o $@

check-request-identity: request-identity
	./request-identity
