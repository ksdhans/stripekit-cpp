"""Check catalog CLI help and input rejection without contacting Stripe."""

import os
import subprocess
import sys


def main():
    """Reject unsafe keys and invalid identities before any network request."""
    executable = sys.argv[1]
    environment = os.environ.copy()
    environment.pop("STRIPE_SECRET_KEY", None)
    environment.pop("STRIPEKIT_CATALOG_ID", None)
    cases = [
        (["--help"], {}, 0, "STRIPEKIT_CATALOG_ID"),
        (["--unknown"], {}, 1, "Unknown argument"),
        ([], {}, 1, "Set STRIPE_SECRET_KEY"),
        ([], {"STRIPE_SECRET_KEY": "sk_live_invalid"}, 1, "refuses live keys"),
        ([], {"STRIPE_SECRET_KEY": "pk_test_invalid"}, 1, "requires an sk_test_ key"),
        ([], {"STRIPE_SECRET_KEY": "sk_test_invalid"}, 1, "Set STRIPEKIT_CATALOG_ID"),
    ]
    for label in ("", "bad/label", "bad label", "a" * 65, "\u00e9"):
        cases.append(([], {"STRIPE_SECRET_KEY": "sk_test_invalid",
                           "STRIPEKIT_CATALOG_ID": label}, 1,
                      "Set STRIPEKIT_CATALOG_ID" if not label else
                      "STRIPEKIT_CATALOG_ID must contain"))
    for arguments, values, expected_code, message in cases:
        result = subprocess.run([executable, *arguments], env={**environment, **values},
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == expected_code, result
        output = result.stdout if expected_code == 0 else result.stderr
        assert message in output, result
        assert "sk_live_invalid" not in output and "sk_test_invalid" not in output


if __name__ == "__main__":
    main()
