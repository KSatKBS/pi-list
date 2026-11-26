#!/usr/bin/env python3
"""Create an ST 2110-40 test capture carrying Korean CEA-708 (DTVCC) captions.

The CEA-708 CDPs (DID 0x61 / SDID 0x01) of an existing ST 2110-40 capture are
rewritten: their unused DTVCC cc_data slots are filled with caption channel
packets for service 1, whose text is encoded in KS X 1001 (EUC-KR) as done by
Korean broadcasters (TTA TTAK.KO-07.0093). The caption service descriptor
language is set to "kor". Everything else (timing, RTP, other ANC packets,
CEA-608 entries) is kept. CDP checksums, ANC word parity and ANC checksums are
recomputed.

Usage:
    python3 scripts/make_korean_cc_test_pcap.py \
        sample_data/pcap/st2110/2110-40/anc_with_timecode+CC+AFD.pcap \
        sample_data/pcap/st2110/2110-40/anc_cea708_korean.pcap
"""

import struct
import sys

# Caption lines (service 1). Keep in sync with the analysis unit test (test_anc.cpp).
CAPTION_LINES = [
    "안녕하세요. 한글 자막 테스트입니다.",
    "ST 2110-40 CEA-708 디코더",
    "자막 방송 ",
]
MUSIC_NOTE = 0x7F  # CEA-708 G0 0x7F = U+266A

DID_CDP, SDID_CDP = 0x61, 0x01

# CEA-708 commands
DF0 = [0x98, 0x38, 0x00, 0x00, 0x0E, 0x1F, 0x00]  # define window 0 (visible), 15 rows x 32 columns
CR = 0x0D
CLW_ALL = [0x88, 0xFF]
DSW_0 = [0x89, 0x01]


def caption_service_bytes():
    data = list(DF0)
    for i, line in enumerate(CAPTION_LINES):
        if i == len(CAPTION_LINES) - 1:
            data += CLW_ALL
        data += list(line.encode("euc_kr"))
        if i < len(CAPTION_LINES) - 2:
            data.append(CR)
    data.append(MUSIC_NOTE)
    data += DSW_0
    return data


def split_service_blocks(data, max_block=31):
    """Split service data into blocks, never cutting a 2-byte KS X 1001 character or a command."""
    blocks, i = [], 0
    while i < len(data):
        end = min(i + max_block, len(data))
        j = i
        while j < end:
            b = data[j]
            if 0xA1 <= b <= 0xFE:
                step = 2
            elif b == 0x98:
                step = 7
            elif b in (0x88, 0x89):
                step = 2
            else:
                step = 1
            if j + step > end:
                break
            j += step
        blocks.append(data[i:j])
        i = j
    return blocks


def dtvcc_packets(service_number=1):
    packets = []
    for seq, block in enumerate(split_service_blocks(caption_service_bytes())):
        payload = [(service_number << 5) | len(block)] + block
        if (len(payload) + 1) % 2:
            payload.append(0x00)  # null block padding
        size_code = (len(payload) + 1) // 2
        packets.append([((seq & 3) << 6) | size_code] + payload)
    return packets


# ---------------------------------------------------------------------------- bit helpers


