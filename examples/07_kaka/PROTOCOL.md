# kaka protocol

A small length-framed binary protocol.  Defined incrementally as the
phases land; this file is the stub for Phase 0.

## Frame

Every request and response is:

    +--------+--------+------------------+
    | len    | type   | body             |
    | u32 BE | u8     | len-1 bytes      |
    +--------+--------+------------------+

`len` counts `type` plus `body` (i.e. the bytes after the length
field).  All multi-byte integers are big-endian, matching the
network-order convention used by the Bitcask record framing the
partition logs reuse.

## Request types (planned)

| type | name     | phase | body |
|------|----------|-------|------|
| 0x01 | PRODUCE  | 1     | topic, partition, record(s) |
| 0x02 | FETCH    | 1     | topic, partition, offset, max_bytes |
| 0x03 | COMMIT   | 4     | group, topic, partition, offset |
| 0x04 | OFFSETS  | 4     | group, topic, partition |
| 0x05 | METADATA | 1     | (empty) -> topic/partition list |

## Response types (planned)

| type | name        | phase | body |
|------|-------------|-------|------|
| 0x81 | PRODUCE_ACK | 1     | base offset assigned |
| 0x82 | RECORDS     | 1     | record(s) from the requested offset |
| 0x83 | COMMIT_ACK  | 4     | (empty) |
| 0x84 | OFFSET      | 4     | committed offset |
| 0x85 | METADATA    | 1     | topic/partition list |
| 0xff | ERROR       | all   | u16 code, utf-8 message |

## Record

A produced record on the wire mirrors its on-disk Bitcask framing
minus the CRC (added at append time):

    key_len u32 BE, value_len u32 BE, key bytes, value bytes

The Phase 1 codec implements the frame envelope, PRODUCE, FETCH, and
METADATA with round-trip unit tests before any partition logic is
wired in.
