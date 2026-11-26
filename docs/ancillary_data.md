# Ancillary Data

ST 2110-40 embeds ancillary payload according to [SMPTE 291M](https://tools.ietf.org/html/rfc8331)
where each ancillary sub-stream is identified by a DID/SDID pair. LIST
can theorytically detect the presence of any ancillary type and also
detects integrity by counting the payload errors.

With repect to the decoding, third-party [libklvanc](https://github.com/stoth68000/libklvanc)
supports the following types:

 * CEA-708/EIA-608 closed captions
 * SMPTE 12-2 timecode
 * SCTE-104 Ad triggers
 * Active Format Descriptor
 * SMPTE 2038 arbitrary VANC encapsulation

So far, only close caption and timecode are integrated in LIST.
It is mostly tested on North American streams.

## RTP

 * anc must have at least one (keap alive) or more packet per frame
 * in a frame, packets have the same RTP timestamp
 * this number of packets number may vary
 * the frame rate and the media clock are the same as video
 * the last packet of each frame must have the marker bit set
 * just as for video, RTP timestamp delta is logged for every new frame

The delay between packet time and RTP timestamp should be moderate.
Though no range has been defined anywhere, EBU-LIST applies the same
constraints as for video, i.e. [JT-NM tested program](https://static.jt-nm.org/documents/JT-NM_Tested_Catalog_ST2110_Full-Online-2020-05-12.pdf)
proposes: "The instantaneous value of theRTP timestamp of the stream is
not in the future, not more than 1 ms in the past (unless justified),
and preserves a stable relation to the PTP (should not “drift”)"

## Ancillary header

 * field bits must have acceptable value depending on scan type and
   frame/field transition
 * DID/SDID must be registered values
 * payload intergrity is validated against parity bits and checksum

## Timecode

Supported:

 * ATC LTC
 * ATC VITC1
 * ATC VITC2

## Close caption

Supported:

 * NTSC line 21
 * DTVCC Channel
 * AFD
 * SCTE 104 (only opID is decoded)

### CEA-708 (DTVCC) caption text, including Korean

The DTVCC entries (`cc_type` 2/3) of the CDP (DID 0x61 / SDID 0x01) are
reassembled into caption channel packets and decoded per caption service.
Each service gets a `DTVCC Service N` payload containing the caption text
(UTF-8), with window boundaries and carriage returns rendered as line breaks.
A `DTVCC Statistics` payload reports the number of packets, sequence number
discontinuities, size errors and decoded Korean characters.

Korean captions (TTA TTAK.KO-07.0093) are supported: Hangul and Hanja are
transmitted as KS X 1001 (EUC-KR) two-byte codes in the G1 code space
(both bytes in 0xA1..0xFE). The decoder:

 * decodes a G1 byte pair as KS X 1001 when it is an assigned code,
   otherwise as ISO 8859-1 (Latin-1), so that North American / European
   captions keep working;
 * reassembles a character split across two service blocks;
 * also accepts 16-bit characters sent with the P16 command (`EXT1 0x18`),
   either KS X 1001 or UCS-2;
 * decodes G0 (ASCII, 0x7F = ♪), G2 and the G3 `[CC]` icon.

Pen attributes, colors, window positions and caption timing are not rendered.

The KS X 1001 table (`cpp/libs/analysis/lib/src/ebu/list/analysis/handlers/cea708/ksx1001_table.cpp`)
is generated with `scripts/generate_ksx1001_table.py`.

A test capture with Korean captions, `sample_data/pcap/st2110/2110-40/anc_cea708_korean.pcap`,
is generated from `anc_with_timecode+CC+AFD.pcap` by `scripts/make_korean_cc_test_pcap.py`.
Uploaded to LIST, its CEA-708 sub-stream shows in `DTVCC Service 1`:

```
안녕하세요. 한글 자막 테스트입니다.
ST 2110-40 CEA-708 디코더
자막 방송 ♪
```
