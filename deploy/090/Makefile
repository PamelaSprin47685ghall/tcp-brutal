# deploy/090/Makefile
#
# Build one deployable set from this tree without patching mihomo:
#   - vanilla mihomo, downloaded from the upstream GitHub release;
#   - the 0.8 userspace client (host/tbt_client) for the near side;
#   - tbtun.ko from the existing Kbuild rules for the far side.
#
# Usage:
#   make -f deploy/090/Makefile package
#   make -f deploy/090/Makefile package MIHOMO_VERSION=v1.19.34 MIHOMO_SHA256=<hex>
#   make -f deploy/090/Makefile configs UUID=<uuid> DIST=/tmp/tbt-090
#
# Upgrading mihomo is one variable: MIHOMO_VERSION.
# Leaving it unset resolves the latest upstream release tag.

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

UUID         ?= $(shell cat /proc/sys/kernel/random/uuid)
NEAR_PORT    ?= 19000
FAR_PORT     ?= 19002
VLESS_PORT   ?= 19001
MIXED_PORT   ?= 7890
FAR_IP       ?= 203.0.113.10
NEAR_IP      ?= 198.51.100.20

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

# 0.8 userspace client. Makefile:865-866 owns the link rule.
$(DIST)/near/tbt_client: FORCE
	@mkdir -p $(dir $@)
	$(MAKE) -C $(REPO_ROOT) host/tbt_client
	install -m 0755 $(REPO_ROOT)/host/tbt_client $@

client: $(DIST)/near/tbt_client

# Kernel module: far side only.
$(DIST)/far/tbtun.ko: FORCE
	@mkdir -p $(dir $@)
	@test -d "$(KERNEL_DIR)" || { \
	  echo "KERNEL_DIR=$(KERNEL_DIR) not found; see docs/build-env.md"; \
	  exit 1; }
	$(MAKE) -C $(REPO_ROOT) KERNEL_DIR=$(KERNEL_DIR) all
	install -m 0644 $(REPO_ROOT)/tbtun.ko $@
	modinfo $@ | sed -n '1,8p'

module: $(DIST)/far/tbtun.ko

$(DIST)/near/config.yaml: $(REPO_ROOT)/deploy/090/near.yaml.in
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(NEAR_PORT)/g' \
	    -e 's/@MIXED_PORT@/$(MIXED_PORT)/g' $< > $@

$(DIST)/far/config.yaml: $(REPO_ROOT)/deploy/090/far.yaml.in
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(VLESS_PORT)/g' $< > $@

configs: $(DIST)/near/config.yaml $(DIST)/far/config.yaml

package-near: mihomo client $(DIST)/near/config.yaml
	install -m 0755 $(DIST)/mihomo $(DIST)/near/mihomo
	printf '%s\n' \
	  "tunnel 1 role=near listen=127.0.0.1:$(NEAR_PORT) peer=$(FAR_IP):$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on adaptive=off" \
	  > $(DIST)/near/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-near.sh $(DIST)/near/install-near.sh

package-far: mihomo module $(DIST)/far/config.yaml
	install -m 0755 $(DIST)/mihomo $(DIST)/far/mihomo
	printf '%s\n' \
	  "tunnel 1 role=far listen=0.0.0.0:$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on adaptive=off peer=$(NEAR_IP):$(NEAR_PORT)" \
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
	cd $(DIST) && sha256sum near/* far/* > SHA256SUMS

verify:
	$(DIST)/near/mihomo -t -f $(DIST)/near/config.yaml
	$(DIST)/far/mihomo  -t -f $(DIST)/far/config.yaml
	grep -q 'max-connections: 1' $(DIST)/near/config.yaml
	grep -q 'only-tcp: false' $(DIST)/near/config.yaml
	test -x $(DIST)/near/tbt_client
	modinfo $(DIST)/far/tbtun.ko >/dev/null

clean:
	rm -rf $(DIST)

FORCE:
