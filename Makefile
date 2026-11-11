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
                   tbtun/tbt_rec.h tbtun/tbt_http.h tbtun/tbt_fec_batch.h \
                   tbtun/tbt_crypto.h tbtun/tbt_mux.h \
                   tbtun/tbt_fec.h tbtun/tbt_fec_late.h tbtun/tbt_fec_adapt.h \
                   tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_sclc_delta.h \
                   tbtun/tbt_portmap.h tbtun/tbt_map.h tbtun/tbt_carrier_policy.h \
                   tbtun/tbt_tx_acct.h \
                   tbtun/tbt_rx_budget.h \
                   tbtun/tbt_core.c tbtun/tbt_session.c tbtun/tbt_procfs.c tbtun/tbt_listen.c \
                   tbtun/tbt_carrier.c tbtun/tbt_cc.c tbtun/tbt_cc.h tbtun/tbt_sock.c \
                   tbtun/tbt_tx.c tbtun/tbt_rx.c \
                   tbtun/tbt_parse.c tbtun/tbt_proto.c tbtun/tbt_rec.c tbtun/tbt_http.c \
                   tbtun/tbt_fec_batch.c \
                   tbtun/tbt_crypto.c tbtun/tbt_fec.c \
                   tbtun/tbt_fec_data.c tbtun/tbt_fec_adapt.c \
                   tbtun/tbt_fec_adapt_sclc.c tbtun/tbt_fec_sclc.c \
                   tbtun/tbt_fec_sclc.h tbtun/tbt_fec_codec_cache.c \
                   tbtun/tbt_fec_codec_cache.h \
                   tbtun/tbt_portmap.c tbtun/tbt_map.c \
                   tbtun/tbt_sc_ldpc.c tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_sc_ldpc.h tbtun/tbt_sc_ldpc_gf16.h tbtun/tbt_sclc_uplink.h \
                   tbtun/tbt_mux.c \
                   scripts/mkdkmsconf.sh scripts/install_dkms.sh \
                   .clang-format
# scripts/install_dkms.sh stages these beside tbtun/*.c and *.h. A staged
# tree cannot configure itself if one of them leaves SRCS.
DKMS_STAGE_FILES := Makefile .clang-format scripts/mkdkmsconf.sh scripts/install_dkms.sh
FORMAT_SRCS     := $(filter %.c %.h,$(SRCS))

HOSTCC          ?= cc
HOST_CFLAGS     ?= -std=gnu99 -O2 -Wall -Wextra -Werror
HOST_TEST       := host/test_parse
HOST_TEST_SRCS  := host/test_parse.c tbtun/tbt_parse.c tbtun/tbt_fec_adapt_sclc.c \
                   tbtun/tbt_portmap.c tbtun/tbt_map.c
HOST_TEST_HDRS  := tbtun/tbt_parse.h tbtun/tbt_portmap.h tbtun/tbt_map.h tbtun/tbt_uapi.h \
                   tbtun/tbt_crypto.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_adapt.h tbtun/tbt_fec.h
HOST_FUZZ       := host/test_fuzz
HOST_FUZZ_SRCS  := host/test_fuzz.c tbtun/tbt_rec.c tbtun/tbt_proto.c \
                   tbtun/tbt_fec.c tbtun/tbt_parse.c \
                   tbtun/tbt_fec_adapt_sclc.c tbtun/tbt_portmap.c
HOST_FUZZ_HDRS  := tbtun/tbt_rec.h tbtun/tbt_proto.h tbtun/tbt_uapi.h \
                   tbtun/tbt_fec.h tbtun/tbt_parse.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_adapt.h tbtun/tbt_portmap.h
HOST_HTTP       := host/test_http
HOST_HTTP_SRCS  := host/test_http.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_HDRS  := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
HOST_HTTP_H11   := host/test_http_h11.py
HOST_HTTP_FUZZ  := host/test_http_fuzz
HOST_HTTP_FUZZ_SRCS := host/test_http_fuzz.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_HTTP_FUZZ_HDRS := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h tbtun/tbt_rec.h
HTTP_RFC_GEN    := tests/http_rfc9112_gen
HTTP_RFC_GEN_SRCS := tests/http_rfc9112_gen.c tbtun/tbt_http.c tbtun/tbt_proto.c \
                   tbtun/tbt_rec.c tbtun/tbt_fec.c
HTTP_RFC_GEN_HDRS := tbtun/tbt_http.h tbtun/tbt_uapi.h tbtun/tbt_proto.h \
                   tbtun/tbt_rec.h tbtun/tbt_fec.h
HTTP_RFC_GEN_SAN := tests/http_rfc9112_gen_san
HTTP_RFC_AUDIT  := tests/http_rfc9112_audit.py
HOST_CRYPTO     := host/test_crypto
HOST_CRYPTO_SRCS := host/test_crypto.c tbtun/tbt_crypto.c tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_CRYPTO_HDRS := tbtun/tbt_crypto.h tbtun/tbt_http.h tbtun/tbt_proto.h tbtun/tbt_rec.h tbtun/tbt_uapi.h
HOST_BENCH_CRYPTO := host/bench_crypto
HOST_BENCH_CRYPTO_SRCS := host/bench_crypto.c tbtun/tbt_crypto.c tbtun/tbt_proto.c
HOST_BENCH_CRYPTO_HDRS := tbtun/tbt_crypto.h tbtun/tbt_proto.h tbtun/tbt_uapi.h
HOST_BENCH_MICRO := host/bench_micro
HOST_BENCH_MICRO_SRCS := host/bench_micro.c tbtun/tbt_fec.c tbtun/tbt_crypto.c tbtun/tbt_proto.c
HOST_BENCH_TXPATH := host/bench_txpath
HOST_BENCH_TXPATH_SRCS := host/bench_txpath.c tbtun/tbt_fec_batch.c \
                   tbtun/tbt_http.c tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_BENCH_RXPATH := host/bench_rx_stages
HOST_BENCH_RXPATH_SRCS := host/bench_rx_stages.c tbtun/tbt_http.c \
                   tbtun/tbt_fec.c tbtun/tbt_crypto.c tbtun/tbt_proto.c \
                   tbtun/tbt_rec.c
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
HOST_FIN_TAIL   := host/test_fin_tail
HOST_FIN_TAIL_SRCS := host/test_fin_tail.c
HOST_FIN_TAIL_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_CELL_TAIL  := host/test_cell_tail
HOST_CELL_TAIL_SRCS := host/test_cell_tail.c
HOST_CELL_TAIL_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_MEM_BUDGET := host/test_mem_budget
HOST_MEM_BUDGET_SRCS := host/test_mem_budget.c tbtun/tbt_fec.c
HOST_MEM_BUDGET_HDRS := tbtun/tbt_fec.h tbtun/tbt_http.h tbtun/tbt_rec.h \
                   tbtun/tbt_proto.h tbtun/tbt_uapi.h
