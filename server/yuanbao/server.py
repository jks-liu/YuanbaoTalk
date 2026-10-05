"""Single event-loop channel owner; media queues isolate slow subscribers."""

import argparse
import asyncio
from collections import Counter, OrderedDict
from dataclasses import dataclass, field, replace
import json
import logging
import secrets
import time

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from .protocol import Packet, Window, ENVELOPE, AUDIO, END

LOG = logging.getLogger("yuanbao")


def now_ms():
    return time.monotonic_ns() // 1_000_000


def hex_id(value, size=8):
    if not isinstance(value, str) or len(value) != size * 2 or value != value.lower():
        raise ValueError("id")
    raw = bytes.fromhex(value)
    if len(raw) != size or not any(raw):
        raise ValueError("id")
    return raw if size == 16 else int(value, 16)


def uid():
    return secrets.randbits(64) or 1


class Reject(Exception):
    pass


class Bucket:
    def __init__(self, rate, burst):
        self.rate, self.burst, self.tokens, self.at = rate, burst, float(burst), now_ms()

    def take(self):
        now = now_ms()
        self.tokens = min(self.burst, self.tokens + (now - self.at) * self.rate / 1000)
        self.at = now
        if self.tokens < 1:
            return False
        self.tokens -= 1
        return True


@dataclass(eq=False)
class Session:
    ws: object
    ident: int = field(default_factory=uid)
    channel: bytes | None = None
    device: int = 0
    boot: int = 0
    subscription: int = 0
    token: str = ""
    media: object = None
    queue: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(maxsize=64))
    controls: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(maxsize=256))
    recent: OrderedDict = field(default_factory=OrderedDict)
    high_request: int = -1
    last_seen: int = field(default_factory=now_ms)
    synced: bool = False
    capability: dict = field(default_factory=dict)
    limiter: Bucket = field(default_factory=lambda: Bucket(20, 20))
    rates: dict = field(default_factory=dict)


@dataclass
class Stream:
    key: tuple
    handle: int
    created: int
    last_audio: int
    proxies: set = field(default_factory=set)
    window: Window = field(default_factory=Window)
    end_sample: int | None = None
    end_at: int = 0
    inactive_at: int = 0


