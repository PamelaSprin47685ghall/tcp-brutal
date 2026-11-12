KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
# Kernels built with clang (CONFIG_CC_IS_CLANG) need LLVM=1 for the module too
KERNEL_CONFIG   := $(firstword $(wildcard $(KERNEL_DIR)/include/config/auto.conf $(KERNEL_DIR)/.config))
KBUILD_LLVM     := $(if $(KERNEL_CONFIG),$(if $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KERNEL_CONFIG) && echo y),LLVM=1))
DKMS_TARBALL    ?= dkms.tar.gz
TAR             ?= tar
CLANG_FORMAT    ?= clang-format-18
# Out-of-source build root: every generated object, library and binary lives
# here, so the source tree stays clean.
BUILD           ?= $(CURDIR)/build
BINDIR          := $(BUILD)/bin
OBJDIR          := $(BUILD)/obj
LIBDIR          := $(BUILD)/lib
SRCS            := Makefile \
                   src/kernel/Kbuild \
                   src/kernel/tbtun.h src/kernel/tbt_uapi.h src/kernel/tbt_parse.h src/kernel/tbt_proto.h \
                   src/kernel/tbt_rec.h src/kernel/tbt_http.h src/kernel/tbt_fec_batch.h \
                   src/kernel/tbt_crypto.h \
                   src/kernel/tbt_fec.h src/kernel/tbt_fec_late.h src/kernel/tbt_fec_adapt.h \
                   src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_sclc_delta.h \
                   src/kernel/tbt_portmap.h src/kernel/tbt_map.h src/kernel/tbt_carrier_policy.h \
                   src/kernel/tbt_tx_acct.h \
                   src/kernel/tbt_rx_budget.h \
                   src/kernel/tbt_core.c src/kernel/tbt_session.c src/kernel/tbt_procfs.c src/kernel/tbt_listen.c \
                   src/kernel/tbt_carrier.c src/kernel/tbt_cc.c src/kernel/tbt_cc.h src/kernel/tbt_sock.c \
                   src/kernel/tbt_tx.c src/kernel/tbt_rx.c \
                   src/kernel/tbt_parse.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c src/kernel/tbt_http.c \
                   src/kernel/tbt_fec_batch.c \
                   src/kernel/tbt_crypto.c src/kernel/tbt_fec.c \
                   src/kernel/tbt_fec_data.c src/kernel/tbt_fec_adapt.c \
                   src/kernel/tbt_fec_adapt_sclc.c src/kernel/tbt_fec_sclc.c \
                   src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec_codec_cache.c \
                   src/kernel/tbt_fec_codec_cache.h \
                   src/kernel/tbt_portmap.c src/kernel/tbt_map.c \
                   src/kernel/tbt_sc_ldpc.c src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c \
                   src/kernel/tbt_sc_ldpc.h src/kernel/tbt_sc_ldpc_gf16.h src/kernel/tbt_sclc_uplink.h \
                   tools/mkdkmsconf.sh tools/install_dkms.sh \
                   .clang-format
# tools/install_dkms.sh stages these beside src/kernel/*.c and *.h. A staged
# tree cannot configure itself if one of them leaves SRCS.
DKMS_STAGE_FILES := Makefile .clang-format src/kernel/Kbuild tools/mkdkmsconf.sh tools/install_dkms.sh
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))

HOSTCC          ?= cc
HOST_CFLAGS     ?= -std=gnu99 -O2 -Wall -Wextra -Werror -I src/client
HOST_TEST := $(BINDIR)/test_parse
HOST_TEST_SRCS  := tests/unit/test_parse.c src/kernel/tbt_parse.c src/kernel/tbt_fec_adapt_sclc.c \
                   src/kernel/tbt_portmap.c src/kernel/tbt_map.c
HOST_TEST_HDRS  := src/kernel/tbt_parse.h src/kernel/tbt_portmap.h src/kernel/tbt_map.h src/kernel/tbt_uapi.h \
                   src/kernel/tbt_crypto.h src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_adapt.h src/kernel/tbt_fec.h
HOST_FUZZ := $(BINDIR)/test_fuzz
HOST_FUZZ_SRCS  := tests/unit/test_fuzz.c src/kernel/tbt_rec.c src/kernel/tbt_proto.c \
                   src/kernel/tbt_fec.c src/kernel/tbt_parse.c \
                   src/kernel/tbt_fec_adapt_sclc.c src/kernel/tbt_portmap.c
HOST_FUZZ_HDRS  := src/kernel/tbt_rec.h src/kernel/tbt_proto.h src/kernel/tbt_uapi.h \
                   src/kernel/tbt_fec.h src/kernel/tbt_parse.h src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_adapt.h src/kernel/tbt_portmap.h
HOST_HTTP := $(BINDIR)/test_http
HOST_HTTP_SRCS  := tests/unit/test_http.c src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_HTTP_HDRS  := src/kernel/tbt_http.h src/kernel/tbt_uapi.h src/kernel/tbt_proto.h src/kernel/tbt_rec.h
HOST_HTTP_H11   := tests/unit/test_http_h11.py
HOST_HTTP_FUZZ := $(BINDIR)/test_http_fuzz
HOST_HTTP_FUZZ_SRCS := tests/unit/test_http_fuzz.c src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_HTTP_FUZZ_HDRS := src/kernel/tbt_http.h src/kernel/tbt_uapi.h src/kernel/tbt_proto.h src/kernel/tbt_rec.h
HTTP_RFC_GEN := $(BINDIR)/http_rfc9112_gen
HTTP_RFC_GEN_SRCS := tests/http/http_rfc9112_gen.c src/kernel/tbt_http.c src/kernel/tbt_proto.c \
                   src/kernel/tbt_rec.c src/kernel/tbt_fec.c
HTTP_RFC_GEN_HDRS := src/kernel/tbt_http.h src/kernel/tbt_uapi.h src/kernel/tbt_proto.h \
                   src/kernel/tbt_rec.h src/kernel/tbt_fec.h
HTTP_RFC_GEN_SAN := $(BINDIR)/http_rfc9112_gen_san
HTTP_RFC_AUDIT  := tests/http/http_rfc9112_audit.py
HOST_CRYPTO := $(BINDIR)/test_crypto
HOST_CRYPTO_SRCS := tests/unit/test_crypto.c src/kernel/tbt_crypto.c src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_CRYPTO_HDRS := src/kernel/tbt_crypto.h src/kernel/tbt_http.h src/kernel/tbt_proto.h src/kernel/tbt_rec.h src/kernel/tbt_uapi.h
HOST_BENCH_CRYPTO := $(BINDIR)/bench_crypto
HOST_BENCH_CRYPTO_SRCS := tests/bench/bench_crypto.c src/kernel/tbt_crypto.c src/kernel/tbt_proto.c
HOST_BENCH_CRYPTO_HDRS := src/kernel/tbt_crypto.h src/kernel/tbt_proto.h src/kernel/tbt_uapi.h
HOST_BENCH_MICRO := $(BINDIR)/bench_micro
HOST_BENCH_MICRO_SRCS := tests/bench/bench_micro.c src/kernel/tbt_fec.c src/kernel/tbt_crypto.c src/kernel/tbt_proto.c
HOST_BENCH_TXPATH := $(BINDIR)/bench_txpath
HOST_BENCH_TXPATH_SRCS := tests/bench/bench_txpath.c src/kernel/tbt_fec_batch.c \
                   src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_BENCH_RXPATH := $(BINDIR)/bench_rx_stages