HOST_FEC_RX     := host/test_fec_rx
HOST_FEC_RX_SRCS := host/test_fec_rx.c tbtun/tbt_fec.c tbtun/tbt_crypto.c
HOST_FEC_RX_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h tbtun/tbt_crypto.h
HOST_FEC_SWITCH := host/test_fec_switch
HOST_FEC_SWITCH_SRCS := host/test_fec_switch.c tbtun/tbt_fec.c
HOST_FEC_SWITCH_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_OUT    := host/test_fec_out
HOST_FEC_OUT_SRCS := host/test_fec_out.c
HOST_CODEC_CACHE := host/test_fec_codec_cache
HOST_CODEC_CACHE_SRCS := host/test_fec_codec_cache.c \
                   tbtun/tbt_fec_codec_cache.c
HOST_CODEC_CACHE_HDRS := tbtun/tbt_fec_codec_cache.h tbtun/tbt_fec.h \
                   tbtun/tbt_uapi.h
HOST_BATCH      := host/test_batch
HOST_BATCH_SRCS := host/test_batch.c tbtun/tbt_fec_batch.c tbtun/tbt_http.c \
                   tbtun/tbt_proto.c tbtun/tbt_rec.c
HOST_BATCH_HDRS := tbtun/tbt_fec_batch.h tbtun/tbt_http.h tbtun/tbt_uapi.h \
                   tbtun/tbt_proto.h tbtun/tbt_rec.h
HOST_FEC_ADAPT  := host/test_fec_adapt
HOST_FEC_ADAPT_SRCS := host/test_fec_adapt.c tbtun/tbt_fec_adapt.c
HOST_FEC_ADAPT_HDRS := tbtun/tbt_fec_adapt.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_ADAPT_SCLC := host/test_fec_adapt_sclc
HOST_FEC_ADAPT_SCLC_SRCS := host/test_fec_adapt_sclc.c tbtun/tbt_fec_adapt_sclc.c
HOST_FEC_ADAPT_SCLC_HDRS := tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h \
                   tbtun/tbt_fec_sclc_delta.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_SCLC   := host/test_fec_sclc
HOST_FEC_SCLC_SRCS := host/test_fec_sclc.c tbtun/tbt_fec_sclc.c
HOST_FEC_SCLC_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_FEC_SCLC_SHAPES := host/test_fec_sclc_shapes
HOST_FEC_SCLC_SHAPES_SRCS := host/test_fec_sclc_shapes.c tbtun/tbt_fec_sclc.c \
                   tbtun/tbt_fec_adapt_sclc.c
HOST_FEC_SCLC_SHAPES_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_sclc_delta.h tbtun/tbt_fec_adapt.h \
                   tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_PORTMAP    := host/test_portmap
HOST_PORTMAP_SRCS := host/test_portmap.c tbtun/tbt_portmap.c tbtun/tbt_parse.c \
                     tbtun/tbt_fec_adapt_sclc.c tbtun/tbt_map.c
HOST_PORTMAP_HDRS := tbtun/tbt_portmap.h tbtun/tbt_parse.h tbtun/tbt_uapi.h \
                     tbtun/tbt_map.h tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h \
                     tbtun/tbt_fec.h
HOST_MAP        := host/test_map
HOST_MAP_SRCS   := host/test_map.c tbtun/tbt_map.c tbtun/tbt_parse.c \
                   tbtun/tbt_fec_adapt_sclc.c tbtun/tbt_portmap.c
HOST_MAP_HDRS   := tbtun/tbt_map.h tbtun/tbt_parse.h tbtun/tbt_uapi.h \
                   tbtun/tbt_proto.h
HOST_CARRIER_POLICY := host/test_carrier_policy
HOST_CARRIER_POLICY_HDRS := tbtun/tbt_carrier_policy.h tbtun/tbt_uapi.h
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
HOST_INVARIANTS := host/test_invariants
HOST_INVARIANTS_SRCS := host/test_invariants.c tbtun/tbt_fec.c \
                   tbtun/tbt_portmap.c tbtun/tbt_fec_adapt_sclc.c
HOST_INVARIANTS_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h tbtun/tbt_portmap.h \
                   tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h
HOST_WIRE_COMPAT := host/test_wire_compat
HOST_WIRE_COMPAT_SRCS := host/test_wire_compat.c tbtun/tbt_rec.c \
                   tbtun/tbt_proto.c tbtun/tbt_fec.c tbtun/tbt_crypto.c
HOST_WIRE_COMPAT_HDRS := tbtun/tbt_rec.h tbtun/tbt_proto.h tbtun/tbt_fec.h \
                   tbtun/tbt_crypto.h tbtun/tbt_uapi.h
HOST_RX_BUDGET  := host/test_rx_budget
HOST_RX_BUDGET_SRCS := host/test_rx_budget.c
HOST_RX_BUDGET_HDRS := tbtun/tbt_rx_budget.h tbtun/tbt_uapi.h
HOST_MUX        := host/test_mux
HOST_MUX_SRCS   := host/test_mux.c tbtun/tbt_mux.c
HOST_MUX_HDRS   := tbtun/tbt_mux.h
HOST_CAPTURE_POKE := host/test_capture_poke
HOST_CAPTURE_POKE_HDRS := tbtun/tbt_fec.h tbtun/tbt_uapi.h

# The userspace client links the module's pure C units and adds only the
# session/socket engine. The library is the reusable half: 0.8.x Android and
# Linux front ends link it, and the loopback test drives two engines.
HOST_CLIENT_LIB_SRCS := host/tbt_client_engine.c \
                   tbtun/tbt_parse.c tbtun/tbt_proto.c tbtun/tbt_rec.c \
                   tbtun/tbt_http.c tbtun/tbt_fec_batch.c \
                   tbtun/tbt_crypto.c tbtun/tbt_fec.c \
                   tbtun/tbt_fec_sclc.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_fec_adapt.c tbtun/tbt_fec_adapt_sclc.c \
                   tbtun/tbt_fec_codec_cache.c tbtun/tbt_portmap.c
