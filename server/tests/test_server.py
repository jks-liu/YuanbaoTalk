import asyncio
import json
import unittest
from dataclasses import replace
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from yuanbao.server import Relay, now_ms, Session
from yuanbao.protocol import ENVELOPE, Packet, END, channel_id
from test_protocol import packet


class Client:
    async def start(self, base, device, channel="1"):
        self.control = await connect(base + "/v1/control", compression=None)
        self.rid = 0
        self.events = []
        self.channel = channel_id(channel)
        r = await self.rpc(
            "HELLO",
            version=1,
            device_id=f"{device:016x}",
            boot_id=f"{device + 100:016x}",
            profiles=[1],
            max_mix_streams=4,
            effective_mix_streams=2,
            mix_mode="auto",
            requested_mix_streams=None,
        )
        self.sid = int(r["session_id"], 16)
        await self.rpc("TIME_SYNC", c0=now_ms())
        self.join = await self.rpc("JOIN", channel_id=self.channel.hex(), profile=1)
        self.media = await connect(base + "/v1/media", compression=None)
        await self.media.send(
            json.dumps(
                dict(
                    op="BIND",
                    session_id=f"{self.sid:016x}",
                    subscription_id=self.join["subscription_id"],
                    media_token=self.join["media_token"],
                )
            )
        )
        while True:
            r = json.loads(await asyncio.wait_for(self.control.recv(), 1))
            if r.get("op") == "READY":
                break
        return self

    async def rpc(self, op, **kwargs):
        self.rid += 1
        await self.control.send(json.dumps(dict(op=op, request_id=self.rid, **kwargs)))
        while True:
            r = json.loads(await asyncio.wait_for(self.control.recv(), 1))
            if r.get("request_id") == self.rid:
                return r
            self.events.append(r)

    async def open(self, p):
        return await self.rpc(
            "STREAM_OPEN",
            channel_id=p.channel.hex(),
            origin_id=f"{p.origin:016x}",
            boot_id=f"{p.boot:016x}",
            stream_id=f"{p.stream:016x}",
            profile=1,
            first_observed_sample=p.sample,
        )

    async def send(self, p, handle, deadline=None):
        await self.media.send(
            ENVELOPE.pack(self.sid, handle, deadline or now_ms() + 250) + p.encode()
        )

    async def recv(self):
        return await asyncio.wait_for(self.media.recv(), 1)

    async def empty(self):
        with unittest.TestCase().assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(self.media.recv(), 0.05)

    async def close(self):
        await self.media.close()
        await self.control.close()


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.relay = Relay(max_streams=2)
        self.server = await serve(
            self.relay.handler, "127.0.0.1", 0, compression=None, max_size=4096
        )
        self.base = f"ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}"
        self.clients = []

    async def client(self, channel="1"):
        c = await Client().start(self.base, len(self.clients) + 10, channel)
        self.clients.append(c)
        return c

    async def asyncTearDown(self):
        for client in self.clients:
            await client.close()
        self.server.close()
        await self.server.wait_closed()

    async def test_multi_gateway_dedup_channel_isolation_and_no_reupload(self):
        a, b, c, other = (
            await self.client(),
            await self.client(),
            await self.client(),
            await self.client("2"),
        )
        p = packet()
        ha, hb = await a.open(p), await b.open(p)
        self.assertEqual(ha["stream_handle"], hb["stream_handle"])
        handle = int(ha["stream_handle"], 16)
        await a.send(p, handle)
        raw = await c.recv()
        await b.recv()
        sid, _, _ = ENVELOPE.unpack_from(raw)
        out = Packet.decode(raw[24:])
        self.assertEqual(sid, c.sid)
        self.assertEqual(out.flags, 1)
        self.assertEqual(out.ttl, 3)
        await b.send(p, handle)
        await c.empty()
        await other.empty()
        await c.open(p)
        await c.send(replace(out, seq=2, sample=320), handle)
        await a.empty()
        self.assertEqual(self.relay.metrics["accepted"], 1)
        self.assertEqual(self.relay.metrics["duplicates"], 1)

    async def test_concurrency_capacity_and_end_before_tail(self):
        a, b = await self.client(), await self.client()
        p, q = packet(), packet(origin=99, stream=88)
        h = int((await a.open(p))["stream_handle"], 16)
        self.assertTrue((await a.open(q))["ok"])
        self.assertEqual((await a.open(packet(origin=101, stream=102)))["error"], "STREAM_LIMIT")
        await a.send(replace(p, kind=END, seq=4, sample=960, payload=b""), h)
        await b.recv()
        await a.send(replace(p, seq=3, sample=640), h)
        self.assertEqual(Packet.decode((await b.recv())[24:]).sample, 640)
        await a.send(replace(p, seq=5, sample=960), h)
        await b.empty()
        self.relay.expire(now_ms() + 400)
        self.assertEqual((await a.open(p))["error"], "CONFLICT")

    async def test_deadline_duplicate_conflict_and_invalid_does_not_poison(self):
        a, b = await self.client(), await self.client()
        p = packet()
        h = int((await a.open(p))["stream_handle"], 16)
        await a.send(p, h, now_ms() - 1)
        await a.send(replace(p, flags=1), h)
        await a.send(p, h)
        await b.recv()
        await a.send(replace(p, sample=320), h)
        await b.empty()
        self.assertEqual(self.relay.metrics["accepted"], 1)

    async def test_binding_replay_and_leave(self):
        a, b = await self.client(), await self.client()
        replay = await connect(self.base + "/v1/media")
        await replay.send(
            json.dumps(
                dict(
                    op="BIND",
                    session_id=f"{a.sid:016x}",
                    subscription_id=a.join["subscription_id"],
                    media_token=a.join["media_token"],
                )
            )
        )
        await replay.wait_closed()
        self.assertEqual(replay.close_code, 1008)
        p = packet()
        h = int((await a.open(p))["stream_handle"], 16)
        self.assertTrue((await b.rpc("LEAVE"))["ok"])
        await a.send(p, h)
        await b.media.wait_closed()

    async def test_proxy_disconnect_preserves_other_gateway(self):
        a, b, c = await self.client(), await self.client(), await self.client()
        p = packet()
        h = int((await a.open(p))["stream_handle"], 16)
        await b.open(p)
        await a.close()
        await asyncio.sleep(0.02)
        await b.send(p, h)
        self.assertEqual(Packet.decode((await c.recv())[24:]).origin, p.origin)

    async def test_media_rebind_replaces_old_connection(self):
        a, b = await self.client(), await self.client()
        token = await b.rpc("MEDIA_REBIND")
        replacement = await connect(self.base + "/v1/media")
        await replacement.send(
            json.dumps(
                dict(
                    op="BIND",
                    session_id=f"{b.sid:016x}",
                    subscription_id=token["subscription_id"],
                    media_token=token["media_token"],
                )
            )
        )
        await asyncio.wait_for(b.media.wait_closed(), 1)
        b.media = replacement
        # The new binding is effective before READY is sent.
        while True:
            response = json.loads(await asyncio.wait_for(b.control.recv(), 1))
            if response.get("op") == "READY":
                break
        p = packet()
        handle = int((await a.open(p))["stream_handle"], 16)
        await a.send(p, handle)
        self.assertEqual(Packet.decode((await b.recv())[24:]).stream, p.stream)

    async def test_fragmented_control_message(self):
        a = await self.client()
        a.rid += 1
        text = json.dumps(dict(op="PING", request_id=a.rid))
        await a.control.send([text[:8], text[8:]])
        reply = json.loads(await asyncio.wait_for(a.control.recv(), 1))
        self.assertEqual(reply["op"], "PONG")


