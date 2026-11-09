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
                   tbtun/tbt_rec.h tbtun/tbt_http.h tbtun/tbt_crypto.h \
                   tbtun/tbt_fec.h tbtun/tbt_fec_adapt.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_portmap.h tbtun/tbt_tx_acct.h \
                   tbtun/tbt_core.c tbtun/tbt_session.c tbtun/tbt_procfs.c tbtun/tbt_listen.c \
                   tbtun/tbt_carrier.c tbtun/tbt_sock.c tbtun/tbt_tx.c tbtun/tbt_rx.c \
                   tbtun/tbt_parse.c tbtun/tbt_proto.c tbtun/tbt_rec.c tbtun/tbt_http.c \
                   tbtun/tbt_crypto.c tbtun/tbt_fec.c tbtun/tbt_fec_data.c tbtun/tbt_fec_adapt.c \
                   tbtun/tbt_fec_adapt_sclc.c tbtun/tbt_fec_sclc.c \
                   tbtun/tbt_fec_sclc.h tbtun/tbt_portmap.c \
                   tbtun/tbt_sc_ldpc.c tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_sc_ldpc.h tbtun/tbt_sc_ldpc_gf16.h tbtun/tbt_sclc_uplink.h \
                   scripts/mkdkmsconf.sh scripts/install_dkms.sh \
                   .clang-format
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))

HOSTCC          ?= cc
HOST_CFLAGS     ?= -std=gnu99 -O2 -Wall -Wextra -Werror
HOST_TEST       := host/test_parse
HOST_TEST_SRCS  := host/test_parse.c tbtun/tbt_parse.c tbtun/tbt_fec_adapt_sclc.c \
                   tbtun/tbt_portmap.c
HOST_TEST_HDRS  := tbtun/tbt_parse.h tbtun/tbt_portmap.h tbtun/tbt_uapi.h \
                   tbtun/tbt_crypto.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_adapt.h tbtun/tbt_fec.h
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
HOST_CRYPTO     := host/test_crypto
HOST_CRYPTO_SRCS := host/test_crypto.c tbtun/tbt_crypto.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_CRYPTO_HDRS := tbtun/tbt_crypto.h tbtun/tbt_http.h tbtun/tbt_proto.h tbtun/tbt_rec.h tbtun/tbt_uapi.h
HOST_BENCH_CRYPTO := host/bench_crypto
HOST_BENCH_CRYPTO_SRCS := host/bench_crypto.c tbtun/tbt_crypto.c tbtun/tbt_proto.c
HOST_BENCH_CRYPTO_HDRS := tbtun/tbt_crypto.h tbtun/tbt_proto.h tbtun/tbt_uapi.h
HOST_BENCH_MICRO := host/bench_micro
HOST_BENCH_MICRO_SRCS := host/bench_micro.c tbtun/tbt_fec.c tbtun/tbt_crypto.c tbtun/tbt_proto.c
HOST_BENCH_TXPATH := host/bench_txpath
HOST_BENCH_TXPATH_SRCS := host/bench_txpath.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_LIVE  := host/test_http_live
HOST_HTTP_LIVE_SRCS := host/test_http_live.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_LIVE_HDRS := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
HOST_REORDER    := host/test_reorder
HOST_REORDER_SRCS := host/test_reorder.c
HOST_ACK        := host/test_ack
HOST_ACK_SRCS   := host/test_ack.c
HOST_CONTROL    := host/test_control
HOST_CONTROL_SRCS := host/test_control.c
HOST_TX_PUBLISH := host/test_tx_publish
HOST_TX_PUBLISH_SRCS := host/test_tx_publish.c
HOST_TX_PUBLISH_HDRS := tbtun/tbt_tx_acct.h tbtun/tbt_uapi.h
HOST_TX_INDEX   := host/test_tx_index
HOST_TX_INDEX_SRCS := host/test_tx_index.c
HOST_TX_INDEX_HDRS := tbtun/tbt_tx_acct.h tbtun/tbt_uapi.h
HOST_FIN_PRIORITY := host/test_fin_priority
HOST_FIN_PRIORITY_SRCS := host/test_fin_priority.c
HOST_FIN_PRIORITY_HDRS := tbtun/tbt_tx_acct.h tbtun/tbt_uapi.h
HOST_CLOSE_WAKE := host/test_close_wake
HOST_CLOSE_WAKE_SRCS := host/test_close_wake.c
HOST_CLOSE_WAKE_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_HOLD       := host/test_hold
HOST_HOLD_SRCS  := host/test_hold.c
HOST_HOLD_HDRS  := tbtun/tbt_tx_acct.h tbtun/tbt_uapi.h
HOST_MULTISTREAM := host/test_multistream
HOST_MULTISTREAM_SRCS := host/test_multistream.c
HOST_ANCHOR     := host/test_anchor
HOST_ANCHOR_SRCS := host/test_anchor.c
HOST_FEC        := host/test_fec
HOST_FEC_SRCS   := host/test_fec.c tbtun/tbt_fec.c
HOST_FEC_HDRS   := tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_RX     := host/test_fec_rx
HOST_FEC_RX_SRCS := host/test_fec_rx.c tbtun/tbt_fec.c tbtun/tbt_crypto.c
HOST_FEC_RX_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h tbtun/tbt_crypto.h
HOST_FEC_OUT    := host/test_fec_out
HOST_FEC_OUT_SRCS := host/test_fec_out.c
HOST_FEC_ADAPT  := host/test_fec_adapt
HOST_FEC_ADAPT_SRCS := host/test_fec_adapt.c tbtun/tbt_fec_adapt.c
HOST_FEC_ADAPT_HDRS := tbtun/tbt_fec_adapt.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_ADAPT_SCLC := host/test_fec_adapt_sclc
HOST_FEC_ADAPT_SCLC_SRCS := host/test_fec_adapt_sclc.c tbtun/tbt_fec_adapt_sclc.c
HOST_FEC_ADAPT_SCLC_HDRS := tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h \
                   tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_SCLC   := host/test_fec_sclc
