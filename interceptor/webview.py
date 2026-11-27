"""
webview: makes the Interceptor's dashboard reachable from the Windows browser.

The dashboard runs on QNX (192.168.122.158:8081), which only WSL can reach.
This forwards WSL port 8090 to it; WSL passes localhost ports to Windows, so
open  http://localhost:8090  in Chrome or Edge.

Usage: python3 webview.py [--port 8090] [--qnx 192.168.122.158] [--qnx-port 8081]
License: MIT
"""
import argparse
import asyncio
import os

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8090)
ap.add_argument("--qnx", default=os.environ.get("QNX_IP", "192.168.122.158"))
ap.add_argument("--qnx-port", type=int, default=8081)
args = ap.parse_args()


async def pipe(r, w):
    try:
        while True:
            data = await r.read(65536)
            if not data:
                break
            w.write(data)
            await w.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        w.close()


async def client(cr, cw):
    try:
        qr, qw = await asyncio.open_connection(args.qnx, args.qnx_port)
    except OSError:
        msg = b"Interceptor on QNX is not reachable. Is start.sh running?\n"
        cw.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(msg) + msg)
        await cw.drain()
        cw.close()
        return
    await asyncio.gather(pipe(cr, qw), pipe(qr, cw))


async def main():
    server = await asyncio.start_server(client, "0.0.0.0", args.port)
    print("Dashboard: http://localhost:%d  (-> %s:%d)" % (args.port, args.qnx, args.qnx_port), flush=True)
    async with server:
        await server.serve_forever()

asyncio.run(main())