class BackpressureTests(unittest.IsolatedAsyncioTestCase):
    async def test_stalled_writer_is_closed_without_new_incoming_messages(self):
        class SlowSocket:
            def __init__(self):
                self.closed = False

            async def send(self, raw):
                await asyncio.Event().wait()

            async def close(self, **kwargs):
                self.closed = True

        relay = Relay()
        socket = SlowSocket()
        session = Session(None)
        session.media = socket
        session.queue.put_nowait((now_ms(), now_ms() + 1000, b"media"))
        await asyncio.wait_for(relay.media_writer(session, socket), 1)
        self.assertTrue(socket.closed)

    async def test_stale_queue_packet_never_written(self):
        class Socket:
            def __init__(self):
                self.sent = []

            async def send(self, raw):
                self.sent.append(raw)

        relay = Relay()
        socket = Socket()
        session = Session(None)
        session.media = socket
        session.queue.put_nowait((now_ms() - 150, now_ms() + 1000, b"old"))
        session.queue.put_nowait((now_ms(), now_ms() + 1000, b"new"))
        task = asyncio.create_task(relay.media_writer(session, socket))
        await asyncio.sleep(0.02)
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        self.assertEqual(socket.sent, [b"new"])
        self.assertEqual(relay.metrics["stale_media"], 1)


class ControlTests(unittest.TestCase):
    def test_idempotency_conflicts_and_stale_requests(self):
        relay = Relay()
        s = Session(None)
        r = dict(
            op="HELLO",
            request_id=1,
            version=1,
            device_id="0000000000000001",
            boot_id="0000000000000002",
            profiles=[1],
            max_mix_streams=1,
            effective_mix_streams=1,
            mix_mode="auto",
            requested_mix_streams=None,
        )
        first = relay.request(s, r)
        self.assertTrue(first["ok"])
        self.assertEqual(first, relay.request(s, r))
        self.assertEqual(relay.request(s, dict(r, version=2))["error"], "BAD_REQUEST")
        s.recent.clear()
        self.assertEqual(relay.request(s, r)["error"], "STALE_REQUEST")

    def test_invalid_capabilities_do_not_initialize_identity(self):
        relay = Relay()
        s = Session(None)
        reply = relay.request(
            s,
            dict(
                op="HELLO",
                request_id=1,
                version=1,
                device_id="0000000000000001",
                boot_id="0000000000000002",
                profiles=[1],
                max_mix_streams=0,
            ),
        )
        self.assertFalse(reply["ok"])
        self.assertEqual(s.device, 0)
