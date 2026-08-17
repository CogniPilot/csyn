CARGO ?= cargo
CLANG_FORMAT ?= clang-format-18
PYTHON ?= python3
ZEPHYR_BASE ?=
TWISTER_OUT ?= $(CURDIR)/twister-out

RUST_MANIFEST = rust/Cargo.toml
C_FORMAT_INPUTS = zephyr/src/*.c zephyr/include/csyn/*.h zephyr/tests/csyn/basic/src/*.c

.PHONY: fmt-rust fmt-c fmt lint-rust test-rust check-rust test-zephyr ci
.NOTPARALLEL:

fmt-rust:
	$(CARGO) fmt --check --manifest-path $(RUST_MANIFEST)

fmt-c:
	$(CLANG_FORMAT) --dry-run -Werror $(C_FORMAT_INPUTS)

fmt: fmt-rust fmt-c

lint-rust:
	$(CARGO) clippy --locked --manifest-path $(RUST_MANIFEST) --all-targets -- -D warnings

test-rust:
	$(CARGO) test --locked --manifest-path $(RUST_MANIFEST)

check-rust: fmt-rust lint-rust test-rust

test-zephyr:
	@if [ -z "$(ZEPHYR_BASE)" ]; then echo "ZEPHYR_BASE must point to the workspace Zephyr checkout" >&2; exit 2; fi
	@if [ ! -f "$(ZEPHYR_BASE)/scripts/twister" ]; then echo "Twister is missing below ZEPHYR_BASE=$(ZEPHYR_BASE)" >&2; exit 2; fi
	ZEPHYR_TOOLCHAIN_VARIANT=host $(PYTHON) $(ZEPHYR_BASE)/scripts/twister \
		-T $(CURDIR)/zephyr/tests \
		--outdir $(TWISTER_OUT) \
		-v --inline-logs --integration

ci: fmt lint-rust test-rust test-zephyr
