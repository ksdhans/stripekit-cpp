/** StripeKit.h
 * 
 * A library for interacting with the Stripe API.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/Callback.h"
#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Models.h"
#include "StripeKit/RequestOptions.h"
#include "StripeKit/StripeApiClient.h"
#include "StripeKit/WebhookProcessor.h"

#include <string_view>

namespace stripekit {

// Get StripeKit version information.

std::string_view version();
int version_major();
int version_minor();
int version_patch();
std::string_view build_id();
std::string_view build_date();

} // namespace stripekit
