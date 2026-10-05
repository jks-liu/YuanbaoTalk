import unittest
from dataclasses import replace
from yuanbao.protocol import Packet, Window, channel_id, HEADER, AUDIO


def packet(**changes):
    return replace(
        Packet(AUDIO, 0, 2, channel_id("1"), 1, 2, 3, 1, 0, payload=b"\x08\x01"), **changes
    )


class WireTests(unittest.TestCase):
    def test_roundtrip_and_layout(self):
        p = packet()
        self.assertEqual(HEADER.size, 64)
        self.assertEqual(Packet.decode(p.encode()), p)
        self.assertEqual(p.encode()[:8].hex(), "5954010200020140")
        self.assertEqual(
            p.encode()[24:48].hex(), "000000000000000100000000000000020000000000000003"
        )

    def test_channels(self):
        self.assertEqual(channel_id("0001"), channel_id("1"))
        self.assertNotEqual(channel_id("5f180b62-879b-41fa-8ee5-ea641f317ba0"), channel_id("1"))
        for value in ("0", "-1", "4294967296", "00000000-0000-0000-0000-000000000001"):
            with self.assertRaises(ValueError):
                channel_id(value)

    def test_reject_malformed(self):
        for p in (
            packet(ttl=17),
            packet(flags=2),
            packet(sample=1),
            packet(age=300),
            packet(payload=b""),
            packet(payload=b"\x00"),
        ):
            with self.assertRaises(ValueError):
                Packet.decode(p.encode())
        for raw in (packet().encode()[:-1], packet().encode() + b"x", b"YT"):
            with self.assertRaises(ValueError):
                Packet.decode(raw)

    def test_window_reorder_conflict_and_boundary(self):
        window = Window()
        self.assertTrue(window.accept(packet(seq=300)))
        self.assertTrue(window.accept(packet(seq=45)))
        self.assertFalse(window.accept(packet(seq=44)))
        self.assertFalse(window.accept(packet(seq=300, flags=1, ttl=3, age=20)))
        with self.assertRaises(ValueError):
            window.accept(packet(seq=300, sample=320))
