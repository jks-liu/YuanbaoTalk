"""Version 1 wire format; shared golden vectors test the firmware serializer."""

from dataclasses import dataclass, replace
import hashlib
import struct
import uuid

HEADER = struct.Struct("!2s6B16sQQQIIHHI")
ENVELOPE = struct.Struct("!QQQ")
START, AUDIO, END = 1, 2, 3


def opus_20ms(payload):
    if not payload:
        return False
    config, code = payload[0] >> 3, payload[0] & 3
    samples = (
        (40 << (config & 3))
        if config >= 16
        else (
            (160 << (config & 1))
            if config >= 12
            else (960 if config & 3 == 3 else 160 << (config & 3))
        )
    )
    count = 1 if code == 0 else ((payload[1] & 63) if len(payload) > 1 else 0) if code == 3 else 2
    return samples * count == 320


def channel_id(value: str) -> bytes:
    if value.isascii() and value.isdecimal():
        number = int(value)
        if not 1 <= number <= 0xFFFFFFFF:
            raise ValueError("channel range")
        return number.to_bytes(16, "big")
    result = uuid.UUID(value).bytes
    if result[:12] == bytes(12):
        raise ValueError("reserved UUID namespace")
    return result


@dataclass(frozen=True)
class Packet:
    kind: int
    flags: int
    ttl: int
    channel: bytes
    origin: int
    boot: int
    stream: int
    seq: int
    sample: int
    age: int = 0
    payload: bytes = b""

    @property
    def key(self):
        return self.channel, self.origin, self.boot, self.stream

    def encode(self):
        return (
            HEADER.pack(
                b"YT",
                1,
                self.kind,
                self.flags,
                self.ttl,
                1,
                64,
                self.channel,
                self.origin,
                self.boot,
                self.stream,
                self.seq,
                self.sample,
                len(self.payload),
                0,
                self.age,
            )
            + self.payload
        )

    @classmethod
    def decode(cls, raw: bytes):
        if not 64 <= len(raw) <= 250:
            raise ValueError("packet length")
        (
            magic,
            ver,
            kind,
            flags,
            ttl,
            profile,
            size,
            ch,
            origin,
            boot,
            stream,
            seq,
            sample,
            n,
            reserved,
            age,
        ) = HEADER.unpack_from(raw)
        if (
            magic != b"YT"
            or ver != 1
            or kind not in (1, 2, 3)
            or flags & ~1
            or ttl > 16
            or profile != 1
            or size != 64
            or reserved
            or n != len(raw) - 64
            or not any(ch)
            or not origin
            or not boot
            or not stream
            or sample % 320
            or sample > 960000
            or age >= 300
        ):
            raise ValueError("invalid header")
        if (
            (kind == AUDIO and (not opus_20ms(raw[64:]) or sample >= 960000))
            or (kind != AUDIO and n)
            or (kind == START and sample)
        ):
            raise ValueError("invalid payload")
        return cls(kind, flags, ttl, ch, origin, boot, stream, seq, sample, age, raw[64:])

    def fingerprint(self):
        return hashlib.blake2s(
            replace(self, flags=0, ttl=0, age=0).encode(), digest_size=16
        ).digest()


class Window:
    """Bounded reordered messages. Conflicting duplicates never replace originals."""

    def __init__(self):
        self.high = -1
        self.items = {}

    def accept(self, packet):
        if packet.seq <= self.high - 256:
            return False
        digest = packet.fingerprint()
        if packet.seq in self.items:
            if self.items[packet.seq] != digest:
                raise ValueError("conflicting duplicate")
            return False
        self.high = max(self.high, packet.seq)
        self.items = {k: v for k, v in self.items.items() if k > self.high - 256}
        self.items[packet.seq] = digest
        return True