HOST_CLIENT_HDRS := host/tbt_client_engine.h tbtun/tbt_parse.h \
                   tbtun/tbt_proto.h tbtun/tbt_rec.h tbtun/tbt_http.h \
                   tbtun/tbt_fec_batch.h tbtun/tbt_crypto.h tbtun/tbt_fec.h \
                   tbtun/tbt_fec_sclc.h tbtun/tbt_sc_ldpc.h \
                   tbtun/tbt_sc_ldpc_gf16.h tbtun/tbt_sclc_uplink.h \
                   tbtun/tbt_fec_adapt.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_codec_cache.h tbtun/tbt_portmap.h \
                   tbtun/tbt_uapi.h
HOST_CLIENT_OBJS := host/client-engine.o \
                   $(patsubst tbtun/%.c,host/client-%.o,$(filter tbtun/%.c,$(HOST_CLIENT_LIB_SRCS)))
HOST_CLIENT     := host/tbt_client
HOST_CLIENT_LIB := host/libtbtun_client.a
HOST_CLIENT_LOOPBACK := host/test_client_loopback
HOST_CLIENT_WIRE := host/test_client_wire
HOST_CLIENT_WIRE_SRCS := host/test_client_wire.c tbtun/tbt_proto.c \
                   tbtun/tbt_rec.c tbtun/tbt_fec.c tbtun/tbt_http.c
HOST_CLIENT_WIRE_HDRS := tbtun/tbt_fec.h tbtun/tbt_http.h tbtun/tbt_proto.h \
                   tbtun/tbt_rec.h tbtun/tbt_uapi.h
HOST_CLIENT_RX := host/test_client_rx
HOST_CLIENT_RX_SRCS := host/test_client_rx.c \
                   $(filter-out host/tbt_client_engine.c,$(HOST_CLIENT_LIB_SRCS))
GOLDEN_V080     := tests/golden_v080/gen_golden
GOLDEN_V080_SRCS := tests/golden_v080/gen_golden.c tbtun/tbt_fec.c \
                   tbtun/tbt_crypto.c tbtun/tbt_proto.c tbtun/tbt_http.c \
                   tbtun/tbt_rec.c tbtun/tbt_fec_sclc.c \
                   tbtun/tbt_sclc_uplink.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_mux.c
GOLDEN_V080_HDRS := tbtun/tbt_fec.h tbtun/tbt_crypto.h tbtun/tbt_proto.h \
                   tbtun/tbt_http.h tbtun/tbt_mux.h tbtun/tbt_uapi.h \
                   tbtun/tbt_fec_sclc.h tbtun/tbt_sclc_uplink.h
HOST_BENCH_SCLC := host/bench_sclc_uplink
HOST_BENCH_SCLC_SRCS := host/bench_sclc_uplink.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c
HOST_BENCH_RECOVERY := host/bench_sclc_recovery
HOST_BENCH_RECOVERY_SRCS := host/bench_sclc_recovery.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c
HOST_BENCH_SCLC_DOWN := host/bench_sclc_downlink
HOST_BENCH_SCLC_DOWN_SRCS := host/bench_sclc_downlink.c tbtun/tbt_fec_sclc.c
HOST_BENCH_SCLC_DOWN_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec.h \
                   tbtun/tbt_uapi.h
HOST_CALIB_SCLC_DOWN := host/calib_sclc_down
HOST_CALIB_SCLC_DOWN_SRCS := host/calib_sclc_down.c tbtun/tbt_fec_sclc.c
HOST_CALIB_SCLC_DOWN_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec.h \
                   tbtun/tbt_uapi.h
HOST_CALIB_SCLC_DE := host/calib_sclc_de
HOST_CALIB_SCLC_DE_SRCS := host/calib_sclc_de.c tbtun/tbt_fec_sclc.c
HOST_CALIB_SCLC_DE_HDRS := tbtun/tbt_fec_sclc.h tbtun/tbt_fec.h \
                   tbtun/tbt_uapi.h
HOST_BENCH_SCLC_SIM := host/sim_sclc_adaptive
HOST_BENCH_SCLC_SIM_SRCS := host/sim_sclc_adaptive.c tbtun/tbt_fec_sclc.c \
                   tbtun/tbt_fec_adapt_sclc.c
HOST_BENCH_SCLC_SIM_HDRS := tbtun/tbt_fec_sclc.h \
                   tbtun/tbt_fec_adapt_sclc.h tbtun/tbt_fec_adapt.h \
                   tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_BENCH_SCLC_PIPE := host/bench_sclc_pipe
HOST_BENCH_SCLC_PIPE_SRCS := host/bench_sclc_pipe.c tbtun/tbt_fec.c \
                   tbtun/tbt_fec_sclc.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_sc_ldpc.c tbtun/tbt_sc_ldpc_gf16.c \
                   tbtun/tbt_crypto.c tbtun/tbt_proto.c
HOST_BENCH_SCLC_BUILD := host/bench_sclc_build
HOST_BENCH_SCLC_BUILD_SRCS := host/bench_sclc_build.c tbtun/tbt_fec.c \
                   tbtun/tbt_fec_sclc.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_sc_ldpc.c tbtun/tbt_sc_ldpc_gf16.c
HOST_BENCH_SCLC_BUILD_HDRS := tbtun/tbt_fec.h tbtun/tbt_fec_sclc.h \
                   tbtun/tbt_sclc_uplink.h tbtun/tbt_sc_ldpc.h \
                   tbtun/tbt_sc_ldpc_gf16.h tbtun/tbt_uapi.h
HOST_CALIB_SCLC_UP := host/calib_sclc_up
HOST_CALIB_SCLC_UP_SRCS := host/calib_sclc_up.c tbtun/tbt_sc_ldpc.c \
                   tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_sclc_uplink.c \
                   tbtun/tbt_fec_adapt_sclc.c
HOST_CALIB_SCLC_UP_HDRS := tbtun/tbt_sc_ldpc.h tbtun/tbt_sc_ldpc_gf16.h \
                   tbtun/tbt_sclc_uplink.h tbtun/tbt_fec_adapt_sclc.h \
                   tbtun/tbt_fec_adapt.h tbtun/tbt_fec.h tbtun/tbt_uapi.h
