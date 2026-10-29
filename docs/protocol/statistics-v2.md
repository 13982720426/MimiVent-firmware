# MimiVent Statistics and BLE Protocol V2

Status: implementation contract for firmware and Flutter. V2 intentionally does
not read or migrate the old statistics snapshot format. Installing a firmware
with the V2 partition table initializes a new statistics dataset and epoch; NVS
timing settings are preserved.

## Identity and ordering

- A device has a stable `deviceId`; the statistics dataset has a random 128-bit
  `dataEpoch`; each boot has a random 64-bit `bootId`.
- `eventId` is a monotonically increasing non-zero uint32 within one epoch.
  It is the ordering, de-duplication, and pagination key. Time is never a cursor.
- `operationId` and `runId` are the event IDs of their respective start records.
- `sensorId`, `fanId`, and `zoneId` use separate uint16 namespaces. Zero is
  reserved for system events. The first hardware topology is sensor 1, fan 1,
  zone 1. The current board still instantiates one PIR and one fan.
- Epoch and boot IDs are lowercase hex strings in JSON. IDs and durations are
  integers, not display strings.

## Record types

| Code | Name | Meaning |
| ---: | --- | --- |
| 1 | PIR_TRIGGER | PIR LOW to HIGH edge |
| 2 | OP_START | Accepted control operation |
| 3 | OP_END | Operation completed, replaced, or interrupted |
| 4 | RUN_START | Actual fan output became active or ownership changed |
| 5 | RUN_END | Actual fan output stopped or ownership changed |
| 6 | AUTO_SELECTED | User explicitly returned to automatic mode |
| 7 | CLOCK_ANCHOR | UTC/uptime mapping |
| 8 | BOOT | Boot reason and dataset/topology context |
| 9 | SHUTDOWN_INTENT | Controlled restart or OTA intent |

Mode codes: AUTOMATIC=1, MANUAL=2, PAUSE=3, FORCE=4, LOCKED_OFF=5.
Source codes: AUTOMATIC=1, BLE=2, HTTP=3, SYSTEM=4. Profile codes:
PRODUCTION=1, DEV=2. End reasons: COMPLETED=1, REPLACED=2, RESTARTED=3,
PIR_INTERRUPTED=4, CANCELLED=5, REBOOT_INTERRUPTED=6, UPGRADE_RESTART=7,
OWNER_CHANGED=8. Duration quality: UNKNOWN=0, EXACT=1,
CONFIRMED_MINIMUM=2, INFERRED=3. Published numeric codes are immutable.

## Persistent event segment

All integer fields are little-endian. The 64-byte segment header has this fixed
layout: magic[4], storageVersion:u16, headerLength:u16, dataEpoch[16],
bootId[8], segmentId:u32, firstEventId:u32, createdUptimeSeconds:u32,
anchorUtcSeconds:u32, anchorUptimeSeconds:u32, utcOffsetMinutes:i16,
flags:u16, topologyRevision:u32, headerCrc32:u32, reserved[4]. CRC-32/IEEE
covers the preceding 56 bytes.

Each record is `recordLength:u16, type:u8, flags:u8, eventId:u32,
uptimeSeconds:u32, subjectKind:u8, subjectId:u16, payload[0..105],
recordCrc32:u32`. The minimum record size is 19 bytes and the maximum is
128 bytes. The CRC covers every byte except the CRC field itself. Encoders must
write fields explicitly; compiler struct padding is never persisted.

Segments target 8192 bytes. They are append-only; an incomplete or corrupt tail
is ignored and that segment is sealed. Older whole segments can be removed for
retention/capacity, but an active recovery context is checkpointed first.

## API V2

The API contract is transport-neutral. BLE is an adapter that carries a command
envelope and frames its response; local AP HTTP and future station-mode Wi-Fi
HTTP call the same `ControlApi`/query services and return the same JSON models
and error codes. Do not duplicate control rules in `BluetoothManager` or the
web adapter. Advertised capabilities determine which transport and commands a
client can use. HTTP credentials and Wi-Fi secrets are never exposed through
the BLE capabilities response.

