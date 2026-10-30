"""Run the C tests against a fixture server owned by this invocation."""

import os
from pathlib import Path
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import urlsplit


class FixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/auth" and self.headers.get("Authorization") != "Bearer test-token_123":
            status, body = 401, b"authentication required"
        elif path in ("/index.txt", "/auth"):
            status, body = 200, Path(__file__).with_name("index.txt").read_bytes()
        else:
            status, body = 404, b"not found"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def main():
    with ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(os.environ, AIQUOTA_TEST_BASE=f"http://127.0.0.1:{server.server_port}")
        try:
            return subprocess.run(sys.argv[1:], env=env, check=False).returncode
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    sys.exit(main())
