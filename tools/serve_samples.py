#!/usr/bin/env python3
"""Serve local wasm samples with the isolation required by Emscripten threads."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error(f'Not a directory: {args.directory}')
    server = ThreadingHTTPServer(('127.0.0.1', args.port),
                                partial(Handler, directory=str(args.directory.resolve())))
    print(f'Serving {args.directory} at http://127.0.0.1:{args.port}', flush=True)
    server.serve_forever()
