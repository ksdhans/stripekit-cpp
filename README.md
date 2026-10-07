# StripeKit C++

StripeKit C++ is a C++ toolkit for integrating with Stripe for processing payments.

It's designed to be platform/framework agnostic, with a pluggable backend for making
HTTP API calls and receiving webhooks. It ships with [Drogon](https://drogon.org/)
as the default backend (adaptor).

This is part of a larger project of mine, which is a full web app [foundation/starter-kit](https://zitakit.com/) called [ZitaKit](https://zitakit.com/).

## Status

StripeKit supports Stripe-hosted Checkout for subscriptions and one-time card
payments. It sends API requests, parses responses, and verifies and parses webhooks.

It covers a subset of Stripe's API, focused on hosted Checkout. Embedded/custom
payment forms and the full range of Stripe products and API options are not
currently supported.

Your application decides what to do with those results: billing policies, access
control, and other business logic are up to you.

Start with the [Quickstart](docs/Quickstart.md) and [runnable examples](examples/README.md).
For adding support for other web frameworks, see [Writing an Adapter](docs/WritingAnAdapter.md).

## Roadmap

Features are added as practical needs arise, rather than on a fixed schedule.
Current development focuses on Stripe-hosted Checkout. Embedded/custom checkout
may come further down the line.

## Build

Requirements:

- CMake 3.24+
- C++20 compiler; GCC 12 is the oldest version tested
- C compiler and `make`, when CMake builds the bundled libsodium fallback
- On Linux: UUID development headers and library (`uuid-dev` on Debian/Ubuntu,
	`libuuid-devel` on Fedora/RHEL)
- Zlib development headers and library (`zlib1g-dev` on Debian/Ubuntu,
	`zlib-devel` on Fedora/RHEL)
- TLS development headers and library. StripeKit uses Stripe's HTTPS API, so
	install `libssl-dev` on Debian/Ubuntu or `openssl-devel` on Fedora/RHEL.

On Debian/Ubuntu, install the minimal build dependencies with:

```bash
sudo apt install build-essential cmake git uuid-dev zlib1g-dev libssl-dev libjsoncpp-dev
```

Drogon requires Trantor, JsonCpp, UUID, and Zlib on Linux. CMake fetches Drogon
and its Trantor dependency. Install JsonCpp, UUID, Zlib, and TLS development
packages on the host system. StripeKit reuses an existing JsonCpp target or
installed package before fetching a fallback, so it can share the system library
used by Drogon. YAML, Brotli, and c-ares support are
optional Drogon features and are not required by StripeKit.

Configure and build:

```bash
cmake -S . -B build
cmake --build build
```

Install locally:

```bash
cmake --install build --prefix "$PWD/build/install"
```

## Running Tests

StripeKit organizes tests into unit, integration, and sandbox E2E test suites executed via CTest or directly through their test binaries.

### Running Test Suites with CTest

Run all configured tests:

```bash
ctest --test-dir build --output-on-failure
```

List all available CTest test suites:

```bash
ctest --test-dir build -N
```

Run only the unit test suite:

```bash
ctest --test-dir build -R stripekit.unit --output-on-failure
```

Run only the integration test suite:

```bash
ctest --test-dir build -R stripekit.integration --output-on-failure
```

Run only the sandbox E2E test suite (when enabled with `STRIPEKIT_BUILD_SANDBOX_TESTS=ON`):

```bash
ctest --test-dir build -R stripekit.sandbox -V
```

### Stripe Sandbox E2E Tests

The sandbox suite runs against a real Stripe sandbox, so it is opt-in and is not
built by default. Enable it at configure time:

```bash
cmake -S . -B build -DSTRIPEKIT_BUILD_SANDBOX_TESTS=ON
cmake --build build
```

It needs the Stripe sandbox **secret API key**, not the publishable key. Copy the
`sk_test_...` value labelled **Secret key** under Dashboard > Developers > API
keys while the sandbox is selected. The `pk_test_...` Publishable key cannot be
used. Supply the secret key and the Stripe CLI through environment variables so
local shells and CI runners share one contract. To keep the variables scoped to
the test command, load the git-ignored `.env.local` file in a subshell:

```bash
(
	set -a
	. ./.env.local
	set +a
	ctest --test-dir build -R stripekit.sandbox -V
)
```

The variables are inherited by `ctest` and the sandbox test process, then leave
the environment when the subshell exits.

Inbound webhooks arrive through `stripe listen`, which connects outbound to
Stripe. No public hostname, tunnel, or inbound port is needed, so the suite works
behind NAT and on ephemeral CI runners.

Without `STRIPEKIT_SANDBOX_ENABLED` the suite skips and reports success, so
contributors without Stripe access are unaffected.

See [tests/sandbox/README.md](tests/sandbox/README.md) for full setup, including
how to create a dedicated CI sandbox and how to store the key as a protected,
masked GitLab CI/CD variable.

### Running and Listing Individual Tests

You can also run the test executables directly to list all test cases or run specific individual tests:

List all available tests in a suite:

```bash
# List unit tests
./build/bin/stripekit_unit_tests --list

# List integration tests
./build/bin/stripekit_integration_tests --list
```

Run an individual test or a filtered subset by passing a name substring:

```bash
# Run a specific unit test
./build/bin/stripekit_unit_tests WebhookVerifierTest.ValidSignaturePasses

# Run all tests in a test fixture/suite
./build/bin/stripekit_unit_tests ModelsAndParserTest

# Run a specific integration test
./build/bin/stripekit_integration_tests DrogonWebhookEndpointIntegrationTest.ValidInvoicePaidReturns200AndInvokesHandler
```

## Integrating With Your Project(s)

StripeKit exports a CMake package config, so downstream consumers can link it with:

```cmake
find_package(StripeKit CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE StripeKit::stripekit)
```

Linking StripeKit automatically requires C++20 for your target; no manual
`CMAKE_CXX_STANDARD` setting is needed.
Configure your application with `-DCMAKE_PREFIX_PATH=/path/to/stripekit/install`
so CMake can find the installed package and its dependencies.
The installed package includes the fetched libsodium archive when the fallback
is used. JsonCpp, SQLite (when enabled), and external Drogon/libsodium packages
must also be available to the consumer.

To include StripeKit directly in a parent project instead of installing it:

```cmake
set(STRIPEKIT_BUILD_TESTS OFF CACHE BOOL "")
set(STRIPEKIT_BUILD_EXAMPLES OFF CACHE BOOL "")
add_subdirectory(external/StripeKit)
target_link_libraries(your_target PRIVATE StripeKit::stripekit StripeKit::drogon)
```

Add any dependency targets supplied by the parent before `add_subdirectory()`.

The Drogon runtime adapter is enabled by default. Link `StripeKit::drogon` in
addition to `StripeKit::stripekit`:

```cmake
target_link_libraries(your_target PRIVATE StripeKit::stripekit StripeKit::drogon)
```

Create a Stripe client through the Drogon adapter (or an adaptor for your chosen web framework). StripeKit uses JsonCpp for
Stripe response parsing by default:

```cpp
#include <StripeKit/Adapters/Drogon.h>
#include <StripeKit/StripeKit.h>

auto stripe = stripekit::drogon::make_stripe_client({
	.api_key = "sk_test_..."
});
```

Applications that need custom response decoding can still provide their own
`StripeResponseParser`:

```cpp
stripekit::drogon::HTTPClient http_client("sk_test_...");
stripekit::StripeApiClient stripe(http_client, custom_parser);
```

Register a Drogon webhook handler with StripeKit verification and durable
de-duplication:

```cpp
stripekit::SqliteWebhookDedupStore dedup_store("stripe-webhooks.sqlite3");

stripekit::drogon::register_webhook_handler({
	.path = "/stripe/webhook",
	.endpoint_secret = "whsec_...",
	.dedup_store = dedup_store,
	.handler = [&](const stripekit::webhooks::Event& event)
		-> stripekit::Task<stripekit::webhooks::ProcessResult> {
		if (event.type == "invoice.paid") {
			// Update the application's own subscription records here.
			co_return stripekit::webhooks::ProcessResult::Handled;
		}

		co_return stripekit::webhooks::ProcessResult::Ignored;
	}
});
```

Framework adapters are thin request/response glue. If you are integrating with a
different framework, use the core webhook API directly with the raw request body
and `Stripe-Signature` header:

```cpp
auto event = stripekit::webhooks::construct_event(
	raw_body,
	stripe_signature_header,
	"whsec_..."
);

if (event.type == "invoice.paid") {
	// Update the application's own subscription records here.
}
```

For production webhook handling with durable de-duplication, use
`process_event_once`:

```cpp
auto result = co_await stripekit::webhooks::process_event_once(
	raw_body,
	stripe_signature_header,
	"whsec_...",
	dedup_store,
	[&](const stripekit::webhooks::Event& event)
		-> stripekit::Task<stripekit::webhooks::ProcessResult> {
		// Update the application's own database here.
		co_return stripekit::webhooks::ProcessResult::Handled;
	}
);
```

Projects that provide another adapter can disable Drogon with
`-DSTRIPEKIT_BUILD_DROGON_ADAPTER=OFF`. Tests require at least one enabled
adapter and run only the tests for enabled adapters.

By default, StripeKit downloads and builds dependencies it needs when no existing dependency is provided. If your project already provides libsodium or Drogon, set the matching external switch and let CMake discover the dependency through the normal package search rules:

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

## API Documentation

The API reference is generated from the Doxygen comments in the public headers.
Standalone builds generate it automatically when Doxygen is installed, so after a
normal build it is available at `build/docs/html/index.html`.

The published documentation (for the main branch) is available at:
https://keasigmadelta.gitlab.io/stripekit-cpp/

On Debian/Ubuntu, install Doxygen (and Graphviz, for the class diagrams) with:

```bash
sudo apt install doxygen graphviz
```

To build the reference on its own, without building the library:

```bash
cmake -S . -B build-docs -DSTRIPEKIT_BUILD_DOCS=ON -DSTRIPEKIT_BUILD_TESTS=OFF
cmake --build build-docs --target stripekit-docs
```

Use `-DSTRIPEKIT_BUILD_DOCS=OFF` to skip the reference, and `-DSTRIPEKIT_BUILD_DOCS=ON`
to make the build fail when Doxygen is missing rather than silently skipping it.
The generated HTML is not checked into the repository.

Additional usage notes are in `docs/`.

## Project Layout

- `include/StripeKit/` public headers
- `src/core/` framework-agnostic core implementation
- `src/adapters/drogon/` the drogon backend
- `tests/unit/` fast isolated unit tests
- `tests/integration/` adapter integration tests
- `tests/sandbox/` Stripe sandbox E2E tests
- `examples/` consumer examples
- `docs/` project documentation

## License

This project has a BSD-3-Clause license. See `LICENSE`.