class BitReader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def get(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v


def put_bits(buf, pos, n, value):
    for k in range(n):
        bit = (value >> (n - 1 - k)) & 1
        idx, sh = (pos + k) >> 3, 7 - ((pos + k) & 7)
        buf[idx] = (buf[idx] & ~(1 << sh)) | (bit << sh)


def anc_word(b):
    parity = bin(b).count("1") & 1
    return b | (parity << 8) | ((parity ^ 1) << 9)


def anc_checksum(words):
    s = sum(w & 0x1FF for w in words) & 0x1FF
    return s | ((((s >> 8) & 1) ^ 1) << 9)


# ---------------------------------------------------------------------------- CDP rewrite


def rewrite_cdp(cdp, packet_queue):
    """cdp: list of bytes (UDW). Fills DTVCC padding slots with queued caption packets."""
    assert cdp[0] == 0x96 and cdp[1] == 0x69, "not a CDP"
    flags = cdp[4]
    pos = 7
    if flags & 0x80:  # time code present
        pos += 5
    if flags & 0x40:  # cc_data present
        assert cdp[pos] == 0x72
        cc_count = cdp[pos + 1] & 0x1F
        slots = [pos + 2 + 3 * i for i in range(cc_count)]
        dtvcc_slots = [s for s in slots if (cdp[s] & 0x03) >= 2]

        pairs = []
        if packet_queue:
            pkt = packet_queue[0]
            pairs = [pkt[i : i + 2] for i in range(0, len(pkt), 2)]
            if len(pairs) <= len(dtvcc_slots):
                packet_queue.pop(0)
            else:
                pairs = []

        # the original DTVCC entries are replaced by the Korean caption packet or by padding
        for k, slot in enumerate(dtvcc_slots):
            if k < len(pairs):
                cdp[slot] = 0xF8 | 0x04 | (3 if k == 0 else 2)  # marker bits, cc_valid, cc_type
                cdp[slot + 1], cdp[slot + 2] = pairs[k]
            else:
                cdp[slot], cdp[slot + 1], cdp[slot + 2] = 0xFA, 0x00, 0x00  # DTVCC padding
        pos += 2 + 3 * cc_count

    if flags & 0x20 and cdp[pos] == 0x73:  # caption service info: set language to "kor"
        svc_count = cdp[pos + 1] & 0x0F
        for i in range(svc_count):
            entry = pos + 2 + 7 * i
            cdp[entry + 1 : entry + 4] = list(b"kor")

    # CDP checksum: all bytes sum to 0 mod 256
    cdp[-1] = (-sum(cdp[:-1])) & 0xFF
    return cdp


def process(src, dst):
    data = bytearray(open(src, "rb").read())
    magic = struct.unpack("<I", data[:4])[0]
    assert magic == 0xA1B2C3D4, "only little-endian microsecond pcap is supported"
    queue = dtvcc_packets()
    total = len(queue)

    off = 24
    while off < len(data):
        _, _, incl, _ = struct.unpack("<IIII", data[off : off + 16])
        pkt_start = off + 16
        off = pkt_start + incl

        eth = 18 if data[pkt_start + 12 : pkt_start + 14] == b"\x81\x00" else 14
        ip = pkt_start + eth
        if data[ip + 9] != 17:
            continue
        udp = ip + (data[ip] & 0x0F) * 4
        rtp = udp + 8
        cc = data[rtp] & 0x0F
        payload = rtp + 12 + 4 * cc
        anc_count = data[payload + 4]

        bit_base = (payload + 8) * 8
        reader = BitReader(data, bit_base)
        for _ in range(anc_count):
            reader.get(32)  # C, line, offset, S, stream num
            did_pos = reader.pos
            did, sdid, dc = reader.get(10), reader.get(10), reader.get(10)
            udw_pos = reader.pos
            count = dc & 0xFF
            words = [reader.get(10) & 0xFF for _ in range(count)]
            reader.get(10)  # checksum
            while (reader.pos - bit_base) % 32:
                reader.get(1)

            if (did & 0xFF, sdid & 0xFF) != (DID_CDP, SDID_CDP):
                continue

            new_words = [anc_word(b) for b in rewrite_cdp(words, queue)]
            for k, w in enumerate(new_words):
                put_bits(data, udw_pos + 10 * k, 10, w)
            checksum = anc_checksum([did, sdid, dc] + new_words)
            put_bits(data, udw_pos + 10 * count, 10, checksum)
            assert did_pos < udw_pos

        struct.pack_into("!H", data, udp + 6, 0)  # UDP checksum: not computed

    if queue:
        sys.exit(f"error: only {total - len(queue)} of {total} caption packets fit in the capture")
    open(dst, "wb").write(data)
    print(f"wrote {dst}: {total} DTVCC packets, service 1, language kor")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    process(sys.argv[1], sys.argv[2])