HOST_BENCH_RXPATH_SRCS := tests/bench/bench_rx_stages.c src/kernel/tbt_http.c \
                   src/kernel/tbt_fec.c src/kernel/tbt_crypto.c src/kernel/tbt_proto.c \
                   src/kernel/tbt_rec.c
HOST_HTTP_LIVE := $(BINDIR)/test_http_live
HOST_HTTP_LIVE_SRCS := tests/unit/test_http_live.c src/kernel/tbt_http.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_HTTP_LIVE_HDRS := src/kernel/tbt_http.h src/kernel/tbt_uapi.h src/kernel/tbt_proto.h src/kernel/tbt_rec.h
HOST_REORDER := $(BINDIR)/test_reorder
HOST_REORDER_SRCS := tests/unit/test_reorder.c
HOST_ACK := $(BINDIR)/test_ack
HOST_ACK_SRCS   := tests/unit/test_ack.c
HOST_CONTROL := $(BINDIR)/test_control
HOST_CONTROL_SRCS := tests/unit/test_control.c
HOST_TX_PUBLISH := $(BINDIR)/test_tx_publish
HOST_TX_PUBLISH_SRCS := tests/unit/test_tx_publish.c
HOST_TX_PUBLISH_HDRS := src/kernel/tbt_tx_acct.h src/kernel/tbt_uapi.h
HOST_TX_INDEX := $(BINDIR)/test_tx_index
HOST_TX_INDEX_SRCS := tests/unit/test_tx_index.c
HOST_TX_INDEX_HDRS := src/kernel/tbt_tx_acct.h src/kernel/tbt_uapi.h
HOST_FIN_PRIORITY := $(BINDIR)/test_fin_priority
HOST_FIN_PRIORITY_SRCS := tests/unit/test_fin_priority.c
HOST_FIN_PRIORITY_HDRS := src/kernel/tbt_tx_acct.h src/kernel/tbt_uapi.h
HOST_CLOSE_WAKE := $(BINDIR)/test_close_wake
HOST_CLOSE_WAKE_SRCS := tests/unit/test_close_wake.c
HOST_CLOSE_WAKE_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_HOLD := $(BINDIR)/test_hold
HOST_HOLD_SRCS  := tests/unit/test_hold.c
HOST_HOLD_HDRS  := src/kernel/tbt_tx_acct.h src/kernel/tbt_uapi.h
HOST_MULTISTREAM := $(BINDIR)/test_multistream
HOST_MULTISTREAM_SRCS := tests/unit/test_multistream.c
HOST_ANCHOR := $(BINDIR)/test_anchor
HOST_ANCHOR_SRCS := tests/unit/test_anchor.c
HOST_FEC := $(BINDIR)/test_fec
HOST_FEC_SRCS   := tests/unit/test_fec.c src/kernel/tbt_fec.c
HOST_FEC_HDRS   := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_FIN_TAIL := $(BINDIR)/test_fin_tail
HOST_FIN_TAIL_SRCS := tests/unit/test_fin_tail.c
HOST_FIN_TAIL_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_CELL_TAIL := $(BINDIR)/test_cell_tail
HOST_CELL_TAIL_SRCS := tests/unit/test_cell_tail.c
HOST_CELL_TAIL_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_MEM_BUDGET := $(BINDIR)/test_mem_budget
HOST_MEM_BUDGET_SRCS := tests/unit/test_mem_budget.c src/kernel/tbt_fec.c
HOST_MEM_BUDGET_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_http.h src/kernel/tbt_rec.h \
                   src/kernel/tbt_proto.h src/kernel/tbt_uapi.h
HOST_FEC_RX := $(BINDIR)/test_fec_rx
HOST_FEC_RX_SRCS := tests/unit/test_fec_rx.c src/kernel/tbt_fec.c src/kernel/tbt_crypto.c
HOST_FEC_RX_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h src/kernel/tbt_crypto.h
HOST_FEC_SWITCH := $(BINDIR)/test_fec_switch
HOST_FEC_SWITCH_SRCS := tests/unit/test_fec_switch.c src/kernel/tbt_fec.c
HOST_FEC_SWITCH_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_FEC_OUT := $(BINDIR)/test_fec_out
HOST_FEC_OUT_SRCS := tests/unit/test_fec_out.c
HOST_CODEC_CACHE := $(BINDIR)/test_fec_codec_cache
HOST_CODEC_CACHE_SRCS := tests/unit/test_fec_codec_cache.c \
                   src/kernel/tbt_fec_codec_cache.c
HOST_CODEC_CACHE_HDRS := src/kernel/tbt_fec_codec_cache.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_uapi.h
HOST_BATCH := $(BINDIR)/test_batch
HOST_BATCH_SRCS := tests/unit/test_batch.c src/kernel/tbt_fec_batch.c src/kernel/tbt_http.c \
                   src/kernel/tbt_proto.c src/kernel/tbt_rec.c
HOST_BATCH_HDRS := src/kernel/tbt_fec_batch.h src/kernel/tbt_http.h src/kernel/tbt_uapi.h \
                   src/kernel/tbt_proto.h src/kernel/tbt_rec.h
HOST_FEC_ADAPT := $(BINDIR)/test_fec_adapt
HOST_FEC_ADAPT_SRCS := tests/unit/test_fec_adapt.c src/kernel/tbt_fec_adapt.c
HOST_FEC_ADAPT_HDRS := src/kernel/tbt_fec_adapt.h src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_FEC_ADAPT_SCLC := $(BINDIR)/test_fec_adapt_sclc
HOST_FEC_ADAPT_SCLC_SRCS := tests/unit/test_fec_adapt_sclc.c src/kernel/tbt_fec_adapt_sclc.c
HOST_FEC_ADAPT_SCLC_HDRS := src/kernel/tbt_fec_adapt_sclc.h src/kernel/tbt_fec_adapt.h \
                   src/kernel/tbt_fec_sclc_delta.h src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_FEC_SCLC := $(BINDIR)/test_fec_sclc
HOST_FEC_SCLC_SRCS := tests/unit/test_fec_sclc.c src/kernel/tbt_fec_sclc.c
HOST_FEC_SCLC_HDRS := src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_FEC_SCLC_SHAPES := $(BINDIR)/test_fec_sclc_shapes
HOST_FEC_SCLC_SHAPES_SRCS := tests/unit/test_fec_sclc_shapes.c src/kernel/tbt_fec_sclc.c \
                   src/kernel/tbt_fec_adapt_sclc.c
HOST_FEC_SCLC_SHAPES_HDRS := src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_sclc_delta.h src/kernel/tbt_fec_adapt.h \
                   src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_PORTMAP := $(BINDIR)/test_portmap
HOST_PORTMAP_SRCS := tests/unit/test_portmap.c src/kernel/tbt_portmap.c src/kernel/tbt_parse.c \
                     src/kernel/tbt_fec_adapt_sclc.c src/kernel/tbt_map.c
HOST_PORTMAP_HDRS := src/kernel/tbt_portmap.h src/kernel/tbt_parse.h src/kernel/tbt_uapi.h \
                     src/kernel/tbt_map.h src/kernel/tbt_fec_adapt_sclc.h src/kernel/tbt_fec_adapt.h \
                     src/kernel/tbt_fec.h
HOST_MAP := $(BINDIR)/test_map
HOST_MAP_SRCS   := tests/unit/test_map.c src/kernel/tbt_map.c src/kernel/tbt_parse.c \
                   src/kernel/tbt_fec_adapt_sclc.c src/kernel/tbt_portmap.c