HOST_TSAN       := host/test_tsan_race
HOST_TSAN_SRCS  := host/test_tsan_race.c tbtun/tbt_crypto.c
HOST_TSAN_HDRS  := tbtun/tbt_fec.h tbtun/tbt_crypto.h tbtun/tbt_uapi.h
SAN_CFLAGS      := -g -fsanitize=address,undefined -fno-omit-frame-pointer
# A sanitizer run must stop at the first undefined-behaviour report instead
# of continuing through the rest of the suite.
export UBSAN_OPTIONS ?= halt_on_error=1
TSAN_CFLAGS     := -g -fsanitize=thread -fno-omit-frame-pointer

obj-m           += tbtun.o
tbtun-objs      := tbtun/tbt_core.o tbtun/tbt_session.o tbtun/tbt_procfs.o \
                   tbtun/tbt_listen.o tbtun/tbt_proto.o tbtun/tbt_carrier.o tbtun/tbt_cc.o \
                   tbtun/tbt_sock.o tbtun/tbt_tx.o tbtun/tbt_rx.o tbtun/tbt_rec.o \
                   tbtun/tbt_parse.o tbtun/tbt_http.o tbtun/tbt_fec_batch.o tbtun/tbt_crypto.o \
                   tbtun/tbt_portmap.o tbtun/tbt_map.o tbtun/tbt_fec.o tbtun/tbt_fec_data.o \
                   tbtun/tbt_fec_adapt.o tbtun/tbt_fec_adapt_sclc.o \
                   tbtun/tbt_mux.o \
                   tbtun/tbt_fec_sclc.o \
                   tbtun/tbt_fec_codec_cache.o \
                   tbtun/tbt_sc_ldpc.o \
                   tbtun/tbt_sc_ldpc_gf16.o tbtun/tbt_sclc_uplink.o

# Keep every module frame small enough for the kernel stack: a 1 KiB bound
# catches a TBT_STREAMS_MAX pointer array before it reaches a real call path.
ccflags-y       := -std=gnu99 -I$(src)/tbtun -Wframe-larger-than=1024
# tbt_cc.c's ACK hook is selected from the installed header, not from a
# version number: this tree declares cong_control(sk, ack, flag, rs), earlier
# ones declare cong_control(sk, rs) or only the cong_avoid/pkts_acked pair.
# The two cong_control forms are both valid function-pointer types, so a C
# feature test cannot tell them apart; read the declaration. $srctree is the
# kernel tree inside kbuild; KERNEL_DIR covers a direct invocation of this
# Makefile. A header that changes either signature fails the build instead of
# attaching the wrong arguments.
TBT_CC_TCP_H := $(firstword $(wildcard $(srctree)/include/net/tcp.h) \
                            $(wildcard $(KERNEL_DIR)/include/net/tcp.h))
TBT_CC_ACK_HOOK := $(shell [ -n "$(TBT_CC_TCP_H)" ] && \
        grep -qs 'u32 ack, int flag, const struct rate_sample' $(TBT_CC_TCP_H) && echo 1)
TBT_CC_INIT_HOOK := $(shell [ -n "$(TBT_CC_TCP_H)" ] && \
        grep -qs 'void (\*init)(struct sock \*sk);' $(TBT_CC_TCP_H) && echo 1)
ccflags-y += $(if $(TBT_CC_ACK_HOOK),-DTBT_CC_HAVE_CONG_CTRL_ACK=1) \
             $(if $(TBT_CC_INIT_HOOK),-DTBT_CC_HAVE_INIT=1)
# ChaCha8 is a hot per-byte loop; keep the kernel's -O2 for the rest of the
# module and let this one file unroll its fixed 8-round permutation. The
# variable must name the object path Kbuild sees for a composite module.
CFLAGS_tbtun/tbt_crypto.o := -O3
# The SC-LDPC inner loops (bit transpose, peeling sweep) are the second
# largest TX/RX cost after the cipher; -O3 measured 6.9 -> 6.4 us per 2 MiB
# block encode on the host and leaves the object the same size class.
CFLAGS_tbtun/tbt_fec_sclc.o := -O3

.PHONY: all check clean load unload qemu-test host-test host-test-san host-test-san-run host-test-tsan host-test-live host-bench-micro host-bench-sclc host-bench-sclc-down host-bench-sclc-sim host-bench-sclc-pipe host-bench-sclc-build host-bench-rxpath host-calib-sclc-up calib-sclc-up bench bench-full golden-v080

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) $(KBUILD_LLVM) modules

# One command for a human: resolves the header payload, prints the make line it
# runs, then builds the module and runs all host tests (docs/build-env.md).
check: dkms-srcs-check
	@if [ -x ./scripts/build.sh ]; then ./scripts/build.sh; else \
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
$(GOLDEN_V080): $(GOLDEN_V080_SRCS) $(GOLDEN_V080_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(GOLDEN_V080_SRCS)

# Regenerate the 0.8.0 F# golden vectors from the shipping C units.
golden-v080: $(GOLDEN_V080)
	@./$(GOLDEN_V080)

host-test-san-run: host/test_carrier_policy_san

host-test: $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_CRYPTO) $(HOST_REORDER) $(HOST_ACK) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FIN_TAIL) $(HOST_CELL_TAIL) $(HOST_MEM_BUDGET) $(HOST_FEC_RX) $(HOST_FEC_SWITCH) $(HOST_FEC_OUT) $(HOST_CODEC_CACHE) $(HOST_BATCH) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_FEC_SCLC_SHAPES) $(HOST_PORTMAP) $(HOST_MAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_SCLC_DATA) $(HOST_FAULT) $(HOST_INVARIANTS) $(HOST_WIRE_COMPAT) $(HTTP_RFC_GEN) $(HOST_RX_BUDGET) $(HOST_MUX) $(HOST_CLIENT) $(HOST_CLIENT_LOOPBACK) $(HOST_CLIENT_WIRE) $(HOST_CLIENT_RX)
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
	@./$(HOST_FIN_TAIL)
	@./$(HOST_CELL_TAIL)
	@./$(HOST_MEM_BUDGET)
	@./$(HOST_FEC_RX)
	@./$(HOST_FEC_SWITCH)
	@./$(HOST_FEC_OUT)
	@./$(HOST_CODEC_CACHE)
	@./$(HOST_BATCH)
	@./$(HOST_FEC_ADAPT)
	@./$(HOST_FEC_ADAPT_SCLC)
	@./$(HOST_FEC_SCLC)
	@./$(HOST_FEC_SCLC_SHAPES)
	@./$(HOST_PORTMAP)
	@./$(HOST_MAP)
	@./$(HOST_CARRIER_POLICY)
	@./$(HOST_LIFETIME)
	@./$(HOST_SCLC)
	@./$(HOST_SCLC_DATA)
	@./$(HOST_FAULT)
	@./$(HOST_INVARIANTS)
	@./$(HOST_WIRE_COMPAT)
	@./$(HOST_RX_BUDGET)
	@./$(HOST_CAPTURE_POKE)
	@./$(HOST_MUX)
	@./$(HOST_CLIENT_LOOPBACK)
	@./$(HOST_CLIENT_WIRE)
	@./$(HOST_CLIENT_RX)
	@# Third-party cross-check: h11 is not a build dependency. uv with a cached
	@# h11 runs it offline; anything else gets a clear skip, not a failure.
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python $(HOST_HTTP_H11) ./$(HOST_HTTP); \
	else \
		echo "host-test: h11 cross-check skipped (uv or cached h11 unavailable)"; \
	fi
	@# The RFC 9112 audit drives tbt_http.c over many keep-alive cycles. uv
	@# supplies h11 when its offline cache has it; the strict state machine in
	@# the audit runs either way.
	@if command -v uv >/dev/null 2>&1 && \
		uv run --offline --no-project --with h11 -q python -c 'import h11' >/dev/null 2>&1; then \
		uv run --offline --no-project --with h11 -q python $(HTTP_RFC_AUDIT) ./$(HTTP_RFC_GEN); \
	else \
		echo "host-test: http-rfc9112 audit runs without h11 (uv or cached h11 unavailable)"; \
		python3 $(HTTP_RFC_AUDIT) ./$(HTTP_RFC_GEN); \
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

