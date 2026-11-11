#!/bin/sh
# find-near.sh - find the near board after DHCP moved its IPv4 address.
#
# The board keeps its MAC and SSH host key; only the lease changes. This
# script probes the LAN read-only and prints one IPv4 address on stdout.
#
# Methods, cheapest first:
#   1. local neighbour table (ip neigh) for the known MAC
#   2. DHCP lease files (dnsmasq or ISC), local or over read-only router SSH
#   3. ICMP sweep of the private /24, then re-read the neighbour table
#   4. host-key check with ssh-keyscan (no login, no known_hosts write)
#
# Safety:
#   * only a private /24 is accepted; no other range is scanned
#   * no sudo, no route/lease/hosts/known_hosts writes, no SSH login
#   * /dev/watchdog0 is never opened (open() would arm it)
#   * every probe has a timeout; the whole run has a deadline
#
# Exit: 0 one address, 1 none, 2 ambiguous, 3 usage or safety error.
set -eu

SUBNET=192.168.123.0/24
MAC=02:51:22:e5:67:53
FINGERPRINT=SHA256:MbBBsT5vgc7yzPIYndTWY6PnNwxGnMu1Dw9Bf4Xd82w
IFACE=
TIMEOUT=60
SWEEP=1
STRICT_KEY=0
ROUTER=
LEASE_FILE=

usage() {
    cat <<'EOF'
usage: find-near.sh [options]

  --subnet CIDR        private /24 to scan (default 192.168.123.0/24)
  --mac MAC            near MAC (default 02:51:22:e5:67:53)
  --fingerprint FP     expected SSH host key (default: near ed25519)
  --no-key-check       skip the SSH host-key check
  --strict-key         reject a candidate whose host key is absent/mismatched
  --iface NAME         interface for ip neigh (default: route to subnet)
  --router USER@HOST   read DHCP leases over read-only SSH
  --lease-file PATH    read a local dnsmasq/ISC lease file
  --no-sweep           do not ICMP-sweep the /24
  --timeout SECONDS    overall deadline (default 60)
  -h, --help           this text

stdout: one IP address. stderr: evidence and candidates.
EOF
}

die() { printf 'find-near: %s\n' "$*" >&2; exit 3; }
warn() { printf 'find-near: %s\n' "$*" >&2; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --subnet) [ "$#" -ge 2 ] || die "--subnet needs a value"; SUBNET=$2; shift 2 ;;
        --mac) [ "$#" -ge 2 ] || die "--mac needs a value"; MAC=$2; shift 2 ;;
        --fingerprint) [ "$#" -ge 2 ] || die "--fingerprint needs a value"; FINGERPRINT=$2; shift 2 ;;
        --no-key-check) FINGERPRINT=; shift ;;
        --strict-key) STRICT_KEY=1; shift ;;
        --iface) [ "$#" -ge 2 ] || die "--iface needs a value"; IFACE=$2; shift 2 ;;
        --router) [ "$#" -ge 2 ] || die "--router needs a value"; ROUTER=$2; shift 2 ;;
        --lease-file) [ "$#" -ge 2 ] || die "--lease-file needs a value"; LEASE_FILE=$2; shift 2 ;;
        --no-sweep) SWEEP=0; shift ;;
        --timeout) [ "$#" -ge 2 ] || die "--timeout needs a value"; TIMEOUT=$2; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$TIMEOUT" in
    '' | *[!0-9]*) die "--timeout must be a positive integer" ;;
esac
[ "$TIMEOUT" -ge 1 ] || die "--timeout must be >= 1"

case "$SUBNET" in
    10.*/* | 192.168.*/* | 172.1[6-9].*/* | 172.2[0-9].*/* | 172.3[0-1].*/*) ;;
    *) die "refusing non-private subnet: $SUBNET" ;;
esac
case "${SUBNET#*/}" in
    24) ;;
    *) die "only a /24 is supported: $SUBNET" ;;
esac

