# deploy/090/Makefile
#
# Build one deployable set from this tree without patching mihomo:
#   - vanilla mihomo, downloaded from the upstream GitHub release;
#   - the 0.8 userspace client (src/client/tbt_client) for the near side;
#   - tbtun.ko from the existing Kbuild rules for the far side.
#
# Usage:
#   make -f deploy/090/Makefile package
#   make -f deploy/090/Makefile package MIHOMO_VERSION=v1.19.34 MIHOMO_SHA256=<hex>
#   make -f deploy/090/Makefile configs UUID=<uuid> DIST=/tmp/tbt-090
#   make -f deploy/090/Makefile package CRYPTO_KEY=<64 hex> NONCE=<32 hex>
#
# Upgrading mihomo is one variable: MIHOMO_VERSION.
# Leaving it unset resolves the latest upstream release tag.
#
# The near and far tbtun.conf files get the same generated CRYPTO_KEY and
# NONCE. Both are printed into the package, so package once and deploy the
# two halves together; re-running package without CRYPTO_KEY=... mints a new
# key pair and the old far side will reject it.

SHELL        := /bin/sh
REPO_ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
DIST         ?= $(REPO_ROOT)/dist/090
# Honor KDIR from docs/build-env.md's env.sh when it is exported; otherwise
# fall back to the distribution path.
KERNEL_DIR   ?= $(if $(KDIR),$(KDIR),/lib/modules/$(shell uname -r)/build)
UNAME_M      := $(shell uname -m)
GOARCH       ?= $(if $(filter x86_64,$(UNAME_M)),amd64,$(if $(filter aarch64,$(UNAME_M)),arm64,$(UNAME_M)))

