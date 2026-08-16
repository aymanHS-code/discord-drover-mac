CC ?= clang
CFLAGS ?= -Wall -Wextra -O2
ARCHS ?= -arch arm64 -arch x86_64

.PHONY: all dylib test clean install

all: dylib udp_test

dylib: drover_direct.dylib

drover_direct.dylib: drover_direct.c
	$(CC) -dynamiclib -fPIC $(ARCHS) $(CFLAGS) \
		-mmacosx-version-min=11.0 \
		-install_name @rpath/drover_direct.dylib \
		-Wl,-no_data_const \
		-o $@ $<

udp_test: udp_test.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	DYLD_INSERT_LIBRARIES="$(CURDIR)/drover_direct.dylib" \
	DROVER_PACKET_PATH="$(CURDIR)/drover-packet.bin" \
	DROVER_DEBUG=1 \
	./udp_test

install: dylib
	./install.sh

clean:
	rm -f drover_direct.dylib udp_test