NET=${SUBNET%/*}
printf '%s' "$NET" | grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){3}$' \
    || die "bad IPv4 network: $NET"
BASE=${NET%.*}

MAC=$(printf '%s' "$MAC" | tr '[:upper:]' '[:lower:]')
case "$MAC" in
    [0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]) ;;
    *) die "bad MAC: $MAC" ;;
esac

[ -z "$LEASE_FILE" ] || [ -r "$LEASE_FILE" ] || die "lease file is unreadable: $LEASE_FILE"

if [ -z "$IFACE" ]; then
    IFACE=$(ip -4 route show "$SUBNET" 2>/dev/null \
        | awk 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
fi
if [ -z "$IFACE" ]; then
    IFACE=$(ip -4 route show default 2>/dev/null \
        | awk 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
fi
[ -n "$IFACE" ] || die "cannot determine the interface; use --iface"
warn "subnet=$SUBNET iface=$IFACE mac=$MAC timeout=${TIMEOUT}s"

TMP=$(mktemp -d 2>/dev/null) || die "mktemp failed"
trap 'rm -rf "$TMP"' 0 1 2 15
: >"$TMP/candidates"

add_candidate() {
    ip=$1
    why=$2
    if grep -qxF "$ip" "$TMP/candidates" 2>/dev/null; then
        return 0
    fi
    case "$ip" in
        [0-9]*.[0-9]*.[0-9]*.[0-9]*) ;;
        *) warn "ignoring malformed lease address: $ip"; return 0 ;;
    esac
    printf '%s\n' "$ip" >>"$TMP/candidates"
    warn "candidate $ip ($why)"
}

arp_candidates() {
    # ip neigh drops the "dev" column when a device filter is given, so parse
    # the unfiltered table by keyword and filter the device in awk.
    ip -4 neigh show 2>/dev/null | awk -v want="$MAC" -v wantdev="$IFACE" '
        {
            dev = ""
            mac = ""
            state = ""
            for (i = 1; i <= NF; i++) {
                if ($i == "dev") dev = $(i + 1)
                if ($i == "lladdr") {
                    mac = tolower($(i + 1))
                    state = $(i + 2)
                }
            }
            if (mac == want && (wantdev == "" || dev == wantdev))
                print $1, state
        }
    ' | while read -r ip state; do
        case "$state" in
            FAILED | INCOMPLETE) ;;
            *) add_candidate "$ip" "arp $state" ;;
        esac
    done
}

parse_leases() {
    file=$1
    [ -r "$file" ] || return 0
    # dnsmasq: <expiry> <mac> <ip> <hostname> <client-id>
    while read -r _expiry mac ip _rest; do
        [ -n "${mac:-}" ] || continue
        mac=$(printf '%s' "$mac" | tr '[:upper:]' '[:lower:]')
        [ "$mac" = "$MAC" ] && add_candidate "$ip" "dhcp lease $file"
    done <"$file"
    # ISC dhcpd: lease <ip> { ... hardware ethernet <mac>; ... }
    awk -v want="$MAC" '
        /^lease [0-9]/ { ip = $2 }
        /hardware ethernet/ {
            mac = tolower($3)
            gsub(/;/, "", mac)
            if (mac == want && ip != "") print ip
        }
    ' "$file" | while read -r ip; do
        add_candidate "$ip" "dhcpd lease $file"
    done
}

router_leases() {
    [ -n "$ROUTER" ] || return 0
    warn "reading DHCP leases from $ROUTER (read-only SSH)"
    if timeout 10 ssh -o BatchMode=yes -o ConnectTimeout=5 \
        -o StrictHostKeyChecking=yes "$ROUTER" \
        'for f in /tmp/dhcp.leases /var/lib/misc/dnsmasq.leases /var/lib/dhcp/dhcpd.leases; do [ -r "$f" ] && cat "$f"; done' \
        >"$TMP/router.leases" 2>/dev/null; then
        parse_leases "$TMP/router.leases"
    else
        warn "router SSH failed or its host key is not trusted; continuing"
    fi
}

sweep_once() {
    command -v ping >/dev/null 2>&1 || { warn "ping missing; sweep skipped"; return 0; }
    warn "ICMP sweep $NET (32 at a time)"
    i=1
    n=0
    while [ "$i" -le 254 ]; do
        ping -c 1 -W 1 -n -q "$BASE.$i" >/dev/null 2>&1 &
        n=$((n + 1))
        if [ "$n" -ge 32 ]; then
            wait || true
            n=0
        fi
        i=$((i + 1))
    done
    wait || true
}

tcp22_open() {
    ip=$1
    if command -v nc >/dev/null 2>&1; then
        nc -z -w 1 "$ip" 22 >/dev/null 2>&1
        return $?
    fi
    if command -v bash >/dev/null 2>&1; then
        timeout 2 bash -c "exec 3<>/dev/tcp/$ip/22" >/dev/null 2>&1
        return $?
    fi
    return 2
}

key_fp() {
    ip=$1
    command -v ssh-keyscan >/dev/null 2>&1 || return 2
    command -v ssh-keygen >/dev/null 2>&1 || return 2
    key=$(timeout 4 ssh-keyscan -T 2 -t ed25519 "$ip" 2>/dev/null) || return 1
    [ -n "$key" ] || return 1
    printf '%s\n' "$key" | ssh-keygen -lf - 2>/dev/null | awk 'NR == 1 { print $2 }'
}

check_key() {
    ip=$1
    [ -n "$FINGERPRINT" ] || return 0
    if fp=$(key_fp "$ip"); then
        [ -n "$fp" ] || fp=unknown
        if [ "$fp" = "$FINGERPRINT" ]; then
            warn "$ip host key matches $FINGERPRINT"
            return 0
        fi
        if [ "$STRICT_KEY" -eq 1 ]; then
            warn "$ip host key $fp != $FINGERPRINT; rejected"
            return 1
        fi
        warn "$ip host key $fp != $FINGERPRINT; MAC match only"
        return 0
    fi
    if [ "$STRICT_KEY" -eq 1 ]; then
        warn "$ip host key unreadable; rejected by --strict-key"
        return 1
    fi
    warn "$ip host key unreadable; MAC match only"
    return 0
}

pick_candidate() {
    n=$(wc -l <"$TMP/candidates")
    [ "$n" -gt 0 ] || return 1
    if [ "$n" -eq 1 ]; then
        ip=$(sed -n '1p' "$TMP/candidates")
        check_key "$ip" || return 1
        printf '%s\n' "$ip"
        return 0
    fi

    : >"$TMP/verified"
    : >"$TMP/unverified"
    while read -r ip; do
        if [ -z "$FINGERPRINT" ]; then
            printf '%s\n' "$ip" >>"$TMP/unverified"
            continue
        fi
        if fp=$(key_fp "$ip"); then
            if [ "$fp" = "$FINGERPRINT" ]; then
                printf '%s\n' "$ip" >>"$TMP/verified"
                warn "verified $ip by host key"
            else
                warn "rejected $ip: host key $fp != $FINGERPRINT"
            fi
        else
            if [ "$STRICT_KEY" -eq 1 ]; then
                warn "rejected $ip: host key unreadable"
            else
                printf '%s\n' "$ip" >>"$TMP/unverified"
            fi
        fi
    done <"$TMP/candidates"

    vn=$(wc -l <"$TMP/verified")
    if [ "$vn" -eq 1 ]; then
        sed -n '1p' "$TMP/verified"
        return 0
    fi
    if [ "$vn" -gt 1 ]; then
        warn "multiple IPs carry the expected host key:"
        while read -r ip; do warn "  $ip"; done <"$TMP/verified"
        return 2
    fi

    : >"$TMP/tcp22"
    while read -r ip; do
        if tcp22_open "$ip"; then
            printf '%s\n' "$ip" >>"$TMP/tcp22"
        fi
    done <"$TMP/unverified"
    tn=$(wc -l <"$TMP/tcp22")
    if [ "$tn" -eq 1 ]; then
        sed -n '1p' "$TMP/tcp22"
        return 0
    fi
    if [ "$tn" -gt 1 ]; then
        warn "multiple candidates accept TCP/22:"
        while read -r ip; do warn "  $ip"; done <"$TMP/tcp22"
        return 2
    fi

    warn "candidates without a unique identity:"
    while read -r ip; do warn "  $ip"; done <"$TMP/candidates"
    return 2
}

if [ -n "$LEASE_FILE" ]; then
    parse_leases "$LEASE_FILE"
fi
router_leases

DEADLINE=$(( $(date +%s) + TIMEOUT ))
FIRST=1
while :; do
    arp_candidates
    [ -s "$TMP/candidates" ] && break

    if [ "$FIRST" -eq 1 ] && [ "$SWEEP" -eq 1 ]; then
        sweep_once
        sleep 1
        arp_candidates
        FIRST=0
        [ -s "$TMP/candidates" ] && break
    fi

    NOW=$(date +%s)
    [ "$NOW" -ge "$DEADLINE" ] && break
    if [ "$SWEEP" -eq 1 ]; then
        sweep_once
        sleep 1
        arp_candidates
        [ -s "$TMP/candidates" ] && break
    else
        sleep 2
    fi
    [ "$(date +%s)" -ge "$DEADLINE" ] && break
done

if PICKED=$(pick_candidate); then
    printf '%s\n' "$PICKED"
    exit 0
else
    RC=$?
fi

if [ "$RC" -eq 2 ]; then
    warn "ambiguous; use --strict-key, a router lease, or a narrower probe"
    exit 2
fi

warn "no candidate for $MAC on $SUBNET within ${TIMEOUT}s"
warn "next steps:"
warn "  ip -4 neigh show | grep -i $MAC"
warn "  $0 --router root@192.168.123.254 --timeout 120"
warn "  check the router DHCP lease table for ${MAC}"
warn "  confirm the board is powered and on the same L2 segment"
exit 1