HOST_MAP_HDRS   := src/kernel/tbt_map.h src/kernel/tbt_parse.h src/kernel/tbt_uapi.h \
                   src/kernel/tbt_proto.h
HOST_CARRIER_POLICY := $(BINDIR)/test_carrier_policy
HOST_CARRIER_POLICY_HDRS := src/kernel/tbt_carrier_policy.h src/kernel/tbt_uapi.h
HOST_LIFETIME := $(BINDIR)/test_lifetime
HOST_LIFETIME_SRCS := tests/unit/test_lifetime.c
HOST_SCLC := $(BINDIR)/test_sclc_uplink
HOST_SCLC_SRCS  := tests/unit/test_sclc_uplink.c src/kernel/tbt_sc_ldpc.c \
                   src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c
HOST_SCLC_HDRS  := src/kernel/tbt_sc_ldpc.h src/kernel/tbt_sc_ldpc_gf16.h \
                   src/kernel/tbt_sclc_uplink.h src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_SCLC_DATA := $(BINDIR)/test_fec_sclc_data
HOST_SCLC_DATA_SRCS := tests/unit/test_fec_sclc_data.c src/kernel/tbt_fec.c \
                       src/kernel/tbt_fec_sclc.c src/kernel/tbt_sclc_uplink.c \
                       src/kernel/tbt_sc_ldpc.c src/kernel/tbt_sc_ldpc_gf16.c
HOST_SCLC_DATA_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_fec_sclc.h \
                       src/kernel/tbt_sclc_uplink.h src/kernel/tbt_sc_ldpc.h \
                       src/kernel/tbt_sc_ldpc_gf16.h src/kernel/tbt_uapi.h
HOST_FAULT := $(BINDIR)/test_fault
HOST_FAULT_SRCS := tests/unit/test_fault.c src/kernel/tbt_fec.c src/kernel/tbt_crypto.c
HOST_FAULT_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_crypto.h src/kernel/tbt_uapi.h
HOST_INVARIANTS := $(BINDIR)/test_invariants
HOST_INVARIANTS_SRCS := tests/unit/test_invariants.c src/kernel/tbt_fec.c \
                   src/kernel/tbt_portmap.c src/kernel/tbt_fec_adapt_sclc.c
HOST_INVARIANTS_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h src/kernel/tbt_portmap.h \
                   src/kernel/tbt_fec_adapt_sclc.h src/kernel/tbt_fec_adapt.h
HOST_WIRE_COMPAT := $(BINDIR)/test_wire_compat
HOST_WIRE_COMPAT_SRCS := tests/unit/test_wire_compat.c src/kernel/tbt_rec.c \
                   src/kernel/tbt_proto.c src/kernel/tbt_fec.c src/kernel/tbt_crypto.c
HOST_WIRE_COMPAT_HDRS := src/kernel/tbt_rec.h src/kernel/tbt_proto.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_crypto.h src/kernel/tbt_uapi.h
HOST_RX_BUDGET := $(BINDIR)/test_rx_budget
HOST_RX_BUDGET_SRCS := tests/unit/test_rx_budget.c
HOST_RX_BUDGET_HDRS := src/kernel/tbt_rx_budget.h src/kernel/tbt_uapi.h
HOST_CAPTURE_POKE := $(BINDIR)/test_capture_poke
HOST_CAPTURE_POKE_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_LOCAL_QUEUE := $(BINDIR)/test_local_queue
HOST_LOCAL_QUEUE_HDRS := src/kernel/tbt_uapi.h

# The userspace client links the module's pure C units and adds only the
# session/socket engine. The library is the reusable half: 0.8.x Android and
# Linux front ends link it, and the loopback test drives two engines.
HOST_CLIENT_LIB_SRCS := src/client/tbt_client_engine.c \
                   src/kernel/tbt_parse.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c \
                   src/kernel/tbt_http.c src/kernel/tbt_fec_batch.c \
                   src/kernel/tbt_crypto.c src/kernel/tbt_fec.c \
                   src/kernel/tbt_fec_sclc.c src/kernel/tbt_sc_ldpc.c \
                   src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c \
                   src/kernel/tbt_fec_adapt.c src/kernel/tbt_fec_adapt_sclc.c \
                   src/kernel/tbt_fec_codec_cache.c src/kernel/tbt_portmap.c
HOST_CLIENT_HDRS := src/client/tbt_client_engine.h src/kernel/tbt_parse.h \
                   src/kernel/tbt_proto.h src/kernel/tbt_rec.h src/kernel/tbt_http.h \
                   src/kernel/tbt_fec_batch.h src/kernel/tbt_crypto.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_fec_sclc.h src/kernel/tbt_sc_ldpc.h \
                   src/kernel/tbt_sc_ldpc_gf16.h src/kernel/tbt_sclc_uplink.h \
                   src/kernel/tbt_fec_adapt.h src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_codec_cache.h src/kernel/tbt_portmap.h \
                   src/kernel/tbt_uapi.h
HOST_CLIENT_OBJS := $(OBJDIR)/client-engine.o \
                   $(OBJDIR)/client-args.o \
                   $(patsubst src/kernel/%.c,$(OBJDIR)/client-%.o,$(filter src/kernel/%.c,$(HOST_CLIENT_LIB_SRCS)))
HOST_CLIENT := $(BINDIR)/tbt_client
HOST_CLIENT_LIB := $(LIBDIR)/libtbtun_client.a
HOST_CLIENT_ARGS := $(BINDIR)/test_client_args
HOST_CLIENT_ARGS_SRCS := tests/unit/test_client_args.c src/client/tbt_client_args.c \
                   src/kernel/tbt_parse.c src/kernel/tbt_fec_adapt_sclc.c \
                   src/kernel/tbt_fec_adapt.c src/kernel/tbt_portmap.c src/kernel/tbt_map.c
HOST_CLIENT_ARGS_HDRS := src/client/tbt_client_args.h src/kernel/tbt_parse.h \
                   src/kernel/tbt_uapi.h src/kernel/tbt_crypto.h src/kernel/tbt_portmap.h \
                   src/kernel/tbt_map.h
HOST_CLIENT_LOOPBACK := $(BINDIR)/test_client_loopback
HOST_CLIENT_WIRE := $(BINDIR)/test_client_wire
HOST_CLIENT_WIRE_SRCS := tests/unit/test_client_wire.c src/kernel/tbt_proto.c \
                   src/kernel/tbt_rec.c src/kernel/tbt_fec.c src/kernel/tbt_http.c
HOST_CLIENT_WIRE_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_http.h src/kernel/tbt_proto.h \
                   src/kernel/tbt_rec.h src/kernel/tbt_uapi.h
HOST_CLIENT_RX := $(BINDIR)/test_client_rx
HOST_CLIENT_RX_SRCS := tests/unit/test_client_rx.c \
                   $(filter-out src/client/tbt_client_engine.c,$(HOST_CLIENT_LIB_SRCS))
HOST_BENCH_SCLC := $(BINDIR)/bench_sclc_uplink
HOST_BENCH_SCLC_SRCS := tests/bench/bench_sclc_uplink.c src/kernel/tbt_sc_ldpc.c \
                   src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c
HOST_BENCH_RECOVERY := $(BINDIR)/bench_sclc_recovery
HOST_BENCH_RECOVERY_SRCS := tests/bench/bench_sclc_recovery.c src/kernel/tbt_sc_ldpc.c \
                   src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c
