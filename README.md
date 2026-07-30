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
- Webhook families outside checkout/subscription/invoice/dispute/refund MVP set.
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

**TODO: Make this clearer for a developer who has never used this before**

The project exports a package config so downstream consumers can use:

```cmake
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit)
```

**TODO: Add details of build options (e.g., how to use your own libsodium or drogon instead of StripeKit fetching it)**

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
