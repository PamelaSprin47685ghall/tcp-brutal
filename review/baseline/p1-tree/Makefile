KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
# Kernels built with clang (CONFIG_CC_IS_CLANG) need LLVM=1 for the module too
KERNEL_CONFIG   := $(firstword $(wildcard $(KERNEL_DIR)/include/config/auto.conf $(KERNEL_DIR)/.config))
KBUILD_LLVM     := $(if $(KERNEL_CONFIG),$(if $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KERNEL_CONFIG) && echo y),LLVM=1))
DKMS_TARBALL    ?= dkms.tar.gz
TAR             ?= tar
CLANG_FORMAT    ?= clang-format-18
SRCS            := Makefile \
                   tbtun/tbtun.h tbtun/tbt_uapi.h tbtun/tbt_parse.h tbtun/tbt_proto.h \
                   tbtun/tbt_rec.h tbtun/tbt_http.h \
                   tbtun/tbt_core.c tbtun/tbt_session.c tbtun/tbt_procfs.c tbtun/tbt_listen.c \
                   tbtun/tbt_carrier.c tbtun/tbt_sock.c tbtun/tbt_tx.c tbtun/tbt_rx.c \
                   tbtun/tbt_parse.c tbtun/tbt_proto.c tbtun/tbt_rec.c tbtun/tbt_http.c \
                   scripts/mkdkmsconf.sh scripts/install_dkms.sh \
                   .clang-format
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))

HOSTCC          ?= cc
HOST_CFLAGS     ?= -std=gnu99 -O2 -Wall -Wextra -Werror
HOST_TEST       := host/test_parse
HOST_TEST_SRCS  := host/test_parse.c tbtun/tbt_parse.c
HOST_TEST_HDRS  := tbtun/tbt_parse.h tbtun/tbt_uapi.h
HOST_FUZZ       := host/test_fuzz
HOST_FUZZ_SRCS  := host/test_fuzz.c tbtun/tbt_rec.c tbtun/tbt_proto.c
HOST_FUZZ_HDRS  := tbtun/tbt_rec.h tbtun/tbt_proto.h tbtun/tbt_uapi.h
HOST_HTTP       := host/test_http
HOST_HTTP_SRCS  := host/test_http.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_HDRS  := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
HOST_HTTP_H11   := host/test_http_h11.py
HOST_HTTP_FUZZ  := host/test_http_fuzz
HOST_HTTP_FUZZ_SRCS := host/test_http_fuzz.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_FUZZ_HDRS := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
HOST_HTTP_LIVE  := host/test_http_live
HOST_HTTP_LIVE_SRCS := host/test_http_live.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_LIVE_HDRS := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
SAN_CFLAGS      := -g -fsanitize=address,undefined -fno-omit-frame-pointer

obj-m           += tbtun.o
tbtun-objs      := tbtun/tbt_core.o tbtun/tbt_session.o tbtun/tbt_procfs.o \
                   tbtun/tbt_listen.o tbtun/tbt_proto.o tbtun/tbt_carrier.o \
                   tbtun/tbt_sock.o tbtun/tbt_tx.o tbtun/tbt_rx.o tbtun/tbt_rec.o \
                   tbtun/tbt_parse.o tbtun/tbt_http.o

ccflags-y       := -std=gnu99 -I$(src)/tbtun

.PHONY: all check clean load unload qemu-test host-test host-test-san host-test-san-run host-test-live

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $(KBUILD_LLVM) modules

# One command for a human: resolves the header payload, prints the make line it
# runs, then builds the module and runs all host tests (docs/build-env.md).
check:
	@if [ -x ./scripts/build.sh ]; then ./scripts/build.sh; else \
		$(MAKE) KERNEL_DIR=$(KERNEL_DIR) all; fi
	@$(MAKE) --no-print-directory host-test

# The host tests link the same parser sources the module uses, so they need no
# root, no module and no kernel headers: the grammar test (tbt_parse.c), the
# hostile-input test (tbt_rec.c + tbt_proto.c) and the HTTP container test
# (tbt_http.c + tbt_proto.c).
host-test: $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP)
	@./$(HOST_TEST)
	@./$(HOST_FUZZ)
	@./$(HOST_HTTP)
	@# Third-party cross-check: h11 is not a build dependency. uv with a cached
	@# h11 runs it offline; anything else gets a clear skip, not a failure.
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python $(HOST_HTTP_H11) ./$(HOST_HTTP); \
	else \
		echo "host-test: h11 cross-check skipped (uv or cached h11 unavailable)"; \
	fi