$(HTTP_RFC_GEN): $(HTTP_RFC_GEN_SRCS) $(HTTP_RFC_GEN_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HTTP_RFC_GEN_SRCS)

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

# Per-record RX chain cost (HTTP deframe, header/payload CRC, ChaCha8, FEC
# placement, ordered delivery). Not part of `check`: a measurement.
$(HOST_BENCH_RXPATH): $(HOST_BENCH_RXPATH_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_RXPATH_SRCS)

host-bench-rxpath: $(HOST_BENCH_RXPATH)
	@./$(HOST_BENCH_RXPATH)

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

$(HOST_FIN_TAIL): $(HOST_FIN_TAIL_SRCS) $(HOST_FIN_TAIL_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FIN_TAIL_SRCS)

$(HOST_CELL_TAIL): $(HOST_CELL_TAIL_SRCS) $(HOST_CELL_TAIL_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CELL_TAIL_SRCS)

$(HOST_MEM_BUDGET): $(HOST_MEM_BUDGET_SRCS) $(HOST_MEM_BUDGET_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_MEM_BUDGET_SRCS)
$(HOST_FEC_RX): $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_RX_SRCS)

$(HOST_FEC_SWITCH): $(HOST_FEC_SWITCH_SRCS) $(HOST_FEC_SWITCH_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SWITCH_SRCS)

$(HOST_FEC_OUT): $(HOST_FEC_OUT_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

$(HOST_CODEC_CACHE): $(HOST_CODEC_CACHE_SRCS) $(HOST_CODEC_CACHE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CODEC_CACHE_SRCS)

$(HOST_BATCH): $(HOST_BATCH_SRCS) $(HOST_BATCH_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BATCH_SRCS)

$(HOST_FEC_ADAPT): $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SRCS)

$(HOST_FEC_ADAPT_SCLC): $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

$(HOST_FEC_SCLC): $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SRCS)

$(HOST_FEC_SCLC_SHAPES): $(HOST_FEC_SCLC_SHAPES_SRCS) $(HOST_FEC_SCLC_SHAPES_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SHAPES_SRCS)

$(HOST_PORTMAP): $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_PORTMAP_SRCS)

$(HOST_MAP): $(HOST_MAP_SRCS) $(HOST_MAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_MAP_SRCS)

$(HOST_CARRIER_POLICY): host/test_carrier_policy.c $(HOST_CARRIER_POLICY_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ host/test_carrier_policy.c

$(HOST_LIFETIME): $(HOST_LIFETIME_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

$(HOST_SCLC): $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_SRCS)

$(HOST_SCLC_DATA): $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_FAULT): $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_FAULT_SRCS)

$(HOST_INVARIANTS): $(HOST_INVARIANTS_SRCS) $(HOST_INVARIANTS_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_INVARIANTS_SRCS)

$(HOST_WIRE_COMPAT): $(HOST_WIRE_COMPAT_SRCS) $(HOST_WIRE_COMPAT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_WIRE_COMPAT_SRCS)

$(HOST_RX_BUDGET): $(HOST_RX_BUDGET_SRCS) $(HOST_RX_BUDGET_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_RX_BUDGET_SRCS)

