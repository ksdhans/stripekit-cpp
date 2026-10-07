# Versioning and Release Tags {#versioning}

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

## Creating a Release

Before tagging:

1. Set the project version in [CMakeLists.txt](../CMakeLists.txt) to the release
   version.
2. Move the release notes from `Unreleased` in [Changelog.md](../Changelog.md)
   into a section such as `## 0.1.0 - YYYY-MM-DD`, using the release date.
   Leave an empty `Unreleased` section for future changes.
3. Commit and push the changes. Confirm that CI, including the real Stripe
   sandbox tests, passes for that commit.
4. Check out that tested commit and confirm `git status --short` is empty.

Create an annotated tag on the current commit, then push that tag. For example,
for version 0.1.0:

```sh
git tag -a v0.1.0 -m "StripeKit 0.1.0"
git show --no-patch v0.1.0
git push origin v0.1.0
```

Replace the version in these commands for later releases or release candidates.
Push the specific tag rather than all local tags. Do not move a published release
tag; publish a new version if a correction is needed.

Create a GitLab release from the tag and use that version's changelog entry as
its release notes. These few commands do not require a separate release script.

## Build Metadata

Build-time metadata is generated into `stripekit_rev.h`:

- `STRIPEKIT_VERSION`
- `STRIPEKIT_BUILD_ID`
- `STRIPEKIT_BUILD_DATE`

This metadata is generated through `include(cmake/generate_version_header.cmake)` during builds.
