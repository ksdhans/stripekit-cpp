# StripeKit C++

StripeKit C++ is a C++ toolkit for integrating with Stripe for processing payments.

It's designed to be platform/framework agnostic, with a pluggable backend for making 
HTTP API calls and receiving webhooks. For now, it only includes a backend for [drogon](https://drogon.org/).

This is part of a larger project of mine, which is a full web app framework/starter-kit.

## Status

Still a work in progress.

**TODO: Update once we reach MVP**

## Roadmap

Things that may be done later (if the need arises):

- Broad direct `payment_intents` client surface.
- Non-card payment methods.
- Webhook families outside the current checkout/subscription/invoice/dispute/refund scope.
- Additional first-party framework adapters beyond Drogon.
- Expanded billing portal configuration helpers beyond session creation.

## Build

Requirements:

- CMake 3.24+
- C++20 compiler

Configure and build:

```bash
cmake -S . -B build
cmake --build build
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Install locally:

```bash
cmake --install build --prefix "$PWD/build/install"
```

## Integrating With Your Project(s)

StripeKit exports a CMake package config, so downstream consumers can link it with:

```cmake
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit)
```

By default, StripeKit uses FetchContent for dependencies it needs when no existing dependency is provided. If your project already provides libsodium or Drogon, set the matching external switch and let CMake discover the dependency through the normal package search rules:

```bash
cmake -S . -B build \
	-DUSE_EXTERNAL_LIBSODIUM=ON \
	-DUSE_EXTERNAL_DROGON=ON
```

When the external switches are on, StripeKit looks for existing CMake targets first. That means a parent project can add libsodium or Drogon before adding StripeKit, and StripeKit will reuse those targets instead of building or fetching a second copy.

`find_package()` follows normal CMake discovery rules. For an installed dependency, point CMake at the install prefix or package config location using the standard hints:

- `CMAKE_PREFIX_PATH`
- `<PackageName>_ROOT` when the package supports it
- `<PackageName>_DIR` for package config locations

For example, if your system installs libsodium and Drogon under a common prefix, you can point CMake at that prefix and let `find_package()` do the rest:

```bash
cmake -S . -B build \
	-DUSE_EXTERNAL_LIBSODIUM=ON \
	-DUSE_EXTERNAL_DROGON=ON \
	-DCMAKE_PREFIX_PATH="/opt/libsodium;/opt/drogon"
```

If you are already building libsodium or Drogon in the parent project, StripeKit will also reuse the existing `sodium` or `Drogon::Drogon` target when it is present. If the dependency lives only in source form and does not publish a package config, add it before StripeKit with `add_subdirectory()` so the target already exists when StripeKit configures.

## Versioning

Version metadata is generated at build time via:

- `include(cmake/generate_version_header.cmake)`

See `docs/versioning.md` for release and tag policy.

## Project Layout

- `include/StripeKit/` public headers
- `src/core/` framework-agnostic core implementation
- `src/adapters/drogon/` the drogon backend
- `tests/unit/` fast isolated unit tests
- `tests/integration/` adapter integration tests
- `tests/e2e_sandbox/` Stripe sandbox E2E tests
- `examples/` consumer examples
- `docs/` project documentation

## License

This project has a BSD-3-Clause license. See `LICENSE.md`.
