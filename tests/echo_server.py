#!/usr/bin/env python3
"""Minimal WebSocket echo server for testing ws_client."""
import asyncio, sys, websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001

async def echo(ws):
    async for msg in ws:
        await ws.send(msg)

async def main():
    async with websockets.serve(echo, "0.0.0.0", PORT):
        print(f"WS echo server on ws://0.0.0.0:{PORT}/")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