# Boot a kernel in QEMU and run the module there: the only way to exercise a
# kernel module without root on this host. The build goes through `check`'s
# script so the header payload is found the same way; the guest kernel comes
# from ~/.cache/tbtun-build (tests/qemu/README.md).
qemu-test:
	@if [ -x ./scripts/build.sh ]; then ./scripts/build.sh > /dev/null; else \
		$(MAKE) --no-print-directory all; fi
	@tests/qemu/run.sh --ko $(PWD)/tbtun.ko

$(HOST_TEST): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_TEST_SRCS)

$(HOST_FUZZ): $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FUZZ_SRCS)

$(HOST_HTTP): $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_SRCS)

# The same tests with ASan and UBSan. A toolchain without the runtimes gets
# a clear skip, not a build failure.
host-test-san:
	@if printf 'int main(void){return 0;}\n' | $(HOSTCC) $(SAN_CFLAGS) -x c - -o /dev/null 2>/dev/null; then \
		$(MAKE) --no-print-directory host-test-san-run; \
	else \
		echo "host-test-san: skipped, $(HOSTCC) cannot link -fsanitize=address,undefined"; \
	fi

host-test-san-run: host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san
	@./host/test_parse_san
	@./host/test_fuzz_san
	@./host/test_http_san
	@./host/test_http_fuzz_san

host/test_parse_san: $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_TEST_SRCS)

host/test_fuzz_san: $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FUZZ_SRCS)

host/test_http_san: $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_SRCS)

host/test_http_fuzz_san: $(HOST_HTTP_FUZZ_SRCS) $(HOST_HTTP_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_FUZZ_SRCS)

$(HOST_HTTP_LIVE): $(HOST_HTTP_LIVE_SRCS) $(HOST_HTTP_LIVE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_LIVE_SRCS)

# A real capture is an external input, so this target is not part of check or
# host-test. The proc text format and the binary framing are documented in
# docs/http-profile-integration.md ("Capture format (frozen)").
host-test-live: $(HOST_HTTP_LIVE)
	@if [ -z "$(FILE)" ]; then \
		echo "usage: make host-test-live FILE=capture.bin"; exit 2; \
	fi
	@./$(HOST_HTTP_LIVE) < "$(FILE)"
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python host/test_http_live_h11.py "$(FILE)"; \
	else \
		echo "host-test-live: h11 oracle skipped (uv or cached h11 unavailable)"; \
	fi

clean: clean-dkms.conf clean-dkms-tarball
	$(RM) $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_HTTP_LIVE) host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $(KBUILD_LLVM) clean

load:
	sudo insmod tbtun.ko

unload:
	sudo rmmod tbtun

.PHONY: format format-check
format:
	$(CLANG_FORMAT) --style=file -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --style=file --dry-run --Werror $(FORMAT_SRCS)

.PHONY: dkms-tarball dkms-srcs-check clean-dkms-tarball clean-dkms.conf

.always.make:

# dkms.conf is generated. The version comes only from tbtun/tbt_uapi.h via
# scripts/mkdkmsconf.sh; do not edit dkms.conf by hand.
dkms.conf: ./scripts/mkdkmsconf.sh tbtun/tbt_uapi.h .always.make
	./scripts/mkdkmsconf.sh > dkms.conf

clean-dkms.conf:
	$(RM) dkms.conf

# A DKMS tarball must be buildable on a machine that has never seen this tree,
# so every module source has to be in SRCS. Catch a new file that was not added
# before the archive is written, not on the target machine.
dkms-srcs-check:
	@missing=$$(find tbtun \( -name '*.c' -o -name '*.h' \) | sort | while IFS= read -r f; do \
		case " $(SRCS) " in *" $$f "*) ;; *) printf '%s\n' "$$f" ;; esac; done); \
	if [ -n "$$missing" ]; then \
		echo "dkms-srcs-check: SRCS omits:" >&2; \
		printf '%s\n' "$$missing" >&2; \
		exit 1; \
	fi

# scripts/build.sh is a developer convenience (it may source an env file from
# the author's home); a DKMS build must never depend on it, so it is excluded.
$(DKMS_TARBALL): dkms.conf dkms-srcs-check $(SRCS)
	$(TAR) zcf $(DKMS_TARBALL) \
		--transform 's,^,./dkms_source_tree/,' \
		dkms.conf \
		$(SRCS)

dkms-tarball: $(DKMS_TARBALL)

clean-dkms-tarball:
	$(RM) $(DKMS_TARBALL)
