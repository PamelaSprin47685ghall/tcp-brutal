#!/usr/bin/env python3
# Read exactly WANT bytes from the tunnel and write them to OUT.
import hashlib, socket, sys, time
host, port, want, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
t0 = time.time()
s = socket.create_connection((host, port), timeout=30)
s.settimeout(180)
h = hashlib.sha256(); got = 0
with open(out, "wb") as f:
    while got < want:
        b = s.recv(min(1 << 20, want - got))
        if not b:
            break
        f.write(b); h.update(b); got += len(b)
print(f"got={got} sha={h.hexdigest()} seconds={time.time()-t0:.3f}")