HOST_FEC_SCLC_SRCS := host/test_fec_sclc.c tbtun/tbt_fec_sclc.c
HOST_FEC_SCLC_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_PORTMAP    := host/test_portmap
HOST_PORTMAP_SRCS := host/test_portmap.c tbtun/tbt_portmap.c tbtun/tbt_parse.c \
                     tbtun/tbt_fec_adapt_sclc.c
HOST_PORTMAP_HDRS := tbtun/tbt_portmap.h tbtun/tbt_parse.h tbtun/tbt_uapi.h \
                     tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h \
                     tbtun/tbt_fec.h
HOST_LIFETIME   := host/test_lifetime
HOST_LIFETIME_SRCS := host/test_lifetime.c
HOST_SCLC       := host/test_sclc_uplink
HOST_SCLC_SRCS  := host/test_sclc_uplink.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c
HOST_SCLC_HDRS  := tbtun/tbt_sc_ldpc.h tbtun/tbt_sc_ldpc_gf16.h \
                   tbtun/tbt_sclc_uplink.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_SCLC_DATA  := host/test_fec_sclc_data
HOST_SCLC_DATA_SRCS := host/test_fec_sclc_data.c tbtun/tbt_fec.c \
                       tbtun/tbt_fec_sclc.c tbtun/tbt_sclc_uplink.c \
                       tbtun/tbt_sc_ldpc.c tbtun/tbt_sc_ldpc_gf16.c
HOST_SCLC_DATA_HDRS := tbtun/tbt_fec.h tbtun/tbt_fec_sclc.h \
                       tbtun/tbt_sclc_uplink.h tbtun/tbt_sc_ldpc.h \
                       tbtun/tbt_sc_ldpc_gf16.h tbtun/tbt_uapi.h
HOST_FAULT      := host/test_fault
HOST_FAULT_SRCS := host/test_fault.c tbtun/tbt_fec.c tbtun/tbt_crypto.c
HOST_FAULT_HDRS := tbtun/tbt_fec.h tbtun/tbt_crypto.h tbtun/tbt_uapi.h
HOST_BENCH_SCLC := host/bench_sclc_uplink
HOST_BENCH_SCLC_SRCS := host/bench_sclc_uplink.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c
SAN_CFLAGS      := -g -fsanitize=address,undefined -fno-omit-frame-pointer

obj-m           += tbtun.o
tbtun-objs      := tbtun/tbt_core.o tbtun/tbt_session.o tbtun/tbt_procfs.o \
                   tbtun/tbt_listen.o tbtun/tbt_proto.o tbtun/tbt_carrier.o \
                   tbtun/tbt_sock.o tbtun/tbt_tx.o tbtun/tbt_rx.o tbtun/tbt_rec.o \
                   tbtun/tbt_parse.o tbtun/tbt_http.o tbtun/tbt_crypto.o \
                   tbtun/tbt_portmap.o tbtun/tbt_fec.o tbtun/tbt_fec_data.o \
                   tbtun/tbt_fec_adapt.o tbtun/tbt_fec_adapt_sclc.o \
                   tbtun/tbt_fec_sclc.o \
                   tbtun/tbt_sc_ldpc.o \
                   tbtun/tbt_sc_ldpc_gf16.o tbtun/tbt_sclc_uplink.o

