# Constant Integration (CI) Scripts

This directory is reserved for CI helper scripts (for things such as automated testing).

All pipeline jobs use Ubuntu 26.04.

## C++ Formatting

The `verify:format` job uses clang-format 21 and the repository's `.clang-format`
configuration. To run the same check locally from the repository root:

```sh
find include src tests examples -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 | xargs -0 clang-format-21 --dry-run --Werror
```

On Ubuntu 26.04, install it with `sudo apt install clang-format-21`.
Configure your editor to use `clang-format-21` as well.
The VS Code workspace settings use `/usr/bin/clang-format-21` with Microsoft's
C/C++ extension and format C++ files on save. If the executable is installed
elsewhere, update `C_Cpp.clang_format_path` in the workspace settings.

To apply the formatting, replace `--dry-run --Werror` with `-i`.
Use a trailing comma after the last member of a multiline initializer list to
keep its closing brace on a separate line.

## Installed Package Check

The default CTest run includes `stripekit.packaging`. It installs into
`build/package-test/install`, then configures, builds, and runs a separate
consumer using `find_package(StripeKit CONFIG REQUIRED)`. This check does not
use Stripe credentials or make Stripe API requests. Build the project before
running this test. The test installs the existing build outputs.

`stripekit.dependency-reuse` checks that a parent project's libsodium and Drogon
targets are reused without fetching second copies.
