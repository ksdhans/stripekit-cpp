/** Consumer.cpp
 *
 * Checks that an installed StripeKit package can be linked and used.
 */

#include <StripeKit/Adapters/Drogon.h>
#include <StripeKit/StripeKit.h>

int main() {
    auto client = stripekit::drogon::make_stripe_client({ .api_key = "sk_test_package_check" });
    try {
        stripekit::webhooks::construct_event("{}", "invalid", "whsec_package_check");
    } catch (const stripekit::SignatureVerificationException&) {
        return stripekit::version().empty() || !client ? 1 : 0;
    }
    return 1;
}
