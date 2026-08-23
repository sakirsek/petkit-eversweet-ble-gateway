# Baseline capture

`baseline-20260819.jsonl.gz` is six hours of continuous observation of one
fountain on the night of 19 August 2026: 1136 state pushes across 636 BLE
connections, plus 15 cat visits.

It is kept in the repository because that night cannot be re-measured, and
because it is the empirical basis for two numbers the gateway depends on: the
proximity threshold that decides whether a cat is present, and the thirst alarm
window. Anything that claims to improve either should be checked against this
file first.

The field names were originally recorded in Turkish and translated to English.
The raw `hex` frames, which are the actual evidence, were carried across
untouched and verified byte for byte.

## Format

One JSON object per line, gzipped. Every record has `ts` (local time,
`YYYY-MM-DD HH:MM:SS`) and `type`.

| `type` | Count | Meaning |
|---|---:|---|
| `state` | 1136 | A `CMD 230` state push |
| `unknown_byte` | 1179 | A byte in the state payload changed value |
| `ble_connected` | 636 | Connection established |
| `ble_disconnected` | 634 | The fountain closed the connection |
| `visit_start` | 15 | A detection edge, cat arriving |
| `visit_end` | 15 | A detection edge, cat leaving |

### `state`

```json
{"type": "state", "hex": "0101010200...", "decoded": {"power": 1, ...}, "ts": "..."}
```

`hex` is the complete 42-byte payload exactly as received. `decoded` holds the
fields the decoder understood **at the time of capture**, which is fewer than it
understands now: the proximity sensor at bytes 26 to 29 had not been identified
yet. Decode `hex` yourself rather than trusting `decoded` to be complete. The
current byte map is in [../docs/protocol.md](../docs/protocol.md).

### `unknown_byte`

```json
{"type": "unknown_byte", "idx": 28, "old": 163, "new": 177, "ts": "..."}
```

Emitted whenever a byte with no known meaning changed. This is how the
proximity sensor was found: index 28 moved constantly, index 29 moved with it,
and both jumped when the cat appeared.

### `visit_end`

```json
{"type": "visit_end", "duration_s": 42, "suspect": false, "ts": "..."}
```

`suspect` marks a detection the heuristic was unsure about at capture time.

## Using it

```python
import gzip, json
for line in gzip.open("data/baseline-20260819.jsonl.gz", "rt"):
    rec = json.loads(line)
    if rec["type"] == "state":
        payload = bytes.fromhex(rec["hex"])
```
