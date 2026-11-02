#!/usr/bin/env python3
"""Serve app/build/emu over HTTPS with no-cache headers, so the emulator works on a phone.

`ac.audioWorklet` is only defined in a secure context, and a secure context is decided by
scheme and host alone: `https://192.168.31.129` counts even with a self-signed
certificate (after the browser's warning is accepted), while `http://192.168.31.129` does
not. Chrome on a phone therefore reports "undefined is not an object (evaluating
'ac.audioWorklet.addModule')" on the plain-HTTP server and works on this one.

    python3 app/tools/https_emu.py [PORT] [ROOT]

PORT defaults to 8935, ROOT to app/build/emu. The certificate is generated into
app/build/emu-cert/ on first use, with every local IPv4 address in its subjectAltName, so
a phone that reaches this machine by IP does not also need its hostname.
"""
import http.server
import ipaddress
import os
import socket
import ssl
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(APP, "build", "emu"))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8935
CERT_DIR = os.path.join(APP, "build", "emu-cert")


def local_ips():
    """Every IPv4 address this machine answers on, loopback included."""
    ips = {"127.0.0.1"}
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout
        for tok in out.split():
            try:
                ips.add(str(ipaddress.IPv4Address(tok)))
            except ValueError:
                pass
    except OSError:
        pass
    try:                                    # the address that reaches the default route
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.31.1", 9))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ips)


def ensure_cert():
    crt = os.path.join(CERT_DIR, "emu.crt")
    key = os.path.join(CERT_DIR, "emu.key")
    if os.path.exists(crt) and os.path.exists(key):
        return crt, key
    os.makedirs(CERT_DIR, exist_ok=True)
    ips = local_ips()
    san = ",".join(["DNS:localhost"] + [f"IP:{ip}" for ip in ips])
    with tempfile.NamedTemporaryFile("w", suffix=".cnf", delete=False) as f:
        f.write("[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n"
                "[dn]\nCN=fm1-wave emu\n"
                "[v3]\nsubjectAltName=" + san + "\nbasicConstraints=CA:FALSE\n")
        cnf = f.name
    try:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "825",
                        "-keyout", key, "-out", crt, "-config", cnf],
                       check=True, capture_output=True)
    finally:
        os.unlink(cnf)
    print(f"emu-cert {crt}  ({', '.join(ips)})")
    return crt, key


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def end_headers(self):
        # The worklet and the wasm are separate requests that a page reload does not
        # always revalidate; a stale pair (new index.html, old worklet.js) is a black
        # screen with live buttons.
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, format, *args):
        pass                                # a full stdout pipe hangs the server


def main():
    crt, key = ensure_cert()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    with http.server.ThreadingHTTPServer(("0.0.0.0", PORT), H) as srv:
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        print(f"emu  https://127.0.0.1:{PORT}/  ->  {ROOT}")
        for ip in local_ips():
            print(f"     https://{ip}:{PORT}/")
        srv.serve_forever()


if __name__ == "__main__":
    main()
