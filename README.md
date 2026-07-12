# csyn

[![CI](https://github.com/CogniPilot/csyn/actions/workflows/ci.yml/badge.svg)](https://github.com/CogniPilot/csyn/actions/workflows/ci.yml)
[![crates.io](https://img.shields.io/crates/v/csyn.svg)](https://crates.io/crates/csyn)

csyn is the CogniPilot synapse topic toolkit. One repo, two sides of the wire,
one folder per artifact:

- **`zephyr/`**: the west module — a lock-free latest-sample topic store for
  embedded vehicles, with zenoh and native_sim UDP transports, a zros bridge,
  and `csyn topic list/info/echo/hz/watch` shell diagnostics. csyn defines no
  topics of its own: applications declare their topic list with
  `CSYN_TOPIC_DEFINE()`, and the store, shell, and transports iterate whatever
  was declared. Topic payloads are rendered by the synapse_fbs-generated
  field-descriptor printer, so every fixed-layout topic prints without
  hand-written formatting code.
- **`rust/`**: the host-side `csyn` CLI, a ROS-like command-line tool for
  Synapse systems using Zenoh for transport (see `rust/README.md`).

Both sides speak the synapse_fbs schema through a pinned release of the same
version: the Zephyr module pins the C release tarball in
`zephyr/CMakeLists.txt`, while the CLI pins the `synapse_fbs` crate, which
embeds the schema sources, compiled binary schemas, topic catalog, and
generated decoder. Every topic's type, encoding, schema, and catalog id resolve
from the generated catalog; applications provide the deployment-specific wire
key with `CSYN_TOPIC_DEFINE()`. The wire contract is locked by csyn rather than
vendored per application.

Csyn `v0.6.0` pins synapse_fbs `0.8.0`. This release uses the `mocap`, `odom`,
and `odom_cov` catalog topics and expects vehicles to allocate their selected
in-process topics with the native ZROS definition macros.

On Zenoh, every value must carry the canonical Synapse contract metadata: media
type, fully qualified wire type, and that individual type's transitive schema
fingerprint (truncated SHA-256). The Rust CLI and Zephyr transport refuse
metadata-free or mismatched samples and throttle repeated warnings to once per
topic every ten seconds.

Check the compiled schema release with `csyn --version` or `csyn build-info`
on the host. On the Zephyr shell, `csyn status` prints the pinned
`synapse_fbs` release compiled into the module.

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
`CONFIG_CSYN_NATIVE_UDP=y` (native_sim). `CONFIG_CSYN_NAMESPACE` optionally
scopes bare topic keys, e.g. `"cub1"` makes an `"att"` declaration publish on
`cub1/att`. Namespaced keys declared by the vehicle are used verbatim. Both
forms follow the synapse grammar `[<namespace>/]<key>[/<instance>]`; the host
CLI resolves namespaced keys without configuration. Declare the topics your
application carries with `CSYN_TOPIC_DEFINE(symbol, key, dir, max_size)`;
each key must end in a canonical catalog key, and init fails on unknown keys
or fixed-layout size mismatches:

```c
#include <csyn/csyn.h>
#include <csyn/csyn_zros.h>

CSYN_TOPIC_DEFINE(att, "att", CSYN_DIR_TX, sizeof(synapse_topic_AttitudeEstimateData_t));
CSYN_TOPIC_DEFINE(manual, "manual", CSYN_DIR_RX, sizeof(synapse_topic_ManualControlData_t));
CSYN_TOPIC_DEFINE(mocap, "vicon/mocap", CSYN_DIR_RX, CONFIG_CSYN_FLATBUFFER_MAX_SIZE);
CSYN_TOPIC_DEFINE(odom, "vicon/cub1/odom", CSYN_DIR_RX,
		  sizeof(synapse_topic_OdometryData_t));
CSYN_TOPIC_DEFINE(odom_cov, "vicon/cub1/odom_cov", CSYN_DIR_RX,
		  sizeof(synapse_topic_OdometryWithCovarianceData_t));
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(attitude_estimate,
				   synapse_topic_AttitudeEstimateData_t);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(manual_control, struct csyn_manual_control);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(mocap, struct csyn_mocap_rigid_body);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(odometry, synapse_topic_OdometryData_t);
```

Each native ZROS definition explicitly selects and allocates one topic in a
vehicle-owned translation unit. Undeclared topics allocate no storage and are
skipped by the bridge. Csyn only declares the topic interfaces and provides the
bridge functions.

The macro key may be bare or namespaced. The final key segment selects the
synapse_fbs 0.8 catalog type, while the complete declared key is used on the
wire. Source-specific namespaces therefore stay in the vehicle configuration:
the same firmware setup can use `vicon/cub1/odom`, `qualisys/cub1/odom`, or
another deployment path without csyn hardcoding a mocap vendor.

Applications publish and subscribe through the zros topics declared in
`<csyn/csyn_zros.h>`; the bridge mirrors whichever bridged topics the
application declared to the active transport.

Layout (everything for the module lives under `zephyr/`):

- `zephyr/include/csyn/csyn.h` — topic registry and store API
- `zephyr/include/csyn/csyn_codec.h` — payload decode/encode plus PWM/axis
  and quaternion/euler helpers
- `zephyr/include/csyn/csyn_types.h` — plain in-process types (rc channels,
  manual control)
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

Bags use the `synapse/1` MCAP profile built into synapse_fbs 0.8. Schema
records carry canonical rooted topic names and embedded binary schemas, while
required file metadata records the schema-set hash, session, source, and time
basis. The legacy `.csynbag` format is retired.

The CLI uses the published `synapse_fbs` crate matching the Zephyr module's
pinned C release asset.

## Testing

CI runs entirely on hosted GitHub runners with no hardware: twister builds
and runs the module tests on native_sim (using the repo's own `west.yml` as
a CI workspace manifest), and the CLI runs `cargo fmt`/`clippy`/`test`.
Formatting is enforced with the Zephyr `.clang-format` and rustfmt. Board
targets may be added to `platform_allow` for optional local twister runs,
but must stay out of `integration_platforms` so CI never needs hardware.

The Nix flake provides host tools only; Zephyr and zros revisions still come
from `west.yml`. Enter the development shell from the csyn repo root with:

```sh
nix develop
```

Inside that shell, the normal host tools are available:

```sh
cargo test --locked --manifest-path rust/Cargo.toml
clang-format --dry-run -Werror zephyr/src/*.c zephyr/include/csyn/*.h zephyr/tests/csyn/basic/src/*.c
west --version
```

You can also run host checks without entering a shell:

```sh
nix develop -c cargo fmt --check --manifest-path rust/Cargo.toml
nix develop -c clang-format --dry-run -Werror zephyr/src/*.c zephyr/include/csyn/*.h zephyr/tests/csyn/basic/src/*.c
nix develop -c cargo clippy --locked --manifest-path rust/Cargo.toml --all-targets -- -D warnings
nix develop -c cargo test --locked --manifest-path rust/Cargo.toml
```

Run Twister from an existing west workspace with:

```sh
nix develop -c west twister -T zephyr/tests -v --inline-logs --integration
```

For a fresh Zephyr workspace, keep csyn checked out at `modules/lib/csyn`,
then initialize and update west from the workspace root:

```sh
mkdir -p .west
printf '[manifest]\npath = modules/lib/csyn\nfile = west.yml\n\n[zephyr]\nbase = zephyr\n' > .west/config
nix develop ./modules/lib/csyn -c west update
nix develop ./modules/lib/csyn -c env ZEPHYR_BASE="$PWD/zephyr" python zephyr/scripts/twister -T modules/lib/csyn/zephyr/tests -v --inline-logs --integration
```

## Releases

GitHub Actions publishes the Rust CLI crate to crates.io when a tag matching
`vMAJOR.MINOR.PATCH` is pushed. The tag version must match
`rust/Cargo.toml`, so the `0.5.0` release is:

```sh
git tag v0.5.0
git push origin v0.5.0
```

The release workflow runs the Nix flake check, Rust formatting, clippy, tests,
and a `cargo publish --dry-run` before publishing. crates.io Trusted Publishing
is configured for the `CogniPilot/csyn` repository and the `release.yml`
workflow, so no repository publish secret is required.

## License

Apache-2.0