HOST_BENCH_SCLC_DOWN := $(BINDIR)/bench_sclc_downlink
HOST_BENCH_SCLC_DOWN_SRCS := tests/bench/bench_sclc_downlink.c src/kernel/tbt_fec_sclc.c
HOST_BENCH_SCLC_DOWN_HDRS := src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_uapi.h
HOST_CALIB_SCLC_DOWN := $(BINDIR)/calib_sclc_down
HOST_CALIB_SCLC_DOWN_SRCS := tests/bench/calib_sclc_down.c src/kernel/tbt_fec_sclc.c
HOST_CALIB_SCLC_DOWN_HDRS := src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_uapi.h
HOST_CALIB_SCLC_DE := $(BINDIR)/calib_sclc_de
HOST_CALIB_SCLC_DE_SRCS := tests/bench/calib_sclc_de.c src/kernel/tbt_fec_sclc.c
HOST_CALIB_SCLC_DE_HDRS := src/kernel/tbt_fec_sclc.h src/kernel/tbt_fec.h \
                   src/kernel/tbt_uapi.h
HOST_BENCH_SCLC_PIPE := $(BINDIR)/bench_sclc_pipe
HOST_BENCH_SCLC_PIPE_SRCS := tests/bench/bench_sclc_pipe.c src/kernel/tbt_fec.c \
                   src/kernel/tbt_fec_sclc.c src/kernel/tbt_sclc_uplink.c \
                   src/kernel/tbt_sc_ldpc.c src/kernel/tbt_sc_ldpc_gf16.c \
                   src/kernel/tbt_crypto.c src/kernel/tbt_proto.c
HOST_BENCH_SCLC_BUILD := $(BINDIR)/bench_sclc_build
HOST_BENCH_SCLC_BUILD_SRCS := tests/bench/bench_sclc_build.c src/kernel/tbt_fec.c \
                   src/kernel/tbt_fec_sclc.c src/kernel/tbt_sclc_uplink.c \
                   src/kernel/tbt_sc_ldpc.c src/kernel/tbt_sc_ldpc_gf16.c
HOST_BENCH_SCLC_BUILD_HDRS := src/kernel/tbt_fec.h src/kernel/tbt_fec_sclc.h \
                   src/kernel/tbt_sclc_uplink.h src/kernel/tbt_sc_ldpc.h \
                   src/kernel/tbt_sc_ldpc_gf16.h src/kernel/tbt_uapi.h
HOST_CALIB_SCLC_UP := $(BINDIR)/calib_sclc_up
HOST_CALIB_SCLC_UP_SRCS := tests/bench/calib_sclc_up.c src/kernel/tbt_sc_ldpc.c \
                   src/kernel/tbt_sc_ldpc_gf16.c src/kernel/tbt_sclc_uplink.c \
                   src/kernel/tbt_fec_adapt_sclc.c
HOST_CALIB_SCLC_UP_HDRS := src/kernel/tbt_sc_ldpc.h src/kernel/tbt_sc_ldpc_gf16.h \
                   src/kernel/tbt_sclc_uplink.h src/kernel/tbt_fec_adapt_sclc.h \
                   src/kernel/tbt_fec_adapt.h src/kernel/tbt_fec.h src/kernel/tbt_uapi.h
HOST_TSAN := $(BINDIR)/test_tsan_race
HOST_TSAN_SRCS  := tests/unit/test_tsan_race.c src/kernel/tbt_crypto.c
HOST_TSAN_HDRS  := src/kernel/tbt_fec.h src/kernel/tbt_crypto.h src/kernel/tbt_uapi.h
SAN_CFLAGS      := -g -fsanitize=address,undefined -fno-omit-frame-pointer
# A sanitizer run must stop at the first undefined-behaviour report instead
# of continuing through the rest of the suite.
export UBSAN_OPTIONS ?= halt_on_error=1
TSAN_CFLAGS     := -g -fsanitize=thread -fno-omit-frame-pointer

# The module objects, ccflags and per-file -O3 flags live in
# src/kernel/Kbuild; this Makefile only owns the out-of-source build path.

.PHONY: all check clean load unload qemu-test host-test host-test-san host-test-san-run host-test-tsan host-test-live host-bench-micro host-bench-sclc host-bench-sclc-down host-bench-sclc-pipe host-bench-sclc-build host-bench-rxpath host-calib-sclc-up calib-sclc-up bench bench-full

all:
	@mkdir -p $(abspath $(BUILD))/kernel
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR)/src/kernel MO=$(abspath $(BUILD))/kernel $(KBUILD_LLVM) modules

# One command for a human: resolves the header payload, prints the make line it
# runs, then builds the module and runs all host tests (docs/build-env.md).
check: dkms-srcs-check
	@if [ -x ./tools/build.sh ]; then ./tools/build.sh; else \
		$(MAKE) KERNEL_DIR=$(KERNEL_DIR) all; fi
	@$(MAKE) --no-print-directory host-test

# The host tests link the same parser sources the module uses, so they need no
# root, no module and no kernel headers: the grammar test (tbt_parse.c), the
# hostile-input test (tbt_rec.c + tbt_proto.c) and the HTTP container test
# (tbt_http.c + tbt_proto.c).
# The carrier policy test is header-only; its dependency sits on its own line
# so the long host-test list stays readable.
host-test: $(HOST_CARRIER_POLICY)
host-test: $(HOST_CAPTURE_POKE)
host-test: $(HOST_LOCAL_QUEUE)
host-test-san-run: $(BINDIR)/test_carrier_policy_san

host-test: $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_HTTP_FUZZ) $(HOST_CRYPTO) $(HOST_REORDER) $(HOST_ACK) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FIN_TAIL) $(HOST_CELL_TAIL) $(HOST_MEM_BUDGET) $(HOST_FEC_RX) $(HOST_FEC_SWITCH) $(HOST_FEC_OUT) $(HOST_CODEC_CACHE) $(HOST_BATCH) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_FEC_SCLC_SHAPES) $(HOST_PORTMAP) $(HOST_MAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_SCLC_DATA) $(HOST_FAULT) $(HOST_INVARIANTS) $(HOST_WIRE_COMPAT) $(HTTP_RFC_GEN) $(HOST_RX_BUDGET) $(HOST_CLIENT) $(HOST_CLIENT_ARGS) $(HOST_CLIENT_LOOPBACK) $(HOST_CLIENT_WIRE) $(HOST_CLIENT_RX)
	@$(HOST_TEST)
	@$(HOST_FUZZ)
	@$(HOST_HTTP)
	@$(HOST_HTTP_FUZZ)
	@$(HOST_CRYPTO)
	@$(HOST_REORDER)
	@$(HOST_ACK)
	@$(HOST_CONTROL)
	@$(HOST_TX_PUBLISH)
	@$(HOST_TX_INDEX)
	@$(HOST_FIN_PRIORITY)
	@$(HOST_CLOSE_WAKE)
	@$(HOST_HOLD)
	@$(HOST_MULTISTREAM)
	@$(HOST_ANCHOR)
	@$(HOST_FEC)
	@$(HOST_FIN_TAIL)
	@$(HOST_CELL_TAIL)
	@$(HOST_MEM_BUDGET)
	@$(HOST_FEC_RX)
	@$(HOST_FEC_SWITCH)
	@$(HOST_FEC_OUT)
	@$(HOST_CODEC_CACHE)
	@$(HOST_BATCH)
	@$(HOST_FEC_ADAPT)
	@$(HOST_FEC_ADAPT_SCLC)
	@$(HOST_FEC_SCLC)
	@$(HOST_FEC_SCLC_SHAPES)
	@$(HOST_PORTMAP)
	@$(HOST_MAP)
	@$(HOST_CARRIER_POLICY)
	@$(HOST_LIFETIME)
	@$(HOST_SCLC)
	@$(HOST_SCLC_DATA)
	@$(HOST_FAULT)
	@$(HOST_INVARIANTS)
	@$(HOST_WIRE_COMPAT)
	@$(HOST_RX_BUDGET)
	@$(HOST_CAPTURE_POKE)
	@$(HOST_LOCAL_QUEUE)
	@$(HOST_CLIENT_ARGS)
	@$(HOST_CLIENT_LOOPBACK)
	@$(HOST_CLIENT_WIRE)
	@$(HOST_CLIENT_RX)
	@# Third-party cross-check: h11 is not a build dependency. uv with a cached
	@# h11 runs it offline; anything else gets a clear skip, not a failure.
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python $(HOST_HTTP_H11) $(HOST_HTTP); \
	else \
		echo "host-test: h11 cross-check skipped (uv or cached h11 unavailable)"; \
	fi
	@# The RFC 9112 audit drives tbt_http.c over many keep-alive cycles. uv
	@# supplies h11 when its offline cache has it; the strict state machine in
	@# the audit runs either way.
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python $(HTTP_RFC_AUDIT) $(HTTP_RFC_GEN); \
	else \
		echo "host-test: http-rfc9112 audit runs without h11 (uv or cached h11 unavailable)"; \
		python3 $(HTTP_RFC_AUDIT) $(HTTP_RFC_GEN); \
	fi

