# StripeKit C++

StripeKit C++ is a C++ toolkit for integrating with Stripe for processing payments.

It's designed to be platform/framework agnostic, with a pluggable backend for making 
HTTP API calls and receiving webhooks. For now, it only includes a backend for [drogon](https://drogon.org/).

This is part of a larger project of mine, which is a full web app framework/starter-kit.

## Status

Phase 1 foundation is in place:

- Buildable CMake skeleton.
- Install and export targets for downstream CMake consumers.
- Generated version header integration.
- Baseline docs, license, and CI placeholders.

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

The project exports a package config so downstream consumers can use:

```cmake
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit)
```

**TODO: Add details of build options (e.g., how to use your own libsodium or drogon instead of StripeKit fetching it**

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
