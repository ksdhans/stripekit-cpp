/** StripeKit.h
 *
 * A library for interacting with the Stripe API.
 *
 * Umbrella header: including this pulls in the whole application API, and is
 * the simplest starting point. Individual headers can be included instead when
 * a translation unit only needs part of it.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/ApiConstants.h"
#include "StripeKit/Callback.h"
#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Models.h"
#include "StripeKit/RequestOptions.h"
#include "StripeKit/StripeApiClient.h"
#include "StripeKit/WebhookProcessor.h"
#include "StripeKit/Webhooks.h"

#include <string_view>

namespace stripekit {

/**
 * @addtogroup version
 * @{
 */

/**
 * Returns the StripeKit version as a "major.minor.patch" string.
 *
 * @return The version string, for example "0.1.0".
 */
std::string_view version();

/**
 * Returns the major version number.
 *
 * This changes when the API breaks compatibility with earlier releases.
 *
 * @return The major version number.
 */
int version_major();

/**
 * Returns the minor version number.
 *
 * This changes when features are added in a backwards-compatible way.
 *
 * @return The minor version number.
 */
int version_minor();

/**
 * Returns the patch version number.
 *
 * @return The patch version number.
 */
int version_patch();

/**
 * Returns the identifier of the source revision this build came from.
 *
 * Useful for pinning down exactly which build a bug report came from.
 *
 * @return The revision identifier, or an empty string when it could not be
 *         determined at build time.
 */
std::string_view build_id();

/**
 * Returns the date this build was produced.
 *
 * @return The build date in ISO 8601 form.
 */
std::string_view build_date();

/** @} */

} // namespace stripekit