# Boot a kernel in QEMU and run the module there: the only way to exercise a
# kernel module without root on this host. The build goes through `check`'s
# script so the header payload is found the same way; the guest kernel comes
# from ~/.cache/tbtun-build (tests/qemu/README.md).
qemu-test:
	@if [ -x ./tools/build.sh ]; then ./tools/build.sh > /dev/null; else \
		$(MAKE) --no-print-directory all; fi
	@tests/qemu/run.sh --ko $(abspath $(BUILD))/kernel/tbtun.ko

$(HOST_TEST): $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_TEST_SRCS)

$(HOST_FUZZ): $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FUZZ_SRCS)

$(HOST_HTTP): $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_HTTP_SRCS)

$(HOST_HTTP_FUZZ): $(HOST_HTTP_FUZZ_SRCS) $(HOST_HTTP_FUZZ_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_HTTP_FUZZ_SRCS)

$(HTTP_RFC_GEN): $(HTTP_RFC_GEN_SRCS) $(HTTP_RFC_GEN_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HTTP_RFC_GEN_SRCS)

$(HOST_CRYPTO): $(HOST_CRYPTO_SRCS) $(HOST_CRYPTO_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CRYPTO_SRCS)

$(HOST_BENCH_CRYPTO): $(HOST_BENCH_CRYPTO_SRCS) $(HOST_BENCH_CRYPTO_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) -std=gnu99 -O3 -Wall -Wextra -Werror -I src/kernel -o $@ $(HOST_BENCH_CRYPTO_SRCS)

# Throughput and cycles/byte for the host port. Not part of `check`: it is a
# measurement, not a gate.
host-bench-crypto: $(HOST_BENCH_CRYPTO)
	@$(HOST_BENCH_CRYPTO)

# The microbenchmark base: one binary, CSV on stdout. -O2 is the module's
# default for tbt_fec.c; the crypto object is -O3 in the module, so its rows
# here are a lower bound (build/bin/bench_crypto measures that core at -O3).
$(HOST_BENCH_MICRO): $(HOST_BENCH_MICRO_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_MICRO_SRCS)

host-bench-micro: $(HOST_BENCH_MICRO)

# Per-record FEC TX cost: queue allocation, HTTP chunk framing, and the
# buffer-reuse alternative. Not part of `check`; it is a measurement.
$(HOST_BENCH_TXPATH): $(HOST_BENCH_TXPATH_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_TXPATH_SRCS)

host-bench-txpath: $(HOST_BENCH_TXPATH)

# Per-record RX chain cost (HTTP deframe, header/payload CRC, ChaCha8, FEC
# placement, ordered delivery). Not part of `check`: a measurement.
$(HOST_BENCH_RXPATH): $(HOST_BENCH_RXPATH_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_RXPATH_SRCS)

host-bench-rxpath: $(HOST_BENCH_RXPATH)
	@$(HOST_BENCH_RXPATH)

# The feedback loop. `bench` is the <= 2 s pass for iteration; `bench-full`
# is the <= 10 s pass for a recorded baseline. Both write a timestamped
# directory under tests/throughput/results/ and print its path.
bench: $(HOST_BENCH_MICRO)
	@./tools/microbench.sh run --quick

bench-full: $(HOST_BENCH_MICRO)
	@./tools/microbench.sh run --full

$(HOST_REORDER): $(HOST_REORDER_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_REORDER_SRCS)

$(HOST_ACK): $(HOST_ACK_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_ACK_SRCS)

$(HOST_CONTROL): $(HOST_CONTROL_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_CONTROL_SRCS)

$(HOST_TX_PUBLISH): $(HOST_TX_PUBLISH_SRCS) $(HOST_TX_PUBLISH_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_TX_PUBLISH_SRCS)

$(HOST_TX_INDEX): $(HOST_TX_INDEX_SRCS) $(HOST_TX_INDEX_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_TX_INDEX_SRCS)

$(HOST_FIN_PRIORITY): $(HOST_FIN_PRIORITY_SRCS) $(HOST_FIN_PRIORITY_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FIN_PRIORITY_SRCS)

$(HOST_CLOSE_WAKE): $(HOST_CLOSE_WAKE_SRCS) $(HOST_CLOSE_WAKE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CLOSE_WAKE_SRCS)

$(HOST_HOLD): $(HOST_HOLD_SRCS) $(HOST_HOLD_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_HOLD_SRCS)

$(HOST_MULTISTREAM): $(HOST_MULTISTREAM_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_MULTISTREAM_SRCS)

$(HOST_ANCHOR): $(HOST_ANCHOR_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_ANCHOR_SRCS)

$(HOST_FEC): $(HOST_FEC_SRCS) $(HOST_FEC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SRCS)

$(HOST_FIN_TAIL): $(HOST_FIN_TAIL_SRCS) $(HOST_FIN_TAIL_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FIN_TAIL_SRCS)

$(HOST_CELL_TAIL): $(HOST_CELL_TAIL_SRCS) $(HOST_CELL_TAIL_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CELL_TAIL_SRCS)

$(HOST_MEM_BUDGET): $(HOST_MEM_BUDGET_SRCS) $(HOST_MEM_BUDGET_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_MEM_BUDGET_SRCS)
$(HOST_FEC_RX): $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_RX_SRCS)

$(HOST_FEC_SWITCH): $(HOST_FEC_SWITCH_SRCS) $(HOST_FEC_SWITCH_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SWITCH_SRCS)

$(HOST_FEC_OUT): $(HOST_FEC_OUT_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

$(HOST_CODEC_CACHE): $(HOST_CODEC_CACHE_SRCS) $(HOST_CODEC_CACHE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CODEC_CACHE_SRCS)

$(HOST_BATCH): $(HOST_BATCH_SRCS) $(HOST_BATCH_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BATCH_SRCS)

$(HOST_FEC_ADAPT): $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_ADAPT_SRCS)

$(HOST_FEC_ADAPT_SCLC): $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

$(HOST_FEC_SCLC): $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SCLC_SRCS)

$(HOST_FEC_SCLC_SHAPES): $(HOST_FEC_SCLC_SHAPES_SRCS) $(HOST_FEC_SCLC_SHAPES_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SCLC_SHAPES_SRCS)

