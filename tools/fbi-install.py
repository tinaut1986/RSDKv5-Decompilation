#!/usr/bin/env python3
"""Sends a CIA to FBI's Remote Install ("Receive URLs over the network") when the game's own FTP is not available
(a build whose network does not start). Same protocol as FBI's servefiles.py: serve the file over HTTP from this PC,
send its URL to the console on port 5000 (4-byte big-endian length, then the URL), wait until FBI has fetched it.

  tools/fbi-install.py <console IP> [file.cia]   (default: 3ds/out/SonicMania.cia)
"""
import http.server
import os
import socket
import struct
import sys
import threading
import urllib.parse

if len(sys.argv) < 2:
    sys.exit(__doc__)
target = sys.argv[1]
path = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "3ds/out/SonicMania.cia")
directory, name = os.path.split(path)

# This PC's address on the console's network
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe.connect((target, 5000))
host = probe.getsockname()[0]
probe.close()

done = threading.Event()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=directory, **k)

    def log_message(self, fmt, *args):
        print("http:", fmt % args, flush=True)

    def do_GET(self):
        super().do_GET()
        if urllib.parse.unquote(self.path).lstrip("/") == name:
            done.set()


server = http.server.ThreadingHTTPServer((host, 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = "http://%s:%d/%s" % (host, server.server_address[1], urllib.parse.quote(name))
print("serving", url, flush=True)

payload = url.encode("ascii")
# FBI listens only while "Receive URLs over the network" is open: keep trying for ten minutes.
import time
for attempt in range(300):
    try:
        conn = socket.create_connection((target, 5000), timeout=10)
        break
    except OSError:
        if attempt == 0:
            print("waiting for FBI (Remote Install > Receive URLs over the network)...", flush=True)
        time.sleep(2)
else:
    sys.exit("FBI never listened on %s:5000" % target)
with conn as s:
    s.sendall(struct.pack("!L", len(payload)) + payload)
    print("sent to FBI at %s; accept the install on the console" % target, flush=True)
    s.settimeout(None)
    s.recv(1)  # FBI answers once it has the list

done.wait(600)
# Let the transfer finish before the server goes away
for t in threading.enumerate():
    if t is not threading.current_thread() and not t.daemon:
        t.join(600)
server.shutdown()
print("done" if done.is_set() else "FBI did not fetch the file", flush=True)
