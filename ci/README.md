# Constant Integration (CI) Scripts

This directory is reserved for CI helper scripts (for things such as automated testing).

## C++ Formatting

The `verify:format` job uses clang-format 14 and the repository's `.clang-format`
configuration. Run the same check locally from the repository root:

```sh
find include src tests examples -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 | xargs -0 clang-format-14 --dry-run --Werror
```

To apply the formatting, replace `--dry-run --Werror` with `-i`.
Use a trailing comma after the last member of a multiline initializer list to
keep its closing brace on a separate line.