ccflags-y       := -std=gnu99 -I$(src)/tbtun
# ChaCha8 is a hot per-byte loop; keep the kernel's -O2 for the rest of the
# module and let this one file unroll its fixed 8-round permutation. The
# variable must name the object path Kbuild sees for a composite module.
CFLAGS_tbtun/tbt_crypto.o := -O3

.PHONY: all check clean load unload qemu-test host-test host-test-san host-test-san-run host-test-live host-bench-micro host-bench-sclc bench bench-full

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
host-test: $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_CRYPTO) $(HOST_REORDER) $(HOST_ACK) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FEC_RX) $(HOST_FEC_OUT) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_PORTMAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_SCLC_DATA) $(HOST_FAULT)
	@./$(HOST_TEST)
	@./$(HOST_FUZZ)
	@./$(HOST_HTTP)
	@./$(HOST_CRYPTO)
	@./$(HOST_REORDER)
	@./$(HOST_ACK)
	@./$(HOST_CONTROL)
	@./$(HOST_TX_PUBLISH)
	@./$(HOST_TX_INDEX)
	@./$(HOST_FIN_PRIORITY)
	@./$(HOST_CLOSE_WAKE)
	@./$(HOST_HOLD)
	@./$(HOST_MULTISTREAM)
	@./$(HOST_ANCHOR)
	@./$(HOST_FEC)
	@./$(HOST_FEC_RX)
	@./$(HOST_FEC_OUT)
	@./$(HOST_FEC_ADAPT)
	@./$(HOST_FEC_ADAPT_SCLC)
	@./$(HOST_FEC_SCLC)
	@./$(HOST_PORTMAP)
	@./$(HOST_LIFETIME)
	@./$(HOST_SCLC)
	@./$(HOST_SCLC_DATA)
	@./$(HOST_FAULT)
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

$(HOST_CRYPTO): $(HOST_CRYPTO_SRCS) $(HOST_CRYPTO_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CRYPTO_SRCS)

$(HOST_BENCH_CRYPTO): $(HOST_BENCH_CRYPTO_SRCS) $(HOST_BENCH_CRYPTO_HDRS)
	$(HOSTCC) -std=gnu99 -O3 -Wall -Wextra -Werror -I tbtun -o $@ $(HOST_BENCH_CRYPTO_SRCS)

# Throughput and cycles/byte for the host port. Not part of `check`: it is a
# measurement, not a gate.
host-bench-crypto: $(HOST_BENCH_CRYPTO)
	@./$(HOST_BENCH_CRYPTO)

# The microbenchmark base: one binary, CSV on stdout. -O2 is the module's
# default for tbt_fec.c; the crypto object is -O3 in the module, so its rows
# here are a lower bound (host/bench_crypto measures that core at -O3).
$(HOST_BENCH_MICRO): $(HOST_BENCH_MICRO_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_MICRO_SRCS)

host-bench-micro: $(HOST_BENCH_MICRO)

# Per-record FEC TX cost: queue allocation, HTTP chunk framing, and the
# buffer-reuse alternative. Not part of `check`; it is a measurement.
$(HOST_BENCH_TXPATH): $(HOST_BENCH_TXPATH_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_TXPATH_SRCS)

host-bench-txpath: $(HOST_BENCH_TXPATH)

# The feedback loop. `bench` is the <= 2 s pass for iteration; `bench-full`
# is the <= 10 s pass for a recorded baseline. Both write a timestamped
# directory under benches/results/ and print its path.
bench: $(HOST_BENCH_MICRO)
	@./scripts/microbench.sh run --quick

bench-full: $(HOST_BENCH_MICRO)
	@./scripts/microbench.sh run --full

$(HOST_REORDER): $(HOST_REORDER_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_REORDER_SRCS)

$(HOST_ACK): $(HOST_ACK_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_ACK_SRCS)

$(HOST_CONTROL): $(HOST_CONTROL_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_CONTROL_SRCS)

$(HOST_TX_PUBLISH): $(HOST_TX_PUBLISH_SRCS) $(HOST_TX_PUBLISH_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_TX_PUBLISH_SRCS)

$(HOST_TX_INDEX): $(HOST_TX_INDEX_SRCS) $(HOST_TX_INDEX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_TX_INDEX_SRCS)

$(HOST_FIN_PRIORITY): $(HOST_FIN_PRIORITY_SRCS) $(HOST_FIN_PRIORITY_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FIN_PRIORITY_SRCS)

$(HOST_CLOSE_WAKE): $(HOST_CLOSE_WAKE_SRCS) $(HOST_CLOSE_WAKE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CLOSE_WAKE_SRCS)

$(HOST_HOLD): $(HOST_HOLD_SRCS) $(HOST_HOLD_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_HOLD_SRCS)

