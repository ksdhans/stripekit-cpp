/** ApiConstants.h
 *
 * Shared defaults for Stripe API connections.
 *
 * @author StripeKit contributors
 * @License See LICENSE
 */

#pragma once

namespace stripekit {

/** Default Stripe API base URL. Test transports may use a loopback server instead. */
inline constexpr char default_api_base_url[] = "https://api.stripe.com";

} // namespace stripekit
