#!/usr/bin/env bash
#
# format.sh
#
# Checks or fixes C++ formatting using the same formatter as CI.
# @author StripeKit contributors
# @License See LICENSE

set -euo pipefail

case "${1:---check}" in
    --check)
        options=(--dry-run --Werror)
        ;;
    --fix)
        options=(-i)
        ;;
    --help|-h)
        printf 'Usage: %s [--check|--fix]\n\nCheck C++ formatting (default), or fix files in place with --fix.\n' "$0"
        exit 0
        ;;
    *)
        printf 'Unknown option: %s\nUse --help for usage.\n' "$1" >&2
        exit 2
        ;;
esac

if (( $# > 1 )); then
    printf 'Expected at most one option. Use --help for usage.\n' >&2
    exit 2
fi

if ! command -v clang-format-21 >/dev/null 2>&1; then
    printf 'clang-format-21 is required. On Ubuntu, install it with: sudo apt install clang-format-21\n' >&2
    exit 1
fi

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
find include src tests examples -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 |
    xargs -0 -r clang-format-21 "${options[@]}"