$(HOST_MULTISTREAM): $(HOST_MULTISTREAM_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_MULTISTREAM_SRCS)

$(HOST_ANCHOR): $(HOST_ANCHOR_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_ANCHOR_SRCS)

$(HOST_FEC): $(HOST_FEC_SRCS) $(HOST_FEC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SRCS)

$(HOST_FEC_RX): $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_RX_SRCS)

$(HOST_FEC_OUT): $(HOST_FEC_OUT_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

$(HOST_FEC_ADAPT): $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SRCS)

$(HOST_FEC_ADAPT_SCLC): $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

$(HOST_FEC_SCLC): $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SRCS)

$(HOST_PORTMAP): $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_PORTMAP_SRCS)

$(HOST_LIFETIME): $(HOST_LIFETIME_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

$(HOST_SCLC): $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_SRCS)

$(HOST_SCLC_DATA): $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_FAULT): $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FAULT_SRCS)

host/test_fec_sclc_data_san: $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_BENCH_SCLC): $(HOST_BENCH_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_SRCS)

host-bench-sclc: $(HOST_BENCH_SCLC)
	@./$(HOST_BENCH_SCLC) 8 8 24 1 4 0.3
	@./$(HOST_BENCH_SCLC) 8 8 24 1 4 0.4

# The same tests with ASan and UBSan. A toolchain without the runtimes gets
# a clear skip, not a build failure.
host-test-san:
	@if printf 'int main(void){return 0;}\n' | $(HOSTCC) $(SAN_CFLAGS) -x c - -o /dev/null 2>/dev/null; then \
		$(MAKE) --no-print-directory host-test-san-run; \
	else \
		echo "host-test-san: skipped, $(HOSTCC) cannot link -fsanitize=address,undefined"; \
	fi

host-test-san-run: host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san host/test_crypto_san host/test_fec_san host/test_fec_rx_san host/test_fec_out_san host/test_fec_adapt_san host/test_fec_adapt_sclc_san host/test_fec_sclc_san host/test_close_wake_san host/test_portmap_san host/test_lifetime_san host/test_sclc_uplink_san host/test_fec_sclc_data_san host/test_fault_san
	@./host/test_parse_san
	@./host/test_fuzz_san
	@./host/test_http_san
	@./host/test_http_fuzz_san
	@./host/test_crypto_san
	@./host/test_fec_san
	@./host/test_fec_rx_san
	@./host/test_fec_out_san
	@./host/test_fec_adapt_san
	@./host/test_fec_adapt_sclc_san
	@./host/test_fec_sclc_san
	@./host/test_portmap_san
	@./host/test_lifetime_san
	@./host/test_sclc_uplink_san
	@./host/test_fec_sclc_data_san
	@./host/test_close_wake_san
	@./host/test_fault_san

host/test_parse_san: $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_TEST_SRCS)

host/test_fuzz_san: $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FUZZ_SRCS)

host/test_http_san: $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_SRCS)

host/test_http_fuzz_san: $(HOST_HTTP_FUZZ_SRCS) $(HOST_HTTP_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_FUZZ_SRCS)

host/test_crypto_san: $(HOST_CRYPTO_SRCS) $(HOST_CRYPTO_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CRYPTO_SRCS)

host/test_fec_san: $(HOST_FEC_SRCS) $(HOST_FEC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SRCS)

host/test_close_wake_san: $(HOST_CLOSE_WAKE_SRCS) $(HOST_CLOSE_WAKE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CLOSE_WAKE_SRCS)

host/test_fec_rx_san: $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_RX_SRCS)

host/test_fec_out_san: $(HOST_FEC_OUT_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

host/test_fec_adapt_san: $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SRCS)

host/test_fec_adapt_sclc_san: $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

host/test_fec_sclc_san: $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SRCS)

host/test_portmap_san: $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_PORTMAP_SRCS)

host/test_lifetime_san: $(HOST_LIFETIME_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

host/test_sclc_uplink_san: $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_SRCS)

host/test_fault_san: $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FAULT_SRCS)

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
	$(RM) $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_HTTP_LIVE) $(HOST_CRYPTO) $(HOST_BENCH_CRYPTO) $(HOST_BENCH_MICRO) $(HOST_BENCH_TXPATH) $(HOST_REORDER) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FEC_RX) $(HOST_FEC_OUT) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_PORTMAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_FAULT) $(HOST_BENCH_SCLC) host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san host/test_crypto_san host/test_fec_san host/test_fec_rx_san host/test_fec_out_san host/test_fec_adapt_san host/test_fec_adapt_sclc_san host/test_fec_sclc_san host/test_close_wake_san host/test_portmap_san host/test_lifetime_san host/test_sclc_uplink_san host/test_fault_san
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
