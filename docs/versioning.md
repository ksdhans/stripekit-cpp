# Versioning and Release Tags

StripeKit C++ uses Semantic Versioning (SemVer):

- MAJOR: incompatible public API changes.
- MINOR: backwards-compatible feature additions.
- PATCH: backwards-compatible fixes and maintenance.

## Tag Format

Release tags must follow:

- `vMAJOR.MINOR.PATCH`

Examples:

- `v0.1.0`
- `v1.0.0`
- `v1.3.4`

## Pre-release Tags

Pre-release tags may be used for release candidates:

- `vMAJOR.MINOR.PATCH-rc.N`

Example:

- `v0.2.0-rc.1`

## Build Metadata

Build-time metadata is generated into `stripekit_rev.h`:

- `STRIPEKIT_VERSION`
- `STRIPEKIT_BUILD_ID`
- `STRIPEKIT_BUILD_DATE`

This metadata is generated through `include(cmake/generate_version_header.cmake)` during builds.
