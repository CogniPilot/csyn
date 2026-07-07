{
  description = "csyn CI and development tooling";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems =
        f:
        lib.genAttrs systems (
          system:
          f system (import nixpkgs {
            inherit system;
          })
        );

      pythonEnv =
        pkgs:
        pkgs.python3.withPackages (
          ps: with ps; [
            anytree
            colorama
            coverage
            intelhex
            jinja2
            jsonschema
            junitparser
            kconfiglib
            natsort
            packaging
            psutil
            pyelftools
            pykwalify
            pyserial
            pytest
            pyyaml
            requests
            semver
            tabulate
            tqdm
            west
          ]
        );

      toolPackages =
        pkgs:
        [
          pkgs.cargo
          pkgs.rustc
          pkgs.rustfmt
          pkgs.clippy
          pkgs.clang-tools
          pkgs.cmake
          pkgs.gnumake
          pkgs.ninja
          pkgs.git
          (pythonEnv pkgs)
          pkgs.dtc
          pkgs.gperf
          pkgs.file
          pkgs.which
        ]
        ++ lib.optionals (pkgs.stdenv.hostPlatform.system == "x86_64-linux") [
          pkgs.gcc_multi
          pkgs.glibc_multi
        ];

      mkScript =
        pkgs: name: text:
        pkgs.writeShellApplication {
          name = "csyn-${name}";
          runtimeInputs = toolPackages pkgs;
          inherit text;
        };

      nativeSimEnv = ''
        export NIX_HARDENING_ENABLE="format stackprotector pic strictoverflow relro bindnow"
      '';
    in
    {
      packages = forAllSystems (
        system: pkgs:
        let
          script = mkScript pkgs;
        in
        rec {
          fmt = script "fmt" ''
            cargo fmt --check --manifest-path rust/Cargo.toml
            clang-format --dry-run -Werror zephyr/src/*.c zephyr/include/csyn/*.h zephyr/tests/csyn/basic/src/*.c
          '';

          clippy = script "clippy" ''
            cargo clippy --locked --manifest-path rust/Cargo.toml --all-targets -- -D warnings
          '';

          test-rust = script "test-rust" ''
            cargo test --locked --manifest-path rust/Cargo.toml
          '';

          test-zephyr = script "test-zephyr" ''
            ${nativeSimEnv}
            west twister -T zephyr/tests -v --inline-logs --integration
          '';

          ci = script "ci" ''
            cargo fmt --check --manifest-path rust/Cargo.toml
            clang-format --dry-run -Werror zephyr/src/*.c zephyr/include/csyn/*.h zephyr/tests/csyn/basic/src/*.c
            cargo clippy --locked --manifest-path rust/Cargo.toml --all-targets -- -D warnings
            cargo test --locked --manifest-path rust/Cargo.toml
            ${nativeSimEnv}
            west twister -T zephyr/tests -v --inline-logs --integration
          '';

          default = ci;
        }
      );

      apps = forAllSystems (
        system: pkgs:
        let
          pkg = self.packages.${system};
          mkApp = name: {
            type = "app";
            program = "${pkg.${name}}/bin/csyn-${name}";
          };
        in
        {
          fmt = mkApp "fmt";
          clippy = mkApp "clippy";
          test-rust = mkApp "test-rust";
          test-zephyr = mkApp "test-zephyr";
          ci = mkApp "ci";
          default = mkApp "ci";
        }
      );

      devShells = forAllSystems (
        system: pkgs:
        {
          default = pkgs.mkShell {
            packages = toolPackages pkgs;
            hardeningDisable = [ "fortify" ];
            NIX_HARDENING_ENABLE = "format stackprotector pic strictoverflow relro bindnow";
            ZEPHYR_TOOLCHAIN_VARIANT = "host";
          };
        }
      );
    };
}