$(HOST_CAPTURE_POKE): host/test_capture_poke.c $(HOST_CAPTURE_POKE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ host/test_capture_poke.c

host/test_fec_sclc_data_san: $(HOST_SCLC_DATA_SRCS) $(HOST_SCLC_DATA_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_DATA_SRCS)

$(HOST_BENCH_SCLC): $(HOST_BENCH_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_SRCS)

$(HOST_BENCH_SCLC_DOWN): $(HOST_BENCH_SCLC_DOWN_SRCS) $(HOST_BENCH_SCLC_DOWN_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_DOWN_SRCS)

host-bench-sclc: $(HOST_BENCH_SCLC)
	@./$(HOST_BENCH_SCLC) 8 8 24 1 4 0.3
	@./$(HOST_BENCH_SCLC) 8 8 24 1 4 0.4

# Paired recovery comparison across the six uplink rate splits. A
# measurement, not a gate: the same block and the same share-loss mask are
# decoded with the legacy and the bit-plane mapping.
$(HOST_BENCH_RECOVERY): $(HOST_BENCH_RECOVERY_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_RECOVERY_SRCS)

host-bench-recovery: $(HOST_BENCH_RECOVERY)
	@./$(HOST_BENCH_RECOVERY)

host-bench-sclc-down: $(HOST_BENCH_SCLC_DOWN)
	@./$(HOST_BENCH_SCLC_DOWN)

$(HOST_CALIB_SCLC_DOWN): $(HOST_CALIB_SCLC_DOWN_SRCS) $(HOST_CALIB_SCLC_DOWN_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CALIB_SCLC_DOWN_SRCS)

.PHONY: host-calib-sclc-down
# Recovery calibration of the shipped downlink: failure rate against share
# loss, the 0 ms wake race, burst and dead-carrier models. It is a
# measurement, not a gate; the full matrix is a shell loop documented in
# review/calib/sclc-down.md. The smoke run only proves the binary works.
host-calib-sclc-down: $(HOST_CALIB_SCLC_DOWN)
	@./$(HOST_CALIB_SCLC_DOWN) --header
	@./$(HOST_CALIB_SCLC_DOWN) iid 50 300000 200 20261111
	@./$(HOST_CALIB_SCLC_DOWN) first 70 100000 200 20261111
	@./$(HOST_CALIB_SCLC_DOWN) ge 60 200000 200 20261111 4
	@./$(HOST_CALIB_SCLC_DOWN) dead 50 0 200 20261111 16

$(HOST_CALIB_SCLC_DE): $(HOST_CALIB_SCLC_DE_SRCS) $(HOST_CALIB_SCLC_DE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CALIB_SCLC_DE_SRCS)

.PHONY: host-calib-sclc-de
# Deterministic BEC density evolution for every constructible n, plus the
# constructor's own accept range. No random input, no timing.
host-calib-sclc-de: $(HOST_CALIB_SCLC_DE)
	@./$(HOST_CALIB_SCLC_DE) --range
	@./$(HOST_CALIB_SCLC_DE)

$(HOST_BENCH_SCLC_SIM): $(HOST_BENCH_SCLC_SIM_SRCS) $(HOST_BENCH_SCLC_SIM_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_SIM_SRCS)

# Closed-loop 120 s BEC trace through the real estimator, controller and
# downlink ladder. This is the cross-rate wire/app witness; it is not a gate
# and stays out of `check` because it runs the codec for simulation time.
host-bench-sclc-sim: $(HOST_BENCH_SCLC_SIM)
	@./$(HOST_BENCH_SCLC_SIM)

# Full-chain production shapes: FEC, header CRCs and ChaCha8 for both
# directions, at the report loss points. Not part of `check`.
$(HOST_BENCH_SCLC_PIPE): $(HOST_BENCH_SCLC_PIPE_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_PIPE_SRCS)

host-bench-sclc-pipe: $(HOST_BENCH_SCLC_PIPE)
	@./$(HOST_BENCH_SCLC_PIPE)

$(HOST_BENCH_SCLC_BUILD): $(HOST_BENCH_SCLC_BUILD_SRCS) $(HOST_BENCH_SCLC_BUILD_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_BENCH_SCLC_BUILD_SRCS)

# Codec build cost against the route-A gate: p99 <= 2 ms for the downlink
# binary codec and for the eight-group uplink GF(16) build.
host-bench-sclc-build: $(HOST_BENCH_SCLC_BUILD)
	@./$(HOST_BENCH_SCLC_BUILD)
# Calibration of the uplink 8x32 GF(16) SC-LDPC ladder against the real
# codec. The binary is a measurement, not a gate, so it is not part of
# host-test. Six ladder points run in parallel; every sample seeds itself
# from (section, level, p), so the CSV does not depend on completion order.
$(HOST_CALIB_SCLC_UP): $(HOST_CALIB_SCLC_UP_SRCS) $(HOST_CALIB_SCLC_UP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CALIB_SCLC_UP_SRCS) -lm

host-calib-sclc-up: $(HOST_CALIB_SCLC_UP)

CALIB_LEVELS := 25 30 40 50 60 70
CALIB_DIR := review/calib
CALIB_PARTS := $(CALIB_DIR)/.parts

calib-sclc-up: $(HOST_CALIB_SCLC_UP)
	@mkdir -p $(CALIB_PARTS)
	@./$(HOST_CALIB_SCLC_UP) header > $(CALIB_PARTS)/00-header.csv
	@./$(HOST_CALIB_SCLC_UP) construct > $(CALIB_PARTS)/01-construct.csv
	@set -e; for pct in $(CALIB_LEVELS); do \
	  ./$(HOST_CALIB_SCLC_UP) analyze --level $$pct > $(CALIB_PARTS)/02-analyze-$$pct.csv & \
	  ./$(HOST_CALIB_SCLC_UP) spot --level $$pct --trials 1000 > $(CALIB_PARTS)/03-spot-$$pct.csv & \
	done; wait
	@./$(HOST_CALIB_SCLC_UP) spot --level 70 --trials 1000 --core 512 > $(CALIB_PARTS)/04-spot-core512-70.csv
	@cat $(CALIB_PARTS)/*.csv > $(CALIB_DIR)/sclc-up.csv
	@rm -rf $(CALIB_PARTS)
	@echo "calib-sclc-up: wrote $(CALIB_DIR)/sclc-up.csv"

# The same tests with ASan and UBSan. A toolchain without the runtimes gets
# a clear skip, not a build failure.
host-test-san: host/test_capture_poke_san
	@if printf 'int main(void){return 0;}\n' | $(HOSTCC) $(SAN_CFLAGS) -x c - -o /dev/null 2>/dev/null; then \
		$(MAKE) --no-print-directory host-test-san-run; \
	else \
		echo "host-test-san: skipped, $(HOSTCC) cannot link -fsanitize=address,undefined"; \
	fi

host-test-san-run: host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san host/test_crypto_san host/test_fec_san host/test_fin_tail_san host/test_cell_tail_san host/test_mem_budget_san host/test_fec_rx_san host/test_fec_switch_san host/test_fec_out_san host/test_batch_san host/test_fec_adapt_san host/test_fec_adapt_sclc_san host/test_fec_sclc_san host/test_fec_sclc_shapes_san host/test_close_wake_san host/test_hold_san host/test_portmap_san host/test_map_san host/test_lifetime_san host/test_sclc_uplink_san host/test_fec_sclc_data_san host/test_fault_san host/test_invariants_san host/test_wire_compat_san host/test_rx_budget_san host/test_mux_san host/test_client_loopback_san host/test_client_wire_san host/test_client_rx_san $(HTTP_RFC_GEN_SAN)
	@./host/test_parse_san
	@./host/test_fuzz_san
	@./host/test_http_san
	@./host/test_http_fuzz_san
	@./host/test_crypto_san
	@./host/test_fec_san
	@./host/test_fin_tail_san
	@./host/test_cell_tail_san
	@./host/test_mem_budget_san
	@./host/test_fec_rx_san
	@./host/test_fec_switch_san
	@./host/test_fec_out_san
	@./host/test_batch_san
	@./host/test_fec_adapt_san
	@./host/test_fec_adapt_sclc_san
	@./host/test_fec_sclc_san
	@./host/test_fec_sclc_shapes_san
	@./host/test_hold_san
	@./host/test_portmap_san
	@./host/test_map_san
	@./host/test_carrier_policy_san
	@./host/test_lifetime_san
	@./host/test_sclc_uplink_san
	@./host/test_fec_sclc_data_san
	@./host/test_close_wake_san
	@./host/test_fault_san
	@./host/test_invariants_san
	@./host/test_wire_compat_san
	@./host/test_rx_budget_san
	@./host/test_capture_poke_san
	@./host/test_mux_san
	@./host/test_client_loopback_san
	@./host/test_client_wire_san
	@./host/test_client_rx_san
	@python3 $(HTTP_RFC_AUDIT) ./$(HTTP_RFC_GEN_SAN)

host/test_parse_san: $(HOST_TEST_SRCS) $(HOST_TEST_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_TEST_SRCS)

host/test_fuzz_san: $(HOST_FUZZ_SRCS) $(HOST_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FUZZ_SRCS)

host/test_http_san: $(HOST_HTTP_SRCS) $(HOST_HTTP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_SRCS)

host/test_http_fuzz_san: $(HOST_HTTP_FUZZ_SRCS) $(HOST_HTTP_FUZZ_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HTTP_FUZZ_SRCS)

$(HTTP_RFC_GEN_SAN): $(HTTP_RFC_GEN_SRCS) $(HTTP_RFC_GEN_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HTTP_RFC_GEN_SRCS)

host/test_crypto_san: $(HOST_CRYPTO_SRCS) $(HOST_CRYPTO_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CRYPTO_SRCS)

host/test_fec_san: $(HOST_FEC_SRCS) $(HOST_FEC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SRCS)

host/test_fin_tail_san: $(HOST_FIN_TAIL_SRCS) $(HOST_FIN_TAIL_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FIN_TAIL_SRCS)

host/test_cell_tail_san: $(HOST_CELL_TAIL_SRCS) $(HOST_CELL_TAIL_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CELL_TAIL_SRCS)

host/test_mem_budget_san: $(HOST_MEM_BUDGET_SRCS) $(HOST_MEM_BUDGET_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_MEM_BUDGET_SRCS)
host/test_close_wake_san: $(HOST_CLOSE_WAKE_SRCS) $(HOST_CLOSE_WAKE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CLOSE_WAKE_SRCS)

host/test_hold_san: $(HOST_HOLD_SRCS) $(HOST_HOLD_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_HOLD_SRCS)

host/test_fec_rx_san: $(HOST_FEC_RX_SRCS) $(HOST_FEC_RX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_RX_SRCS)

host/test_fec_switch_san: $(HOST_FEC_SWITCH_SRCS) $(HOST_FEC_SWITCH_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SWITCH_SRCS)

host/test_fec_out_san: $(HOST_FEC_OUT_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_FEC_OUT_SRCS)

host/test_batch_san: $(HOST_BATCH_SRCS) $(HOST_BATCH_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_BATCH_SRCS)

host/test_fec_adapt_san: $(HOST_FEC_ADAPT_SRCS) $(HOST_FEC_ADAPT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SRCS)

host/test_fec_adapt_sclc_san: $(HOST_FEC_ADAPT_SCLC_SRCS) $(HOST_FEC_ADAPT_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_ADAPT_SCLC_SRCS)

host/test_fec_sclc_san: $(HOST_FEC_SCLC_SRCS) $(HOST_FEC_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SRCS)

host/test_fec_sclc_shapes_san: $(HOST_FEC_SCLC_SHAPES_SRCS) $(HOST_FEC_SCLC_SHAPES_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FEC_SCLC_SHAPES_SRCS)

host/test_portmap_san: $(HOST_PORTMAP_SRCS) $(HOST_PORTMAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_PORTMAP_SRCS)

host/test_map_san: $(HOST_MAP_SRCS) $(HOST_MAP_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_MAP_SRCS)

host/test_carrier_policy_san: host/test_carrier_policy.c $(HOST_CARRIER_POLICY_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ host/test_carrier_policy.c

host/test_lifetime_san: $(HOST_LIFETIME_SRCS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -o $@ $(HOST_LIFETIME_SRCS)

host/test_sclc_uplink_san: $(HOST_SCLC_SRCS) $(HOST_SCLC_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_SCLC_SRCS)

host/test_fault_san: $(HOST_FAULT_SRCS) $(HOST_FAULT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_FAULT_SRCS)

host/test_invariants_san: $(HOST_INVARIANTS_SRCS) $(HOST_INVARIANTS_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_INVARIANTS_SRCS)

host/test_wire_compat_san: $(HOST_WIRE_COMPAT_SRCS) $(HOST_WIRE_COMPAT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_WIRE_COMPAT_SRCS)

host/test_rx_budget_san: $(HOST_RX_BUDGET_SRCS) $(HOST_RX_BUDGET_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_RX_BUDGET_SRCS)

host/test_capture_poke_san: host/test_capture_poke.c $(HOST_CAPTURE_POKE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ host/test_capture_poke.c

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
	@TSAN_OPTIONS=halt_on_error=1:exitcode=66 ./$(HOST_TSAN)

$(HOST_TSAN): $(HOST_TSAN_SRCS) $(HOST_TSAN_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(TSAN_CFLAGS) -pthread -I tbtun -o $@ $(HOST_TSAN_SRCS)

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
	$(RM) $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_HTTP_LIVE) $(HOST_CRYPTO) $(HOST_BENCH_CRYPTO) $(HOST_BENCH_MICRO) $(HOST_BENCH_TXPATH) $(HOST_REORDER) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FEC_RX) $(HOST_FEC_SWITCH) $(HOST_FEC_OUT) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_FEC_SCLC_SHAPES) $(HOST_PORTMAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_FAULT) $(HOST_BENCH_SCLC) $(HOST_BENCH_SCLC_DOWN) $(HOST_BENCH_SCLC_PIPE) host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san host/test_crypto_san host/test_fec_san host/test_fec_rx_san host/test_fec_switch_san host/test_fec_out_san host/test_fec_adapt_san host/test_fec_adapt_sclc_san host/test_fec_sclc_san host/test_fec_sclc_shapes_san host/test_close_wake_san host/test_portmap_san host/test_lifetime_san host/test_sclc_uplink_san host/test_fault_san
	$(RM) $(HOST_TEST) $(HOST_FUZZ) $(HOST_HTTP) $(HOST_HTTP_LIVE) $(HOST_CRYPTO) $(HOST_BENCH_CRYPTO) $(HOST_BENCH_MICRO) $(HOST_BENCH_TXPATH) $(HOST_REORDER) $(HOST_CONTROL) $(HOST_TX_PUBLISH) $(HOST_TX_INDEX) $(HOST_FIN_PRIORITY) $(HOST_CLOSE_WAKE) $(HOST_HOLD) $(HOST_MULTISTREAM) $(HOST_ANCHOR) $(HOST_FEC) $(HOST_FEC_RX) $(HOST_FEC_SWITCH) $(HOST_FEC_OUT) $(HOST_FEC_ADAPT) $(HOST_FEC_ADAPT_SCLC) $(HOST_FEC_SCLC) $(HOST_FEC_SCLC_LADDER) $(HOST_PORTMAP) $(HOST_LIFETIME) $(HOST_SCLC) $(HOST_FAULT) $(HOST_BENCH_SCLC) $(HOST_BENCH_SCLC_DOWN) $(HOST_BENCH_SCLC_SIM) $(HOST_BENCH_SCLC_PIPE) host/test_parse_san host/test_fuzz_san host/test_http_san host/test_http_fuzz_san host/test_crypto_san host/test_fec_san host/test_fec_rx_san host/test_fec_switch_san host/test_fec_out_san host/test_fec_adapt_san host/test_fec_adapt_sclc_san host/test_fec_sclc_san host/test_fec_sclc_ladder_san host/test_close_wake_san host/test_portmap_san host/test_lifetime_san host/test_sclc_uplink_san host/test_fault_san
	$(RM) $(HOST_CALIB_SCLC_DOWN) $(HOST_CALIB_SCLC_DE)
	$(RM) $(HOST_INVARIANTS) host/test_invariants_san
	$(RM) $(HOST_MEM_BUDGET) host/test_mem_budget_san
	$(RM) $(HOST_CELL_TAIL) host/test_cell_tail_san
	$(RM) $(HOST_ACK) $(HOST_FIN_TAIL) $(HOST_MAP) $(HOST_SCLC_DATA) $(HOST_MUX)
	$(RM) host/test_fin_tail_san host/test_map_san host/test_fec_sclc_data_san
	$(RM) host/test_mux_san
	$(RM) $(HOST_BENCH_RECOVERY)
	$(RM) $(HOST_CODEC_CACHE)
	$(RM) host/test_hold_san
	$(RM) $(HOST_BATCH) $(HOST_WIRE_COMPAT) $(HOST_TSAN) host/test_batch_san host/test_wire_compat_san
	$(RM) $(HTTP_RFC_GEN) $(HTTP_RFC_GEN_SAN)
	$(RM) $(HOST_RX_BUDGET) host/test_rx_budget_san
	$(RM) $(HOST_CAPTURE_POKE) host/test_capture_poke_san
	$(RM) $(HOST_CLIENT) $(HOST_CLIENT_LIB) $(HOST_CLIENT_OBJS) host/client-main.o
	$(RM) $(HOST_CLIENT_LOOPBACK) host/test_client_loopback_san
	$(RM) $(HOST_CLIENT_WIRE) host/test_client_wire_san
	$(RM) $(HOST_CLIENT_RX) host/test_client_rx_san
	$(RM) $(HOST_BENCH_RXPATH)
	$(RM) $(HOST_BENCH_SCLC_BUILD)
	$(RM) $(HOST_CARRIER_POLICY) host/test_carrier_policy_san
	$(RM) $(wildcard tbtun/*.su) $(wildcard host/*.su)
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
# before the archive is written, not on the target machine. The same gate
# rejects a stale SRCS entry and a staging file that lost its SRCS entry.
dkms-srcs-check:
	@missing=$$(find tbtun \( -name '*.c' -o -name '*.h' \) | sort | while IFS= read -r f; do \
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

$(HOST_MUX): $(HOST_MUX_SRCS) $(HOST_MUX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_MUX_SRCS)

host/test_mux_san: $(HOST_MUX_SRCS) $(HOST_MUX_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_MUX_SRCS)

host/client-%.o: tbtun/%.c $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -c -o $@ $<

host/client-engine.o: host/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -c -o $@ $<

host/client-main.o: host/tbt_client_main.c $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -c -o $@ $<

$(HOST_CLIENT_LIB): $(HOST_CLIENT_OBJS)
	$(AR) rcs $@ $^

$(HOST_CLIENT): host/client-main.o $(HOST_CLIENT_LIB)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ host/client-main.o $(HOST_CLIENT_LIB)

$(HOST_CLIENT_LOOPBACK): host/test_client_loopback.c $(HOST_CLIENT_LIB) $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ host/test_client_loopback.c $(HOST_CLIENT_LIB)

host/test_client_loopback_san: host/test_client_loopback.c $(HOST_CLIENT_LIB_SRCS) $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -DTBT_LOOPBACK_MB=1 -I tbtun -o $@ host/test_client_loopback.c $(HOST_CLIENT_LIB_SRCS)

$(HOST_CLIENT_WIRE): $(HOST_CLIENT_WIRE_SRCS) $(HOST_CLIENT_WIRE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -I tbtun -o $@ $(HOST_CLIENT_WIRE_SRCS)

host/test_client_wire_san: $(HOST_CLIENT_WIRE_SRCS) $(HOST_CLIENT_WIRE_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -I tbtun -o $@ $(HOST_CLIENT_WIRE_SRCS)

$(HOST_CLIENT_RX): $(HOST_CLIENT_RX_SRCS) host/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) -Wno-unused-function -Wno-unused-variable -I tbtun -I host -o $@ $(HOST_CLIENT_RX_SRCS)

host/test_client_rx_san: $(HOST_CLIENT_RX_SRCS) host/tbt_client_engine.c $(HOST_CLIENT_HDRS)
	$(HOSTCC) $(HOST_CFLAGS) $(SAN_CFLAGS) -Wno-unused-function -Wno-unused-variable -I tbtun -I host -o $@ $(HOST_CLIENT_RX_SRCS)
