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

## Requests and responses

| type | name     | body |
|------|----------|------|
| 0x01 | PRODUCE  | u16 topic_len, topic, u32 partition, u32 n_records, records |
| 0x02 | FETCH    | u16 topic_len, topic, u32 partition, u64 offset, u32 max_bytes |
| 0x03 | COMMIT   | u16 group_len, group, u16 topic_len, topic, u32 partition, u64 offset |
| 0x04 | OFFSETS  | u16 group_len, group, u16 topic_len, topic, u32 partition |

| type | name        | body |
|------|-------------|------|
| 0x81 | PRODUCE_ACK | u64 base offset (offset of the batch's first record) |
| 0x82 | RECORDS     | u64 first offset, u32 n, n x (u64 offset, u32 key_len, u32 value_len, key, value) |
| 0x83 | COMMIT_ACK  | (empty) |
| 0x84 | OFFSET      | u8 found, u64 committed offset (0 if not found) |
| 0xff | ERROR       | u16 code, message |

Topic names are 1-63 bytes of `[A-Za-z0-9._-]` (not starting with
`.`): they name a directory.  Group and topic names in COMMIT/OFFSETS
are at most 31 bytes.  METADATA (0x05) is reserved, not implemented.

### Semantics

- PRODUCE_ACK means every record of the batch is stored and, in the
  default mode, fdatasync'ed.  With `--fsync-interval-ms=N` the ack
  follows the write and a sync happens within N ms.
- FETCH returns records from `offset` (or from the oldest retained
  offset if `offset` was deleted by retention) up to `max_bytes` of
  record data; `max_bytes` 0 means 64 KiB and is clamped to fit the
  64 MiB frame.  It returns only acked records.  A record larger than
  `max_bytes` is not returned: n is 0.
- COMMIT_ACK means the offset is fdatasync'ed in the offset store.

### Error codes

| code | name        | meaning | connection |
|------|-------------|---------|------------|
| 1 | BAD_REQUEST | malformed / unknown request, bad topic name | closed |
| 2 | MEMORY      | PRODUCE over `--max-memory`: nothing from the batch was stored | kept |
| 3 | STORAGE     | write or fsync failed; the partition accepts no more writes. The batch may be partly on disk; it is not acked | kept |
| 4 | TIMEOUT     | the partition or offset store did not answer in time; the request may still complete | kept |
| 5 | SHUTDOWN    | broker shutting down | kept |
| 6 | GROUP       | offset store full or failed | kept |

## Record

A produced record on the wire mirrors its on-disk framing minus the
CRC (added at append time):

    key_len u32 BE, value_len u32 BE, key bytes, value bytes
