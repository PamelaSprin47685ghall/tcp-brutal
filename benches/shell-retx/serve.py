#!/usr/bin/env python3
# Serve one file to one connection. Host and port are explicit so the same
# script works inside the app namespace and on loopback.
import socket, sys
host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((host, port)); s.listen(1)
c, _ = s.accept()
with open(path, "rb") as f:
    while True:
        b = f.read(1 << 20)
        if not b:
            break
        c.sendall(b)
c.shutdown(socket.SHUT_WR)
c.close(); s.close()
