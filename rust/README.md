# csyn

`csyn` is a ROS-like command-line tool for Synapse systems using Zenoh for
transport and FlatBuffers for payloads.

The wire contract comes entirely from the pinned `synapse_fbs` crate: the
topic catalog, the embedded schema sources and binary schemas (`.bfbs`), and
the generated payload decoder. There are no vendored schema copies in this
tree, so the CLI cannot drift from the release it was built against
(`csyn type list` prints that release).

The command grammar intentionally mirrors common ROS 2 workflows. Topic
arguments accept catalog keys and names (`health`, `VehicleHealth`) as well as
raw Zenoh key expressions; bare names expand to the canonical catalog keys,
including namespaced and instance-suffixed matches:

```sh
csyn topic list
csyn topic list att --type AttitudeEstimate
csyn topic echo att
csyn topic pub att --type AttitudeEstimate --file sample.bin --rate 50
csyn topic hz att
csyn topic bw '**/imu/**'

csyn type list
csyn type show health --fbs

csyn bag record '**' -o flight.mcap --source ground-station
csyn bag info flight.mcap
csyn bag play flight.mcap
csyn bag export flight.mcap -o flight.jsonl

csyn graph serve
```

The default Zenoh router endpoint is `tcp/127.0.0.1:7447`. Override it with:

```sh
csyn --connect tcp/192.168.1.10:7447 topic list
CSYN_CONNECT=tcp/192.168.1.10:7447 csyn topic list
```

If no router is running locally, start one in another terminal:

```sh
zenohd -l tcp/127.0.0.1:7447
```

## Topic Commands

`topic list` subscribes to `**` for a short observation window and prints
topics carrying a valid Synapse value contract. An optional positional filter
narrows the subscription to one catalog topic or key expression, and `--type`
keeps only topics carrying that catalog type. Type inference from keys is
not used. Metadata-free or schema-mismatched samples are rejected, with
warnings throttled to once per topic every ten seconds. Zenoh
does not provide ROS-style graph discovery by default, so this is
traffic-observed discovery for now.

```sh
csyn topic list
csyn topic list 'cub1/**'
csyn topic list --type InertialSample
```

`topic echo` requires and validates the value's fully qualified wire type and
per-message schema fingerprint before decoding. `--type` is an optional
additional assertion; it never bypasses value-contract validation:

```sh
csyn topic echo att
csyn topic echo 'cub1/**' --type AttitudeEstimate --output json
csyn topic echo test/topic --raw
```

`topic pub` publishes raw payloads from text or files and requires `--type`.
It validates the payload and attaches the canonical value contract; bare
catalog names resolve to the canonical publication key. Typed
JSON-to-FlatBuffer publish is intentionally left for a BFBS reflection builder
layer.

## Graph Debugger

`graph serve` starts a local web UI for debugging traffic and Zenoh topology:

```sh
csyn graph serve
csyn --connect tcp/127.0.0.1:7448 graph serve --bind 127.0.0.1:8088
```

Open `http://127.0.0.1:8088` in a browser. The graph shows observed topics,
message counts, rates, payload sizes, Zenoh router/session topology, and any
publisher/subscriber declarations exposed by Zenoh admin-space.

## Bag Format

Bags use the frozen `synapse/1` MCAP profile built into `synapse_fbs` 0.8.
Recordings contain the required schema-set hash, random session id, source,
and Unix-epoch time-basis metadata; `--source` identifies the recorder and
defaults to `csyn`.

Each observed Zenoh key becomes a channel whose `synapse.topic_id` metadata,
root-table schema name, `flatbuffer` encoding, and embedded `.bfbs` come from
the pinned catalog. Fixed-layout Zenoh structs are wrapped in their existing
root table for MCAP and unwrapped again during replay/export. Variable-size
root-table payloads are stored unchanged. The upstream profile writer is
uncompressed, unchunked, and index-less, so `bag info` scans messages rather
than requiring an MCAP summary section. Samples without a valid Synapse value
contract are not recorded.

The legacy `.csynbag` v1 format is retired; `bag` subcommands only speak
MCAP.
