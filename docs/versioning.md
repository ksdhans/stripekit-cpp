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

To tag the release, run the `tag-release.sh` script:

```sh
./tag-release.sh
```

The script verifies that:

- You are on the `main` branch.
- The working directory has no uncommitted changes or untracked files.
- The local `main` branch is in sync with `origin/main`.
- The project version in [CMakeLists.txt](../CMakeLists.txt) matches SemVer format.
- The release tag does not already exist locally or on the remote.
- [Changelog.md](../Changelog.md) contains release notes and the release date for this version, and the `Unreleased` section has been cleared.

You can preview the checks without creating a tag by passing `--dry-run`:

```sh
./tag-release.sh --dry-run
```

To create the tag and automatically push it to the remote repository:

```sh
./tag-release.sh --push
```

To tag manually instead:

```sh
git tag -a v0.1.0 -m "StripeKit 0.1.0"
git show --no-patch v0.1.0
git push origin v0.1.0
```

Replace the version in these commands for later releases or release candidates.
Push the specific tag rather than all local tags. Do not move a published release
tag; publish a new version if a correction is needed.

Create a GitLab release from the tag and use that version's changelog entry as
its release notes.

## Build Metadata

Build-time metadata is generated into `stripekit_rev.h`:

- `STRIPEKIT_VERSION`
- `STRIPEKIT_BUILD_ID`
- `STRIPEKIT_BUILD_DATE`

This metadata is generated through `include(cmake/generate_version_header.cmake)` during builds.
