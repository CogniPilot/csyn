# csyn

`csyn` is a ROS-like command-line tool for Synapse systems using Zenoh for
transport and FlatBuffers for payloads.

The wire contract comes entirely from the pinned `synapse_fbs` crate: the
topic catalog, the embedded schema sources and binary schemas (`.bfbs`), and
the generated payload decoder. There are no vendored schema copies in this
tree, so the CLI cannot drift from the release it was built against
(`csyn type list` prints that release).

The command grammar intentionally mirrors common ROS 2 workflows. Topic
arguments accept catalog names (`vehicle_health`, `VehicleHealth`) as well as
raw Zenoh key expressions; bare names expand to the canonical catalog keys,
including namespaced and instance-suffixed matches:

```sh
csyn topic list
csyn topic echo attitude_estimate
csyn topic pub attitude_estimate --file sample.bin --rate 50
csyn topic hz attitude_estimate
csyn topic bw '**/inertial_sample/**'

csyn type list
csyn type show vehicle_health --fbs

csyn bag record '**' -o flight.mcap
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
topics that were seen, tagged with their catalog type when the key parses as
a catalog topic. Zenoh does not provide ROS-style graph discovery by default,
so this is traffic-observed discovery for now.

`topic echo` infers the topic from the sample key (or `--type`) and decodes
the payload with the generated decoder — bare fixed-layout structs and root
tables both render through the bindings' pretty Debug format:

```sh
csyn topic echo attitude_estimate
csyn topic echo 'cub1/**' --type AttitudeEstimate --output json
csyn topic echo test/topic --raw
```

`topic pub` publishes raw payloads from text or files; bare catalog names
resolve to the canonical publication key. Typed JSON-to-FlatBuffer publish is
intentionally left for a BFBS reflection builder layer.

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

Bags are standard [MCAP](https://mcap.dev) files. Each observed Zenoh key
becomes a channel; each catalog topic contributes a schema record with the
fully qualified wire type as its name, `flatbuffer` encoding, and the
embedded `.bfbs` binary schema from the pinned `synapse_fbs` release as its
data — so bags are self-describing for any MCAP tool.

Channel message encodings mirror the catalog: `flatbuffer` for root-table
topics and `synapse_struct` for the canonical bare fixed-layout struct
payloads (which carry no FlatBuffers root offset). Unknown keys are recorded
with no schema and an empty encoding.

The legacy `.csynbag` v1 format is retired; `bag` subcommands only speak
MCAP.
