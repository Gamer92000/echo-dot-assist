"""MPEG-TS around the Echo's H.264 (docs/camera.md): Home Assistant's stream (PyAV) and go2rtc both read it from an
HTTP URL with the Echo's own timing.  Raw Annex B carries none: ffmpeg then assumes 25 fps, and the camera's 10-15
came out of recordings at nearly twice the speed.  One program, one video PID; PAT and PMT before every key frame (a
viewer may start at any of them), the codec config (SPS, PPS) in front of it as well: MediaTek's encoder sends them
once, in their own buffer, first."""
from __future__ import annotations

PAT_PID, PMT_PID, VIDEO_PID = 0x0000, 0x1000, 0x0100
AUD = b"\x00\x00\x00\x01\x09\xf0"           # access unit delimiter: each PES one whole access unit


def _crc32(data: bytes) -> int:
    """MPEG-2's CRC-32 (poly 0x04C11DB7, not reflected, no final XOR)"""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def _section(table_id: int, ext: int, body: bytes) -> bytes:
    """a PSI section: header, body, CRC"""
    length = 5 + len(body) + 4
    s = bytes([table_id, 0xB0 | (length >> 8), length & 0xFF, ext >> 8, ext & 0xFF, 0xC1, 0, 0]) + body
    return s + _crc32(s).to_bytes(4, "big")


PAT = _section(0x00, 1, bytes([0, 1, 0xE0 | (PMT_PID >> 8), PMT_PID & 0xFF]))
PMT = _section(0x02, 1, bytes([0xE0 | (VIDEO_PID >> 8), VIDEO_PID & 0xFF, 0xF0, 0,        # PCR PID, no program info
                               0x1B, 0xE0 | (VIDEO_PID >> 8), VIDEO_PID & 0xFF, 0xF0, 0]))  # H.264


class TsMuxer:
    """one per viewer: its own continuity counters, so that every viewer's stream is whole from its first packet"""

    def __init__(self) -> None:
        self.cc = {PAT_PID: 0, PMT_PID: 0, VIDEO_PID: 0}

    def _packets(self, pid: int, payload: bytes, pcr: int | None = None) -> bytes:
        out, first = bytearray(), True
        while payload or first:
            af = b""
            if first and pcr is not None:                  # adaptation field with the PCR: its 90 kHz base, extension 0
                base = pcr & ((1 << 33) - 1)
                af = bytes([0x10, base >> 25 & 0xFF, base >> 17 & 0xFF, base >> 9 & 0xFF, base >> 1 & 0xFF,
                            (base & 1) << 7 | 0x7E, 0])
            room = 184 - (1 + len(af) if af else 0)
            if len(payload) < room:                        # the last piece: stuffing in the adaptation field
                pad = room - len(payload)
                if af:
                    af += b"\xff" * pad
                else:
                    af = b"" if pad == 1 else bytes([0x00]) + b"\xff" * (pad - 2)
                    if pad == 1:
                        af = None                          # an adaptation field of length 0 (one byte)
            head = bytes([0x47, (0x40 if first else 0) | pid >> 8, pid & 0xFF])
            cc = self.cc[pid]; self.cc[pid] = (cc + 1) & 0xF
            if af is None:
                pkt = head + bytes([0x30 | cc, 0]) + payload[:183]
                payload = payload[183:]
            elif af:
                n = 184 - 1 - len(af)
                pkt = head + bytes([0x30 | cc, len(af)]) + af + payload[:n]
                payload = payload[n:]
            else:
                pkt = head + bytes([0x10 | cc]) + payload[:184]
                payload = payload[184:]
            assert len(pkt) == 188, len(pkt)
            out += pkt
            first = False
        return bytes(out)

    def tables(self) -> bytes:
        return self._packets(PAT_PID, b"\x00" + PAT) + self._packets(PMT_PID, b"\x00" + PMT)

    def frame(self, pts90: int, au: bytes, key: bool, config: bytes = b"") -> bytes:
        """one access unit at that PTS (90 kHz); a key frame with the tables and the config before it"""
        pts = pts90 & ((1 << 33) - 1)
        pes_head = bytes([0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5,               # length 0: unbounded (video), PTS only
                          0x21 | (pts >> 29 & 0x0E), pts >> 22 & 0xFF, 0x01 | (pts >> 14 & 0xFE), pts >> 7 & 0xFF,
                          0x01 | (pts << 1 & 0xFE)])
        body = AUD + (config if key else b"") + au
        pcr = max(0, pts90 - 9000)                         # 100 ms ahead of the picture: a decoder's buffer
        return (self.tables() if key else b"") + self._packets(VIDEO_PID, pes_head + body, pcr)
