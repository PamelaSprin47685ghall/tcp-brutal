# deploy/090 findings

Two production findings from HND0. Both must survive a reinstall.

## 1. HND0 egress is routed through WARP (table 50000)

On HND0 the IPv4 default route is owned by `warp-go` in routing table 50000.
A route lookup from the far host therefore selects the WARP interface:

    ip route get <target>    # ... dev WARP table 50000

Symptom: the far side's mihomo egress leaves with WARP's source address, not
with the tunnel host's own `eth0` address. Traffic that must originate from
the host's own address then takes the wrong path, and any peer that checks the
source address sees WARP's.

Required line in the far mihomo config (`/etc/mihomo-090/config.yaml`):

    interface-name: eth0

`deploy/090/far.yaml.in` has no egress-interface key on this branch, so the
template does not pin it; add the line above to the deployed far config.

## 2. Near-client restart no longer collides with tombstones

The far side drops an OPEN that names a tombstoned stream SID for 2 s, which
blackholed the first request after a fast client restart. That is fixed at the
client and kernel level:

- The client derives its first stream SID per incarnation from the HELLO epoch
  and a CSPRNG seed, so a restarted client does not name the retired first SID.
- The far side clears tombstones from the previous incarnation once the
  authenticated new HELLO generation is accepted.

Therefore no `ExecStartPre=/bin/sleep 3` or other startup delay is needed in
`tbt-client-090.service`; none is present.