class Relay:
    def __init__(
        self, max_streams=8, max_connections=128, max_channels=64, radio_budget=3, token=None
    ):
        self.max_streams, self.max_connections, self.max_channels = (
            max_streams,
            max_connections,
            max_channels,
        )
        self.radio_budget, self.token = radio_budget, token
        self.epoch = uid()
        self.sessions = {}
        self.streams = {}
        self.handles = {}
        self.tombstones = {}
        self.next_handle = 1
        self.metrics = Counter()
        self.sockets = Counter()

    def event(self, session, message):
        try:
            session.controls.put_nowait(message)
        except asyncio.QueueFull:
            self.metrics["control_overflow"] += 1
            # Closing is handled by this session's writer, not by the broadcaster.
            session.last_seen = 0

    async def control_writer(self, session):
        try:
            while True:
                message = await session.controls.get()
                await asyncio.wait_for(
                    session.ws.send(json.dumps(message, separators=(",", ":"))), 0.5
                )
        except (asyncio.TimeoutError, ConnectionClosed):
            await session.ws.close(code=1001, reason="Slow control consumer")

    def leave(self, session):
        for stream in self.streams.values():
            stream.proxies.discard(session.ident)
            if not stream.proxies and not stream.inactive_at:
                stream.inactive_at = now_ms()
        session.channel = None
        session.token = ""
        session.subscription = 0
        session.rates.clear()
        while not session.queue.empty():
            session.queue.get_nowait()

    @staticmethod
    def capabilities(request):
        maximum = request.get("max_mix_streams")
        effective = request.get("effective_mix_streams")
        mode, target = request.get("mix_mode"), request.get("requested_mix_streams")
        if (
            type(maximum) is not int
            or not 1 <= maximum <= 32
            or type(effective) is not int
            or not 1 <= effective <= maximum
            or mode not in ("auto", "manual")
            or (mode == "auto" and target is not None)
            or (mode == "manual" and (type(target) is not int or not 1 <= target <= maximum))
        ):
            raise Reject("BAD_REQUEST")
        return {
            k: request[k]
            for k in (
                "max_mix_streams",
                "effective_mix_streams",
                "mix_mode",
                "requested_mix_streams",
            )
        }

    def command(self, s, r):
        op = r.get("op")
        if op == "HELLO":
            if s.device:
                raise Reject("BAD_REQUEST")
            if r.get("version") != 1:
                raise Reject("UNSUPPORTED_VERSION")
            if 1 not in r.get("profiles", []):
                raise Reject("UNSUPPORTED_PROFILE")
            if self.token and not secrets.compare_digest(str(r.get("token", "")), self.token):
                raise Reject("UNAUTHORIZED")
            capability = self.capabilities(r)
            device, boot = hex_id(r["device_id"]), hex_id(r["boot_id"])
            if any(x is not s and x.device == device for x in self.sessions.values()):
                raise Reject("CONFLICT")
            s.device, s.boot, s.capability = device, boot, capability
            return dict(
                op="WELCOME",
                session_id=f"{s.ident:016x}",
                server_epoch=f"{self.epoch:016x}",
                heartbeat_ms=10000,
            )
        if not s.device:
            raise Reject("UNAUTHORIZED")
        if op == "TIME_SYNC":
            if type(r.get("c0")) is not int or not 0 <= r["c0"] < 2**53:
                raise Reject("BAD_REQUEST")
            s.synced = True
            return dict(
                op=op, c0=r["c0"], s1=now_ms(), s2=now_ms(), server_epoch=f"{self.epoch:016x}"
            )
        if op == "PING":
            return dict(op="PONG")
        if op == "CAPABILITIES_UPDATE":
            s.capability = self.capabilities(r)
            return dict(op="ACK")
        if op == "JOIN":
            if s.channel:
                raise Reject("BAD_REQUEST")
            channel = hex_id(r["channel_id"], 16)
            if r.get("profile") != 1:
                raise Reject("UNSUPPORTED_PROFILE")
            channels = {x.channel for x in self.sessions.values() if x.channel}
            if channel not in channels and len(channels) >= self.max_channels:
                raise Reject("CAPACITY")
            s.channel, s.subscription, s.token = channel, uid(), secrets.token_hex(24)
            return dict(
                op="JOIN_OK",
                subscription_id=f"{s.subscription:016x}",
                profile=1,
                max_active_streams=self.max_streams,
                radio_budget=self.radio_budget,
                media_token=s.token,
            )
        if not s.channel:
            raise Reject("NOT_JOINED")
        if op == "MEDIA_REBIND":
            s.token = secrets.token_hex(24)
            return dict(
                op="MEDIA_TOKEN", subscription_id=f"{s.subscription:016x}", media_token=s.token
            )
        if op == "LEAVE":
            self.leave(s)
            return dict(op="LEFT")
        if not s.media or not s.synced:
            raise Reject("BAD_BINDING")
        if op == "STREAM_OPEN":
            if r.get("profile") != 1:
                raise Reject("UNSUPPORTED_PROFILE")
            if hex_id(r["channel_id"], 16) != s.channel:
                raise Reject("NOT_JOINED")
            key = (s.channel, hex_id(r["origin_id"]), hex_id(r["boot_id"]), hex_id(r["stream_id"]))
            sample = r.get("first_observed_sample", 0)
            if type(sample) is not int or not 0 <= sample <= 960000 or sample % 320:
                raise Reject("BAD_REQUEST")
            now = now_ms()
            self.expire(now)
            if key in self.tombstones:
                raise Reject("CONFLICT")
            stream = self.streams.get(key)
            if stream and stream.end_sample is not None:
                raise Reject("CONFLICT")
            active = [
                x for x in self.streams.values() if x.key[0] == s.channel and not x.inactive_at
            ]
            if not stream or stream.inactive_at:
                if len(active) >= self.max_streams:
                    raise Reject("STREAM_LIMIT")
                if any(x.key[1] == key[1] and x.key != key for x in active):
                    raise Reject("CONFLICT")
            if stream is None:
                if len(self.streams) + len(self.tombstones) >= 4096:
                    raise Reject("CAPACITY")
                stream = Stream(key, self.next_handle, now, now)
                self.next_handle += 1
                self.streams[key] = stream
                self.handles[stream.handle] = stream
            elif stream.inactive_at:
                stream.last_audio = now
                stream.inactive_at = 0
            stream.proxies.add(s.ident)
            s.rates.setdefault(stream.handle, Bucket(75, 10))
            for other in self.sessions.values():
                if other.channel == s.channel:
                    self.event(other, self.stream_state(stream))
            return dict(
                op="STREAM_OK",
                stream_handle=f"{stream.handle:016x}",
                origin_id=r["origin_id"],
                boot_id=r["boot_id"],
                stream_id=r["stream_id"],
            )
        if op in ("STREAM_CLOSE", "STREAM_KEEP"):
            stream = self.handles.get(hex_id(r["stream_handle"]))
            if not stream or s.ident not in stream.proxies:
                raise Reject("UNKNOWN_STREAM")
            if op == "STREAM_CLOSE":
                stream.proxies.discard(s.ident)
                s.rates.pop(stream.handle, None)
                if not stream.proxies:
                    stream.inactive_at = now_ms()
            return dict(op="ACK")
        raise Reject("BAD_REQUEST")

    @staticmethod
    def stream_state(stream):
        ch, origin, boot, ident = stream.key
        return dict(
            op="STREAM_STATE",
            channel_id=ch.hex(),
            origin_id=f"{origin:016x}",
            boot_id=f"{boot:016x}",
            stream_id=f"{ident:016x}",
            profile=1,
            stream_handle=f"{stream.handle:016x}",
        )

    def request(self, session, request):
        if not isinstance(request, dict):
            return dict(ok=False, error="BAD_REQUEST")
        rid = request.get("request_id")
        if type(rid) is not int or not 0 <= rid < 2**32:
            return dict(ok=False, error="BAD_REQUEST")
        encoded = json.dumps(request, sort_keys=True, separators=(",", ":"))
        if rid in session.recent:
            old, response, _ = session.recent[rid]
            return (
                response if old == encoded else dict(ok=False, request_id=rid, error="BAD_REQUEST")
            )
        if rid <= session.high_request:
            return dict(ok=False, request_id=rid, error="STALE_REQUEST")
        # Keep 30 seconds even at 20 rps (128 alone is insufficient).
        while (
            session.recent
            and next(iter(session.recent.values()))[2] < now_ms() - 30000
            and len(session.recent) >= 128
        ):
            session.recent.popitem(last=False)
        if len(session.recent) >= 640 or not session.limiter.take():
            return dict(ok=False, request_id=rid, error="RATE_LIMIT")
        session.high_request = rid
        try:
            response = dict(ok=True, request_id=rid, **self.command(session, request))
        except Reject as exc:
            response = dict(ok=False, request_id=rid, error=str(exc))
        except (KeyError, ValueError, TypeError, OverflowError):
            response = dict(ok=False, request_id=rid, error="BAD_REQUEST")
        session.recent[rid] = encoded, response, now_ms()
        return response

    def expire(self, now):
        self.tombstones = {k: t for k, t in self.tombstones.items() if t > now}
        for key, stream in list(self.streams.items()):
            ended = stream.end_at and now >= stream.end_at + 300
            timed_out = now - stream.created >= 60000
            if ended or timed_out:
                self.tombstones[key] = now + 60000
                self.remove_stream(stream)
            elif not stream.inactive_at and now - stream.last_audio >= 1000:
                stream.inactive_at = now
                stream.proxies.clear()
                for s in self.sessions.values():
                    s.rates.pop(stream.handle, None)
            elif stream.inactive_at and now - stream.inactive_at >= 2000:
                self.remove_stream(stream)

    def remove_stream(self, stream):
        self.streams.pop(stream.key, None)
        self.handles.pop(stream.handle, None)
        for session in self.sessions.values():
            session.rates.pop(stream.handle, None)

    def media_packet(self, session, raw):
        try:
            if not isinstance(raw, bytes) or not 88 <= len(raw) <= 274:
                raise ValueError("length")
            sid, handle, deadline = ENVELOPE.unpack_from(raw)
            now = now_ms()
            self.expire(now)
            stream = self.handles.get(handle)
            if (
                sid != session.ident
                or not session.channel
                or not session.synced
                or stream is None
                or stream.inactive_at
                or session.ident not in stream.proxies
                or not now < deadline <= now + 400
            ):
                raise ValueError("binding/deadline")
            if not session.rates[handle].take():
                self.metrics["media_rate_limit"] += 1
                return
            packet = Packet.decode(raw[24:])
            if packet.flags or packet.key != stream.key or packet.channel != session.channel:
                raise ValueError("source/channel/loop")
            if stream.end_sample is not None:
                if packet.kind == 1:
                    raise ValueError("start after end")
                if packet.kind == AUDIO and packet.sample >= stream.end_sample:
                    raise ValueError("after end")
                if packet.kind == END and packet.sample != stream.end_sample:
                    raise ValueError("end conflict")
            if not stream.window.accept(packet):
                self.metrics["duplicates"] += 1
                return
            if packet.kind == AUDIO:
                stream.last_audio = now
            elif packet.kind == END:
                stream.end_sample, stream.end_at = packet.sample, now
            output = replace(packet, flags=1, ttl=self.radio_budget).encode()
            self.metrics["accepted"] += 1
            for target in self.sessions.values():
                if (
                    target is session
                    or target.channel != session.channel
                    or not target.media
                    or not target.synced
                ):
                    continue
                while not target.queue.empty() and (
                    target.queue.qsize() >= 64 or now - target.queue._queue[0][0] >= 100
                ):
                    target.queue.get_nowait()
                    self.metrics["queue_drop"] += 1
                target.queue.put_nowait(
                    (now, deadline, ENVELOPE.pack(target.ident, handle, deadline) + output)
                )
        except (ValueError, KeyError):
            self.metrics["invalid_media"] += 1

    async def media_writer(self, session, ws):
        try:
            while session.media is ws:
                queued, deadline, raw = await session.queue.get()
                if now_ms() - queued >= 100 or now_ms() >= deadline:
                    self.metrics["stale_media"] += 1
                    continue
                await asyncio.wait_for(ws.send(raw), 0.5)
        except (asyncio.TimeoutError, ConnectionClosed):
            await ws.close(code=1001, reason="Slow media consumer")

    async def media_connection(self, ws):
        session = None
        writer = None
        try:
            raw = await asyncio.wait_for(ws.recv(), 5)
            bind = json.loads(raw) if isinstance(raw, str) else {}
            session = self.sessions.get(hex_id(bind["session_id"]))
            if (
                not session
                or not session.channel
                or not session.synced
                or bind.get("op") != "BIND"
                or hex_id(bind["subscription_id"]) != session.subscription
                or not session.token
                or not secrets.compare_digest(str(bind.get("media_token", "")), session.token)
            ):
                raise ValueError("binding")
            session.token = ""
            bound_subscription = session.subscription
            if session.media:
                await session.media.close(code=1000, reason="Rebound")
            if (
                session.ident not in self.sessions
                or not session.channel
                or session.subscription != bound_subscription
            ):
                raise ValueError("subscription changed during binding")
            while not session.queue.empty():
                session.queue.get_nowait()
            session.media = ws
            self.event(
                session,
                dict(
                    op="READY",
                    session_id=f"{session.ident:016x}",
                    subscription_id=f"{session.subscription:016x}",
                ),
            )
            for stream in self.streams.values():
                if stream.key[0] == session.channel and not stream.inactive_at:
                    self.event(session, self.stream_state(stream))
            writer = asyncio.create_task(self.media_writer(session, ws))
            async for message in ws:
                if session.media is not ws or not session.channel:
                    break
                if writer.done():
                    await writer
                self.media_packet(session, message)
        except (KeyError, ValueError, TypeError, asyncio.TimeoutError):
            await ws.close(code=1008, reason="Invalid or stalled media")
        finally:
            if writer:
                writer.cancel()
                await asyncio.gather(writer, return_exceptions=True)
            if session and session.media is ws:
                session.media = None

    async def handler(self, ws):
        peer = ws.remote_address[0] if ws.remote_address else "unknown"
        if sum(self.sockets.values()) >= self.max_connections * 2 or self.sockets[peer] >= 64:
            await ws.close(code=1008, reason="Connection capacity")
            return
        self.sockets[peer] += 1
        try:
            await self.handle_connection(ws)
        finally:
            self.sockets[peer] -= 1
            if not self.sockets[peer]:
                del self.sockets[peer]

    async def handle_connection(self, ws):
        path = ws.request.path
        if path == "/v1/media":
            try:
                await self.media_connection(ws)
            except ConnectionClosed:
                pass
            return
        if path != "/v1/control" or len(self.sessions) >= self.max_connections:
            await ws.close(code=1008, reason="Route/capacity")
            return
        session = Session(ws)
        self.sessions[session.ident] = session
        writer = asyncio.create_task(self.control_writer(session))
        try:
            async for raw in ws:
                if writer.done():
                    await writer
                session.last_seen = now_ms()
                try:
                    if not isinstance(raw, str) or len(raw.encode()) > 4096:
                        raise ValueError("control")
                    message = json.loads(raw)
                    response = self.request(session, message)
                except (ValueError, TypeError):
                    response = dict(ok=False, error="BAD_REQUEST")
                self.event(session, response)
                if session.channel is None and session.media:
                    await session.media.close()
        except (ConnectionClosed, asyncio.TimeoutError):
            pass
        finally:
            self.leave(session)
            self.sessions.pop(session.ident, None)
            if session.media:
                await session.media.close()
            writer.cancel()
            await asyncio.gather(writer, return_exceptions=True)

    async def maintenance(self):
        while True:
            await asyncio.sleep(0.1)
            self.expire(now_ms())
            stale = [s for s in self.sessions.values() if now_ms() - s.last_seen > 30000]
            if stale:
                await asyncio.gather(
                    *(
                        asyncio.wait_for(s.ws.close(code=1001, reason="Heartbeat timeout"), 2)
                        for s in stale
                    ),
                    return_exceptions=True,
                )


async def run(args):
    relay = Relay(args.streams, args.connections, radio_budget=args.ttl, token=args.token)
    async with serve(
        relay.handler,
        args.host,
        args.port,
        max_size=4096,
        compression=None,
        max_queue=16,
        write_limit=4096,
        ping_interval=10,
        ping_timeout=20,
    ):
        LOG.info("Listening on %s:%s", args.host, args.port)
        await relay.maintenance()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--streams", type=int, default=8)
    parser.add_argument("--connections", type=int, default=128)
    parser.add_argument("--ttl", type=int, choices=range(1, 17), default=3)
    parser.add_argument("--token", default=None)
    args = parser.parse_args()
    if args.streams < 1 or args.connections < 1:
        parser.error("capacity must be positive")
    logging.basicConfig(level=logging.INFO)
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