$(HOST_PORTMAP): $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_PORTMAP_SRCS)

$(HOST_MAP): $(HOST_MAP_SRCS) $(HOST_MAP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_MAP_SRCS)

$(HOST_CARRIER_POLICY): tests/unit/test_carrier_policy.c $(HOST_CARRIER_POLICY_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ tests/unit/test_carrier_policy.c

$(HOST_LIFETIME): $(HOST_LIFETIME_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

$(HOST_SCLC): $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_SCLC_SRCS)

$(HOST_SCLC_DATA): $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_FAULT): $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_FAULT_SRCS)

$(HOST_INVARIANTS): $(HOST_INVARIANTS_SRCS) $(HOST_INVARIANTS_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_INVARIANTS_SRCS)

$(HOST_WIRE_COMPAT): $(HOST_WIRE_COMPAT_SRCS) $(HOST_WIRE_COMPAT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_WIRE_COMPAT_SRCS)

$(HOST_RX_BUDGET): $(HOST_RX_BUDGET_SRCS) $(HOST_RX_BUDGET_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_RX_BUDGET_SRCS)

$(HOST_CAPTURE_POKE): tests/unit/test_capture_poke.c $(HOST_CAPTURE_POKE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ tests/unit/test_capture_poke.c

$(HOST_LOCAL_QUEUE): tests/unit/test_local_queue.c $(HOST_LOCAL_QUEUE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ tests/unit/test_local_queue.c

$(BINDIR)/test_fec_sclc_data_san: $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_BENCH_SCLC): $(HOST_BENCH_SCLC_SRCS) $(HOST_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_SCLC_SRCS)

$(HOST_BENCH_SCLC_DOWN): $(HOST_BENCH_SCLC_DOWN_SRCS) $(HOST_BENCH_SCLC_DOWN_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_SCLC_DOWN_SRCS)

host-bench-sclc: $(HOST_BENCH_SCLC)
	@$(HOST_BENCH_SCLC) 8 8 24 1 4 0.3
	@$(HOST_BENCH_SCLC) 8 8 24 1 4 0.4

# Paired recovery comparison across the six uplink rate splits. A
# measurement, not a gate: the same block and the same share-loss mask are
# decoded with the legacy and the bit-plane mapping.
$(HOST_BENCH_RECOVERY): $(HOST_BENCH_RECOVERY_SRCS) $(HOST_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_RECOVERY_SRCS)

host-bench-recovery: $(HOST_BENCH_RECOVERY)
	@$(HOST_BENCH_RECOVERY)

host-bench-sclc-down: $(HOST_BENCH_SCLC_DOWN)
	@$(HOST_BENCH_SCLC_DOWN)

$(HOST_CALIB_SCLC_DOWN): $(HOST_CALIB_SCLC_DOWN_SRCS) $(HOST_CALIB_SCLC_DOWN_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CALIB_SCLC_DOWN_SRCS)

.PHONY: host-calib-sclc-down
# Recovery calibration of the shipped downlink: failure rate against share
# loss, the 0 ms wake race, burst and dead-carrier models. It is a
# measurement, not a gate; the full matrix is a shell loop documented in
# tests/calib/sclc-down.md. The smoke run only proves the binary works.
host-calib-sclc-down: $(HOST_CALIB_SCLC_DOWN)
	@$(HOST_CALIB_SCLC_DOWN) --header
	@$(HOST_CALIB_SCLC_DOWN) iid 50 300000 200 20261111
	@$(HOST_CALIB_SCLC_DOWN) first 70 100000 200 20261111
	@$(HOST_CALIB_SCLC_DOWN) ge 60 200000 200 20261111 4
	@$(HOST_CALIB_SCLC_DOWN) dead 50 0 200 20261111 16

$(HOST_CALIB_SCLC_DE): $(HOST_CALIB_SCLC_DE_SRCS) $(HOST_CALIB_SCLC_DE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CALIB_SCLC_DE_SRCS)

.PHONY: host-calib-sclc-de
# Deterministic BEC density evolution for every constructible n, plus the
# constructor's own accept range. No random input, no timing.
host-calib-sclc-de: $(HOST_CALIB_SCLC_DE)
	@$(HOST_CALIB_SCLC_DE) --range
	@$(HOST_CALIB_SCLC_DE)

# Full-chain production shapes: FEC, header CRCs and ChaCha8 for both
# directions, at the report loss points. Not part of `check`.
$(HOST_BENCH_SCLC_PIPE): $(HOST_BENCH_SCLC_PIPE_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_SCLC_PIPE_SRCS)

host-bench-sclc-pipe: $(HOST_BENCH_SCLC_PIPE)
	@$(HOST_BENCH_SCLC_PIPE)

$(HOST_BENCH_SCLC_BUILD): $(HOST_BENCH_SCLC_BUILD_SRCS) $(HOST_BENCH_SCLC_BUILD_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_BENCH_SCLC_BUILD_SRCS)

# Codec build cost against the route-A gate: p99 <= 2 ms for the downlink
# binary codec and for the eight-group uplink GF(16) build.
host-bench-sclc-build: $(HOST_BENCH_SCLC_BUILD)
	@$(HOST_BENCH_SCLC_BUILD)
# Calibration of the uplink 8x32 GF(16) SC-LDPC ladder against the real
# codec. The binary is a measurement, not a gate, so it is not part of
# host-test. Six ladder points run in parallel; every sample seeds itself
# from (section, level, p), so the CSV does not depend on completion order.
$(HOST_CALIB_SCLC_UP): $(HOST_CALIB_SCLC_UP_SRCS) $(HOST_CALIB_SCLC_UP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CALIB_SCLC_UP_SRCS) -lm

host-calib-sclc-up: $(HOST_CALIB_SCLC_UP)

CALIB_LEVELS := 25 30 40 50 60 70
CALIB_DIR := tests/calib
CALIB_PARTS := $(CALIB_DIR)/.parts

calib-sclc-up: $(HOST_CALIB_SCLC_UP)
	@mkdir -p $(CALIB_PARTS)
	@$(HOST_CALIB_SCLC_UP) header > $(CALIB_PARTS)/00-header.csv
	@$(HOST_CALIB_SCLC_UP) construct > $(CALIB_PARTS)/01-construct.csv
	@set -e; for pct in $(CALIB_LEVELS); do \
	  $(HOST_CALIB_SCLC_UP) analyze --level $$pct > $(CALIB_PARTS)/02-analyze-$$pct.csv & \
	  $(HOST_CALIB_SCLC_UP) spot --level $$pct --trials 1000 > $(CALIB_PARTS)/03-spot-$$pct.csv & \
	done; wait
	@$(HOST_CALIB_SCLC_UP) spot --level 70 --trials 1000 --core 512 > $(CALIB_PARTS)/04-spot-core512-70.csv
	@cat $(CALIB_PARTS)/*.csv > $(CALIB_DIR)/sclc-up.csv
	@rm -rf $(CALIB_PARTS)
	@echo "calib-sclc-up: wrote $(CALIB_DIR)/sclc-up.csv"

# The same tests with ASan and UBSan. A toolchain without the runtimes gets
# a clear skip, not a build failure.
host-test-san: $(BINDIR)/test_capture_poke_san
	@if printf 'int main(void){return 0;}\n' | $(HOSTCC) $(SAN_CFLAGS) -x c - -o /dev/null 2>/dev/null; then \
		$(MAKE) --no-print-directory host-test-san-run; \
	else \
		echo "host-test-san: skipped, $(HOSTCC) cannot link -fsanitize=address,undefined"; \
	fi

host-test-san-run: $(BINDIR)/test_parse_san $(BINDIR)/test_fuzz_san $(BINDIR)/test_http_san $(BINDIR)/test_http_fuzz_san $(BINDIR)/test_crypto_san $(BINDIR)/test_fec_san $(BINDIR)/test_fin_tail_san $(BINDIR)/test_cell_tail_san $(BINDIR)/test_mem_budget_san $(BINDIR)/test_fec_rx_san $(BINDIR)/test_fec_switch_san $(BINDIR)/test_fec_out_san $(BINDIR)/test_batch_san $(BINDIR)/test_fec_adapt_san $(BINDIR)/test_fec_adapt_sclc_san $(BINDIR)/test_fec_sclc_san $(BINDIR)/test_fec_sclc_shapes_san $(BINDIR)/test_close_wake_san $(BINDIR)/test_hold_san $(BINDIR)/test_portmap_san $(BINDIR)/test_map_san $(BINDIR)/test_lifetime_san $(BINDIR)/test_sclc_uplink_san $(BINDIR)/test_fec_sclc_data_san $(BINDIR)/test_fault_san $(BINDIR)/test_invariants_san $(BINDIR)/test_wire_compat_san $(BINDIR)/test_rx_budget_san $(BINDIR)/test_local_queue_san $(BINDIR)/test_client_loopback_san $(BINDIR)/test_client_wire_san $(BINDIR)/test_client_rx_san $(HTTP_RFC_GEN_SAN)
	@$(BINDIR)/test_parse_san
	@$(BINDIR)/test_fuzz_san
	@$(BINDIR)/test_http_san
	@$(BINDIR)/test_http_fuzz_san
	@$(BINDIR)/test_crypto_san
	@$(BINDIR)/test_fec_san
	@$(BINDIR)/test_fin_tail_san
	@$(BINDIR)/test_cell_tail_san
	@$(BINDIR)/test_mem_budget_san
	@$(BINDIR)/test_fec_rx_san
	@$(BINDIR)/test_fec_switch_san
	@$(BINDIR)/test_fec_out_san
	@$(BINDIR)/test_batch_san
	@$(BINDIR)/test_fec_adapt_san
	@$(BINDIR)/test_fec_adapt_sclc_san
	@$(BINDIR)/test_fec_sclc_san
	@$(BINDIR)/test_fec_sclc_shapes_san
	@$(BINDIR)/test_hold_san
	@$(BINDIR)/test_portmap_san
	@$(BINDIR)/test_map_san
	@$(BINDIR)/test_carrier_policy_san
	@$(BINDIR)/test_lifetime_san
	@$(BINDIR)/test_sclc_uplink_san
	@$(BINDIR)/test_fec_sclc_data_san
	@$(BINDIR)/test_close_wake_san
	@$(BINDIR)/test_fault_san
	@$(BINDIR)/test_invariants_san
	@$(BINDIR)/test_wire_compat_san
	@$(BINDIR)/test_rx_budget_san
	@$(BINDIR)/test_capture_poke_san
	@$(BINDIR)/test_local_queue_san
	@$(BINDIR)/test_client_loopback_san
	@$(BINDIR)/test_client_wire_san
	@$(BINDIR)/test_client_rx_san
	@python3 $(HTTP_RFC_AUDIT) $(HTTP_RFC_GEN_SAN)

$(BINDIR)/test_parse_san: $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_TEST_SRCS)

$(BINDIR)/test_fuzz_san: $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FUZZ_SRCS)

$(BINDIR)/test_http_san: $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_HTTP_SRCS)

$(BINDIR)/test_http_fuzz_san: $(HOST_HTTP_FUZZ_SRCS) $(HOST_HTTP_FUZZ_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_HTTP_FUZZ_SRCS)

$(HTTP_RFC_GEN_SAN): $(HTTP_RFC_GEN_SRCS) $(HTTP_RFC_GEN_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HTTP_RFC_GEN_SRCS)

$(BINDIR)/test_crypto_san: $(HOST_CRYPTO_SRCS) $(HOST_CRYPTO_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_CRYPTO_SRCS)

$(BINDIR)/test_fec_san: $(HOST_FEC_SRCS) $(HOST_FEC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SRCS)

$(BINDIR)/test_fin_tail_san: $(HOST_FIN_TAIL_SRCS) $(HOST_FIN_TAIL_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FIN_TAIL_SRCS)

$(BINDIR)/test_cell_tail_san: $(HOST_CELL_TAIL_SRCS) $(HOST_CELL_TAIL_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_CELL_TAIL_SRCS)

$(BINDIR)/test_mem_budget_san: $(HOST_MEM_BUDGET_SRCS) $(HOST_MEM_BUDGET_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_MEM_BUDGET_SRCS)
$(BINDIR)/test_close_wake_san: $(HOST_CLOSE_WAKE_SRCS) $(HOST_CLOSE_WAKE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_CLOSE_WAKE_SRCS)

$(BINDIR)/test_hold_san: $(HOST_HOLD_SRCS) $(HOST_HOLD_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_HOLD_SRCS)

$(BINDIR)/test_fec_rx_san: $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_RX_SRCS)

$(BINDIR)/test_fec_switch_san: $(HOST_FEC_SWITCH_SRCS) $(HOST_FEC_SWITCH_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SWITCH_SRCS)

$(BINDIR)/test_fec_out_san: $(HOST_FEC_OUT_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

$(BINDIR)/test_batch_san: $(HOST_BATCH_SRCS) $(HOST_BATCH_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_BATCH_SRCS)

$(BINDIR)/test_fec_adapt_san: $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_ADAPT_SRCS)

$(BINDIR)/test_fec_adapt_sclc_san: $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

$(BINDIR)/test_fec_sclc_san: $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SCLC_SRCS)

$(BINDIR)/test_fec_sclc_shapes_san: $(HOST_FEC_SCLC_SHAPES_SRCS) $(HOST_FEC_SCLC_SHAPES_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FEC_SCLC_SHAPES_SRCS)

$(BINDIR)/test_portmap_san: $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_PORTMAP_SRCS)

$(BINDIR)/test_map_san: $(HOST_MAP_SRCS) $(HOST_MAP_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_MAP_SRCS)

$(BINDIR)/test_carrier_policy_san: tests/unit/test_carrier_policy.c $(HOST_CARRIER_POLICY_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ tests/unit/test_carrier_policy.c

$(BINDIR)/test_lifetime_san: $(HOST_LIFETIME_SRCS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

$(BINDIR)/test_sclc_uplink_san: $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_SCLC_SRCS)

$(BINDIR)/test_fault_san: $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_FAULT_SRCS)

$(BINDIR)/test_invariants_san: $(HOST_INVARIANTS_SRCS) $(HOST_INVARIANTS_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_INVARIANTS_SRCS)

$(BINDIR)/test_wire_compat_san: $(HOST_WIRE_COMPAT_SRCS) $(HOST_WIRE_COMPAT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_WIRE_COMPAT_SRCS)

$(BINDIR)/test_rx_budget_san: $(HOST_RX_BUDGET_SRCS) $(HOST_RX_BUDGET_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_RX_BUDGET_SRCS)

$(BINDIR)/test_capture_poke_san: tests/unit/test_capture_poke.c $(HOST_CAPTURE_POKE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ tests/unit/test_capture_poke.c

$(BINDIR)/test_local_queue_san: tests/unit/test_local_queue.c $(HOST_LOCAL_QUEUE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ tests/unit/test_local_queue.c

# The concurrent host contracts under ThreadSanitizer. Opt-in, like
# host-test-san: `check` stays free of a sanitizer runtime. A toolchain
# without the TSan runtime gets a clear skip, not a build failure. The test
# itself refuses to build without -fsanitize=thread, so a skipped runtime
# cannot masquerade as "no race found".
host-test-tsan:
	@if printf 'int main(void){return 0;}\n' | $(HOSTCC) $(TSAN_CFLAGS) -pthread -x c - -o /dev/null 2>/dev/null; then \
		$(MAKE) --no-print-directory host-test-tsan-run; \
	else \
		echo "host-test-tsan: skipped, $(HOSTCC) cannot link -fsanitize=thread"; \
	fi

host-test-tsan-run: $(HOST_TSAN)
	@TSAN_OPTIONS=halt_on_error=1:exitcode=66 $(HOST_TSAN)

$(HOST_TSAN): $(HOST_TSAN_SRCS) $(HOST_TSAN_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(TSAN_CFLAGS) -pthread -I src/kernel -o $@ $(HOST_TSAN_SRCS)

$(HOST_HTTP_LIVE): $(HOST_HTTP_LIVE_SRCS) $(HOST_HTTP_LIVE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_HTTP_LIVE_SRCS)

# A real capture is an external input, so this target is not part of check or
# host-test. The proc text format and the binary framing are documented in
# docs/http-profile-integration.md ("Capture format (frozen)").
host-test-live: $(HOST_HTTP_LIVE)
	@if [ -z "$(FILE)" ]; then \
		echo "usage: make host-test-live FILE=capture.bin"; exit 2; \
	fi
	@$(HOST_HTTP_LIVE) < "$(FILE)"
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python tests/unit/test_http_live_h11.py "$(FILE)"; \
	else \
		echo "host-test-live: h11 oracle skipped (uv or cached h11 unavailable)"; \
	fi

clean:
	-$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR)/src/kernel MO=$(abspath $(BUILD))/kernel $(KBUILD_LLVM) clean
	$(RM) -r $(BUILD)
	$(RM) dkms.conf $(DKMS_TARBALL)

load:
	sudo insmod $(abspath $(BUILD))/kernel/tbtun.ko

unload:
	sudo rmmod tbtun

.PHONY: format format-check
format:
	$(CLANG_FORMAT) --style=file -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --style=file --dry-run --Werror $(FORMAT_SRCS)

.PHONY: dkms-tarball dkms-srcs-check clean-dkms-tarball clean-dkms.conf

.always.make:

# dkms.conf is generated. The version comes only from src/kernel/tbt_uapi.h via
# tools/mkdkmsconf.sh; do not edit dkms.conf by hand.
dkms.conf: ./tools/mkdkmsconf.sh src/kernel/tbt_uapi.h .always.make
	./tools/mkdkmsconf.sh > dkms.conf

clean-dkms.conf:
	$(RM) dkms.conf

# A DKMS tarball must be buildable on a machine that has never seen this tree,
# so every module source has to be in SRCS. Catch a new file that was not added
# before the archive is written, not on the target machine. The same gate
# rejects a stale SRCS entry and a staging file that lost its SRCS entry.
dkms-srcs-check:
	@missing=$$(find src/kernel \( -name '*.c' -o -name '*.h' \) | sort | while IFS= read -r f; do \
		case " $(SRCS) " in *" $$f "*) ;; *) printf '%s\n' "$$f" ;; esac; done); \
	if [ -n "$$missing" ]; then \
		echo "dkms-srcs-check: SRCS omits:" >&2; \
		printf '%s\n' "$$missing" >&2; \
		exit 1; \
	fi; \
	absent=$$(for f in $(SRCS); do [ -e "$$f" ] || printf '%s\n' "$$f"; done); \
	if [ -n "$$absent" ]; then \
		echo "dkms-srcs-check: SRCS names missing files:" >&2; \
		printf '%s\n' "$$absent" >&2; \
		exit 1; \
	fi; \
	unlisted=$$(for f in $(DKMS_STAGE_FILES); do \
		case " $(SRCS) " in *" $$f "*) ;; *) printf '%s\n' "$$f" ;; esac; done); \
	if [ -n "$$unlisted" ]; then \
		echo "dkms-srcs-check: SRCS omits a staged file:" >&2; \
		printf '%s\n' "$$unlisted" >&2; \
		exit 1; \
	fi

# tools/build.sh is a developer convenience (it may source an env file from
# the author's home); a DKMS build must never depend on it, so it is excluded.
$(DKMS_TARBALL): dkms.conf dkms-srcs-check $(SRCS)
	$(TAR) zcf $(DKMS_TARBALL) \
		--transform 's,^,./dkms_source_tree/,' \
		dkms.conf \
		$(SRCS)

dkms-tarball: $(DKMS_TARBALL)

clean-dkms-tarball:
	$(RM) $(DKMS_TARBALL)

$(OBJDIR)/client-%.o: src/kernel/%.c $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -c -o $@ $<

$(OBJDIR)/client-engine.o: src/client/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -c -o $@ $<

$(OBJDIR)/client-args.o: src/client/tbt_client_args.c src/client/tbt_client_args.h \
                   src/kernel/tbt_parse.h src/kernel/tbt_uapi.h src/kernel/tbt_crypto.h
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -c -o $@ $<

$(OBJDIR)/client-main.o: src/client/tbt_client_main.c src/client/tbt_client_args.h \
                   $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -c -o $@ $<

$(HOST_CLIENT_LIB): $(HOST_CLIENT_OBJS)
	@mkdir -p $(@D) && $(AR) rcs $@ $^

$(HOST_CLIENT): $(OBJDIR)/client-main.o $(HOST_CLIENT_LIB)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -o $@ $(OBJDIR)/client-main.o $(HOST_CLIENT_LIB)

$(HOST_CLIENT_LOOPBACK): tests/unit/test_client_loopback.c $(HOST_CLIENT_LIB) $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ tests/unit/test_client_loopback.c $(HOST_CLIENT_LIB)

$(HOST_CLIENT_ARGS): $(HOST_CLIENT_ARGS_SRCS) $(HOST_CLIENT_ARGS_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CLIENT_ARGS_SRCS)

$(BINDIR)/test_client_loopback_san: tests/unit/test_client_loopback.c $(HOST_CLIENT_LIB_SRCS) $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -DTBT_LOOPBACK_MB=1 -I src/kernel -o $@ tests/unit/test_client_loopback.c $(HOST_CLIENT_LIB_SRCS)

$(HOST_CLIENT_WIRE): $(HOST_CLIENT_WIRE_SRCS) $(HOST_CLIENT_WIRE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -I src/kernel -o $@ $(HOST_CLIENT_WIRE_SRCS)

$(BINDIR)/test_client_wire_san: $(HOST_CLIENT_WIRE_SRCS) $(HOST_CLIENT_WIRE_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I src/kernel -o $@ $(HOST_CLIENT_WIRE_SRCS)

$(HOST_CLIENT_RX): $(HOST_CLIENT_RX_SRCS) src/client/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) -Wno-unused-function -Wno-unused-variable -I src/kernel -I src/client -o $@ $(HOST_CLIENT_RX_SRCS)

$(BINDIR)/test_client_rx_san: $(HOST_CLIENT_RX_SRCS) src/client/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	@mkdir -p $(@D) && $(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -Wno-unused-function -Wno-unused-variable -I src/kernel -I src/client -o $@ $(HOST_CLIENT_RX_SRCS)
