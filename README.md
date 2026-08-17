# csyn

[![CI](https://github.com/CogniPilot/csyn/actions/workflows/ci.yml/badge.svg)](https://github.com/CogniPilot/csyn/actions/workflows/ci.yml)
[![crates.io](https://img.shields.io/crates/v/csyn.svg)](https://crates.io/crates/csyn)

csyn is the CogniPilot synapse topic toolkit. One repo, two sides of the wire,
one folder per artifact:

- **`zephyr/`**: the west module - a lock-free latest-sample topic store for
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

This branch aligns the Rust CLI with the synapse_fbs `0.10.0` C package
already pinned by the `golden` Zephyr module. The tagged csyn `v0.7.0`
release pinned synapse_fbs `0.9.0` for the Rust CLI. The current branch uses
the `mocap`, `odom`, and `odom_cov` catalog topics and expects vehicles to
allocate their selected in-process topics with the native ZROS definition
macros.

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
corresponding pinned synapse_fbs catalog type, while the complete declared key
is used on the wire. Source-specific namespaces therefore stay in the vehicle
configuration: the same firmware setup can use `vicon/cub1/odom`,
`qualisys/cub1/odom`, or another deployment path without csyn hardcoding a
mocap vendor.

Applications publish and subscribe through the zros topics declared in
`<csyn/csyn_zros.h>`; the bridge mirrors whichever bridged topics the
application declared to the active transport.

Layout (everything for the module lives under `zephyr/`):

- `zephyr/include/csyn/csyn.h` - topic registry and store API
- `zephyr/include/csyn/csyn_codec.h` - payload decode/encode plus PWM/axis
  and quaternion/euler helpers
- `zephyr/include/csyn/csyn_types.h` - plain in-process types (rc channels,
  manual control)
- `zephyr/include/csyn/csyn_zros.h` - zros topic declarations vehicles use
- `zephyr/src/` - store, codec, bridge, shell, and transports
- `zephyr/{module.yml,Kconfig,CMakeLists.txt}` - west integration and the
  pinned synapse_fbs release

### Local schema source override

An unpublished synapse_fbs change can be compiled without creating a tag or
release. Generate the local C package in the synapse_fbs checkout first:

```sh
make local-offline
```

Pass its absolute package path to the existing Zephyr build command:

```sh
-- -DFETCHCONTENT_SOURCE_DIR_SYNAPSE_FBS_C:PATH=/absolute/path/to/synapse_fbs/target/xtask/packages/c
```

Ordinary Zephyr builds consume the standard CMake FetchContent source
override directly. In sysbuild, csyn imports the same global cache value into
each image, so one command-line value covers the application and bootloader.
The generated package is used instead of the release archive and requires no
schema publication or network fetch.

## Rust CLI

The host tool lives in `rust/`:

```sh
cd rust
cargo run -- topic list
```

Bags use the `synapse/1` MCAP profile built into the pinned synapse_fbs
release. Schema records carry canonical rooted topic names and embedded binary
schemas, while required file metadata records the schema-set hash, session,
source, and time basis. The legacy `.csynbag` format is retired.

The CLI uses the published `synapse_fbs` crate matching the Zephyr module's
pinned C release asset.

## Testing

CI runs entirely on hosted GitHub runners with no hardware: twister builds
and runs the module tests on native_sim (using the repo's own `west.yml` as
a CI workspace manifest), and the CLI runs `cargo fmt`/`clippy`/`test`.
Formatting is enforced with the Zephyr `.clang-format` and rustfmt. Board
targets may be added to `platform_allow` for optional local twister runs,
but must stay out of `integration_platforms` so CI never needs hardware.

Host checks require GNU Make and rustup. They use the Rust toolchain pinned in
`rust-toolchain.toml` plus `clang-format-18`. On Ubuntu 24.04, install the
native host tools with:

```sh
sudo apt-get install clang-format-18 make
```

Rustup reads `rust-toolchain.toml` automatically. Run the native checks from
the repository root:

```sh
make fmt
make lint-rust
make test-rust
```

Run all host checks with:

```sh
make check-rust
make fmt-c
```

Twister additionally requires the native Zephyr build packages and a Python
environment containing the pinned west release:

```sh
sudo apt-get install \
  build-essential ccache cmake device-tree-compiler file \
  g++-multilib gcc-multilib git gperf make ninja-build \
  python3-dev python3-pip python3-venv
python3 -m venv /path/to/zephyr-venv
/path/to/zephyr-venv/bin/pip install --upgrade pip
/path/to/zephyr-venv/bin/pip install -r .github/requirements-zephyr.txt
/path/to/zephyr-venv/bin/pip install \
  -r /path/to/workspace/zephyr/scripts/requirements.txt
```

From an initialized west workspace, give the native command the exact Zephyr
checkout and Python environment:

```sh
make -C /path/to/csyn test-zephyr \
  ZEPHYR_BASE=/path/to/workspace/zephyr \
  PYTHON=/path/to/zephyr-venv/bin/python
```

The CI-only `west.yml` pins Zephyr and zros. Vehicle workspaces continue to
select their own manifests and revisions.

## Releases

GitHub Actions publishes the Rust CLI crate to crates.io when a tag matching
`vMAJOR.MINOR.PATCH` is pushed. The tag version must match
`rust/Cargo.toml`, so the `0.7.0` release is:

```sh
git tag v0.7.0
git push origin v0.7.0
```

The release workflow runs the pinned native Rust formatting, Clippy, tests,
and a `cargo publish --dry-run` before publishing. crates.io Trusted
Publishing is configured for the `CogniPilot/csyn` repository and the
`release.yml` workflow, so no repository publish secret is required.

## License

Apache-2.0