`GET /api/v2/capabilities` returns `apiVersion`, `eventSchema`, `deviceId`,
`dataEpoch`, firmware/minimum client versions, retention and available range,
persisted event watermarks, page limits, storage health, topology revision,
endpoint counts and supported capabilities.

`GET /api/v2/topology` returns a versioned endpoint registry. `/api/v2/status`
returns `fans[]` and `sensors[]`; UI may simplify the one-fan/one-sensor case.

`GET /api/v2/events` accepts `dataEpoch`, UTC `[start,end)`, optional numeric
types/profile/endpoint filters, `direction`, opaque `cursor`, `limit` (default
64, maximum 128), and `snapshotUpperId`. A cursor binds the full query and epoch.
The first response fixes the upper event ID; all subsequent pages use it.
Responses include compact rows, dictionaries, next cursor, range, truncation,
unknown-time, gaps, and storage-health flags. Unknown record types remain
skippable and are preserved as raw rows.

Rows are compact fixed-order arrays:

```json
{"dataEpoch":"00112233445566778899aabbccddeeff","upperId":120,
 "boots":["0102030405060708"],"scopes":[[1,1,3]],
 "rows":[[119,1790000000,0,42,1,0,0,[]]],
 "nextCursor":"opaque-token","hasMore":false}
```

Each row is `[eventId,utcSecondsOrNull,bootRef,uptimeSeconds,type,flags,
scopeRef,payload]`. `scopeRef` indexes `[subjectKind,subjectId,topologyRevision]`.
JSON `null` means calendar time is unknown; it must not be coerced to epoch 0.

## BLE response framing

The request starts with a 16-bit little-endian `requestId` followed by the
existing UTF-8 request envelope. Every notification begins with
`requestId:u16, fragmentIndex:u16`; fragment zero additionally carries
`frameVersion:u8, flags:u8, bodyLength:u16, bodyCrc32:u32` (12-byte total
header), then the UTF-8 status line and JSON body.
Continuation fragments carry bytes after the 4-byte prefix. Fragment payload
size is computed from the actual negotiated MTU minus ATT and frame overhead.

The receiver associates all fragments with one active request, accepts only
byte-identical duplicate fragments, validates total length and CRC before UTF-8
decode, and rejects missing, conflicting, stale-session, over-size, or excess
fragment streams. Reads may be retried once with the same snapshot. A timed-out
control write has an unknown result and must never be automatically replayed.

## Future Wi-Fi and OTA boundary

Station-mode Wi-Fi control uses the same versioned business API over HTTP(S),
with per-device authentication added before remote exposure. It does not reuse
BLE fragmentation or MTU logic. OTA uses a separate bounded streaming endpoint
and image validation/rollback flow; capabilities expose current and minimum
supported firmware versions, while OTA metadata identifies the target board,
version, image length, and digest. A BLE/App command may initiate an OTA session,
but image bytes travel over a separately negotiated transport and are never
embedded in a JSON response or statistics log. Partition-table changes require
a USB maintenance install as documented below.

## Storage defaults

Detail retention is 10 local calendar days; hourly and daily summaries retain
30 local calendar days. Default statistics partition budget is 512 KiB with a
320 KiB logical target. The 10-day target requires at least 30% free capacity
after replaying a representative peak-use sample. No reliable clock means no
date-based deletion; capacity pressure still reports the actual truncation.

The custom 4 MiB partition table is `partitions.csv`: NVS 0x5000, otadata
0x2000, two OTA app slots of 0x1B0000 each, `statistics` SPIFFS 0x80000,
and coredump 0x10000. Both OTA slots remain enabled. A USB install is required
to adopt the new partition table; a normal app OTA cannot repartition flash.
