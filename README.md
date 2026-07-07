# csyn

csyn is the CogniPilot synapse topic toolkit. One repo, two sides of the wire,
one folder per artifact:

- **`zephyr/`**: the west module — a lock-free latest-sample topic store for
  embedded vehicles, with zenoh and native_sim UDP transports, a zros bridge,
  and `csyn topic list/info/echo/hz/watch` shell diagnostics. Topic payloads
  are rendered by the synapse_fbs-generated field-descriptor printer, so
  every fixed-layout topic prints without hand-written formatting code.
- **`rust/`**: the host-side `csyn` CLI, a ROS-like command-line tool for
  Synapse systems using Zenoh for transport (see `rust/README.md`).

Both sides speak the synapse_fbs schema through a pinned release of the same
version: the Zephyr module pins the C release tarball in
`zephyr/CMakeLists.txt`, while the CLI pins the `synapse_fbs` crate, which
embeds the schema sources, compiled binary schemas, topic catalog, and
generated decoder. Every topic (name, keyexpr, payload type, encoding, catalog
id) resolves from the generated catalog, so the wire contract is locked by csyn
rather than per application and nothing is vendored.

## Zephyr module

Add csyn to your west manifest:

```yaml
- name: csyn
  remote: cognipilot
  revision: main
  path: modules/lib/csyn
```

Enable it in `prj.conf`:

```
CONFIG_CSYN=y
CONFIG_CSYN_SHELL=y
CONFIG_CSYN_ZROS_BRIDGE=y
```

and pick a transport per board: `CONFIG_CSYN_ZENOH=y` (flight hardware) or
`CONFIG_CSYN_NATIVE_UDP=y` (native_sim). Applications publish and subscribe
through the zros topics declared in `<csyn/csyn_zros.h>`; the bridge mirrors
them to the active transport.

Layout (everything for the module lives under `zephyr/`):

- `zephyr/include/csyn/csyn.h` — topic registry and store API
- `zephyr/include/csyn/csyn_codec.h` — payload decode/encode plus PWM/axis
  and quaternion/euler helpers
- `zephyr/include/csyn/csyn_types.h` — plain in-process types (rc channels,
  mocap rigid body, manual control)
- `zephyr/include/csyn/csyn_zros.h` — zros topic declarations vehicles use
- `zephyr/src/` — store, codec, bridge, shell, and transports
- `zephyr/{module.yml,Kconfig,CMakeLists.txt}` — west integration and the
  pinned synapse_fbs release

## Rust CLI

The host tool lives in `rust/`:

```sh
cd rust
cargo run -- topic list
```

Bags are standard MCAP files whose schema records carry the embedded
synapse_fbs binary schemas, so recordings are self-describing for any MCAP
tool. The legacy `.csynbag` format is retired.

The CLI uses the published `synapse_fbs` crate matching the Zephyr module's
pinned C release asset.

## Testing

CI runs entirely on hosted GitHub runners with no hardware: twister builds
and runs the module tests on native_sim (using the repo's own `west.yml` as
a CI workspace manifest), and the CLI runs `cargo fmt`/`clippy`/`test`.
Formatting is enforced with the Zephyr `.clang-format` and rustfmt. Board
targets may be added to `platform_allow` for optional local twister runs,
but must stay out of `integration_platforms` so CI never needs hardware.

```sh
west twister -T zephyr/tests -p native_sim --inline-logs   # Zephyr module
cd rust && cargo test                                      # host CLI
```

## License

Apache-2.0