MIHOMO_API_LATEST := https://api.github.com/repos/MetaCubeX/mihomo/releases/latest
MIHOMO_VERSION ?= $(shell curl -fsSL $(MIHOMO_API_LATEST) \
                    | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
MIHOMO_API     := https://api.github.com/repos/MetaCubeX/mihomo/releases/tags/$(MIHOMO_VERSION)

# Optional. When empty, the release asset digest from the GitHub API is used.
# Set it to pin a checksum from an independent source.
MIHOMO_SHA256 ?=

# `:=` matters: a recursive `?=` re-runs the shell on every reference, and
# near/config.yaml, far/config.yaml, and both tbtun.conf files each expand
# these names. Recursive definitions gave the two sides different UUIDs and
# different PSKs in one package run. Command-line values still override a
# `:=` assignment, so `make ... UUID=... CRYPTO_KEY=...` keeps working.
UUID         := $(shell cat /proc/sys/kernel/random/uuid)
# 256-bit pre-shared key (64 hex digits) and 128-bit session nonce (32 hex
# digits). Without crypto_key= the near side advertises its session key in
# clear text in every HELLO; with it, both ends derive the same key and the
# HELLO carries key_id=0 with no key material.
CRYPTO_KEY   := $(shell od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
NONCE        := $(shell od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
NEAR_PORT    ?= 19000
FAR_PORT     ?= 19002
VLESS_PORT   ?= 19001
MIXED_PORT   ?= 7890
API_PORT     ?= 9090
FAR_IP       ?= 203.0.113.10
# A bare IPv6 literal (the HKG0 V6 test address) must be bracketed inside the
# peer= URL; a value already written as [addr] is left alone.
FAR_IP_URL   := $(if $(findstring :,$(FAR_IP)),$(if $(filter [%,$(FAR_IP)),$(FAR_IP),[$(FAR_IP)]),$(FAR_IP))
# Empty by default: a near client behind a dynamic ISP address or CGNAT has
# no stable public address to pin. The far side then admits the first client
# that presents the configured CRYPTO_KEY and NONCE, and keeps that anchor
# until the session is re-created. Set NEAR_IP=<address> to pin it.
NEAR_IP      ?=

TBTUN_COMMIT := $(shell git -C $(REPO_ROOT) rev-parse --short HEAD 2>/dev/null || echo unknown)

SYSTEMD_UNITS_NEAR := tbt-client-090.service mihomo-090-near.service
SYSTEMD_UNITS_FAR  := mihomo-090-far.service tbtun-090-far.service
NEAR_UNITS := $(addprefix $(DIST)/near/systemd/,$(SYSTEMD_UNITS_NEAR))
FAR_UNITS  := $(addprefix $(DIST)/far/systemd/,$(SYSTEMD_UNITS_FAR))

ifeq ($(GOARCH),amd64)
MIHOMO_ASSET := mihomo-linux-amd64-compatible-$(MIHOMO_VERSION).gz
else ifeq ($(GOARCH),arm64)
MIHOMO_ASSET := mihomo-linux-arm64-$(MIHOMO_VERSION).gz
else
$(error unsupported GOARCH=$(GOARCH))
endif
MIHOMO_URL := https://github.com/MetaCubeX/mihomo/releases/download/$(MIHOMO_VERSION)/$(MIHOMO_ASSET)

.PHONY: all package package-near package-far mihomo client module configs verify clean mihomo-digest
all: package

# Download and verify the upstream asset. The digest comes from MIHOMO_SHA256
# or from the release API's per-asset "digest" field; an empty digest is a hard
# error rather than a silent unverified install.
$(DIST)/mihomo:
	@mkdir -p $(DIST)
	@echo "fetch $(MIHOMO_URL)"
	curl -fL --retry 3 -o $@.gz "$(MIHOMO_URL)"
	@expected="$(MIHOMO_SHA256)"; \
	if [ -z "$$expected" ]; then \
	  expected=$$(curl -fsSL "$(MIHOMO_API)" \
	    | python3 -c 'import json,sys; n=sys.argv[1]; d=json.load(sys.stdin); \
print(next((a.get("digest","").split(":",1)[-1] for a in d.get("assets",[]) if a.get("name")==n), ""))' \
	      "$(MIHOMO_ASSET)"); \
	fi; \
	if [ -z "$$expected" ]; then \
	  echo "no sha256 for $(MIHOMO_ASSET); set MIHOMO_SHA256=<hex>"; \
	  rm -f $@.gz; \
	  exit 1; \
	fi; \
	printf '%s  %s\n' "$$expected" "$@.gz" | sha256sum -c -
	gzip -dc $@.gz > $@.tmp
	chmod 0755 $@.tmp
	mv $@.tmp $@
	rm -f $@.gz
	$@ -v

mihomo: $(DIST)/mihomo

mihomo-digest:
	@curl -fsSL "$(MIHOMO_API)" \
	  | python3 -c 'import json,sys; n=sys.argv[1]; d=json.load(sys.stdin); \
print(next((a.get("digest","") for a in d.get("assets",[]) if a.get("name")==n), "not found"))' \
	    "$(MIHOMO_ASSET)"

$(DIST)/near/tbt_client: FORCE
	@mkdir -p $(dir $@)
	$(MAKE) -C $(REPO_ROOT) $(REPO_ROOT)/build/bin/tbt_client
	install -m 0755 $(REPO_ROOT)/build/bin/tbt_client $@

client: $(DIST)/near/tbt_client

# Kernel module: far side only.
$(DIST)/far/tbtun.ko: FORCE
	@mkdir -p $(dir $@)
	@test -d "$(KERNEL_DIR)" || { \
	  echo "KERNEL_DIR=$(KERNEL_DIR) not found; see docs/build-env.md"; \
	  exit 1; }
	$(MAKE) -C $(REPO_ROOT) KERNEL_DIR=$(KERNEL_DIR) all
	install -m 0644 $(REPO_ROOT)/build/kernel/tbtun.ko $@
	modinfo $@ | sed -n '1,8p'

module: $(DIST)/far/tbtun.ko

# FORCE: UUID/CRYPTO_KEY/NONCE are minted per make run. Without FORCE a stale
# config.yaml from the previous run keeps its old UUID while the freshly
# written tbtun.conf carries the new PSK, and the two sides disagree.
$(DIST)/near/config.yaml: $(REPO_ROOT)/deploy/090/near.yaml.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(NEAR_PORT)/g' \
	    -e 's/@MIXED_PORT@/$(MIXED_PORT)/g' \
	    -e 's/@API_PORT@/$(API_PORT)/g' $< > $@

$(DIST)/far/config.yaml: $(REPO_ROOT)/deploy/090/far.yaml.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(VLESS_PORT)/g' $< > $@

configs: $(DIST)/near/config.yaml $(DIST)/far/config.yaml

$(DIST)/near/systemd/%.service: $(REPO_ROOT)/deploy/090/systemd/%.service.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@TBTUN_COMMIT@/$(TBTUN_COMMIT)/g' $< > $@

$(DIST)/far/systemd/%.service: $(REPO_ROOT)/deploy/090/systemd/%.service.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@TBTUN_COMMIT@/$(TBTUN_COMMIT)/g' $< > $@

package-near: mihomo client $(DIST)/near/config.yaml $(NEAR_UNITS)
	install -m 0755 $(DIST)/mihomo $(DIST)/near/mihomo
	printf '%s\n' \
	  "tunnel 1 role=near listen=127.0.0.1:$(NEAR_PORT) peer=$(FAR_IP_URL):$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=$(CRYPTO_KEY) nonce=$(NONCE) adaptive=off" \
	  > $(DIST)/near/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-near.sh $(DIST)/near/install-near.sh

package-far: mihomo module $(DIST)/far/config.yaml $(FAR_UNITS)
	install -m 0755 $(DIST)/mihomo $(DIST)/far/mihomo
	printf '%s\n' \
	  "tunnel 1 role=far listen=0.0.0.0:$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=$(CRYPTO_KEY) nonce=$(NONCE) adaptive=off$(if $(strip $(NEAR_IP)), peer=$(NEAR_IP):$(NEAR_PORT))" \
	  > $(DIST)/far/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-far.sh $(DIST)/far/install-far.sh

package: package-near package-far
	{ echo "mihomo_version: $(MIHOMO_VERSION)"; \
	  echo "mihomo_asset: $(MIHOMO_ASSET)"; \
	  echo "tbtun_commit: $$(git -C $(REPO_ROOT) rev-parse HEAD)"; \
	  echo "tbtun_dirty: $$(test -n "$$(git -C $(REPO_ROOT) status --porcelain)" && echo yes || echo no)"; \
	  echo "built_at: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	  $(DIST)/near/mihomo -v; \
	  sha256sum $(DIST)/near/mihomo $(DIST)/near/tbt_client \
	            $(DIST)/far/mihomo $(DIST)/far/tbtun.ko; } > $(DIST)/build-info.txt
	cd $(DIST) && find near far -type f ! -name SHA256SUMS -print0 \
	  | sort -z | xargs -0 sha256sum > SHA256SUMS

verify:
	$(DIST)/near/mihomo -t -f $(DIST)/near/config.yaml
	$(DIST)/far/mihomo  -t -f $(DIST)/far/config.yaml
	grep -q 'max-connections: 1' $(DIST)/near/config.yaml
	grep -q 'only-tcp: false' $(DIST)/near/config.yaml
	grep -q 'AND,((NETWORK,UDP),(DST-PORT,443)),REJECT' $(DIST)/near/config.yaml
	test -x $(DIST)/near/tbt_client
	modinfo $(DIST)/far/tbtun.ko >/dev/null
	@set -e; \
	kn=$$(sed -n 's/.*crypto_key=\([0-9a-fA-F]*\).*/\1/p' $(DIST)/near/tbtun.conf); \
	kf=$$(sed -n 's/.*crypto_key=\([0-9a-fA-F]*\).*/\1/p' $(DIST)/far/tbtun.conf); \
	nn=$$(sed -n 's/.*[[:space:]]nonce=\([0-9a-fA-F]*\).*/\1/p' $(DIST)/near/tbtun.conf); \
	nf=$$(sed -n 's/.*[[:space:]]nonce=\([0-9a-fA-F]*\).*/\1/p' $(DIST)/far/tbtun.conf); \
	test $${#kn} -eq 64 || { echo "verify: near crypto_key is not 64 hex digits"; exit 1; }; \
	test "$$kn" = "$$kf" || { echo "verify: near/far crypto_key differ"; exit 1; }; \
	test $${#nn} -eq 32 || { echo "verify: near nonce is not 32 hex digits"; exit 1; }; \
	test "$$nn" = "$$nf" || { echo "verify: near/far nonce differ"; exit 1; }; \
	grep -q 'crypto=on' $(DIST)/near/tbtun.conf; \
	grep -q 'crypto=on' $(DIST)/far/tbtun.conf; \
	if grep -q '198\.51\.100\.20' $(REPO_ROOT)/deploy/090/*.yaml.in; then \
	  echo "verify: hardcoded 198.51.100.20 came back in a YAML template"; exit 1; \
	fi; \
	if [ -z "$(strip $(NEAR_IP))" ]; then \
	  if grep -q '198\.51\.100\.20' $(DIST)/near/tbtun.conf $(DIST)/far/tbtun.conf; then \
	    echo "verify: hardcoded 198.51.100.20 came back in a rendered config"; exit 1; \
	  fi; \
	  if grep -q 'peer=' $(DIST)/far/tbtun.conf; then \
	    echo "verify: far peer= present while NEAR_IP is empty"; exit 1; \
	  fi; \
	else \
	  grep -q "peer=$(NEAR_IP):$(NEAR_PORT)" $(DIST)/far/tbtun.conf || { \
	    echo "verify: far peer=$(NEAR_IP):$(NEAR_PORT) missing"; exit 1; }; \
	fi
	@set -e; \
	for side in near far; do \
	  out=$$(mktemp); \
	  rc=0; \
	  timeout 2 $(DIST)/near/tbt_client --config $(DIST)/$$side/tbtun.conf \
	    >$$out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 124 ]; then \
	    echo "verify: $$side/tbtun.conf was rejected or exited early (rc=$$rc)"; \
	    cat $$out; rm -f $$out; exit 1; \
	  fi; \
	  echo "verify: tbt_client accepted $$side/tbtun.conf (shared tbt_parse_line, ran for 2 s)"; \
	  rm -f $$out; \
	done

clean:
	rm -rf $(DIST)

FORCE:
