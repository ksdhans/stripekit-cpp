/** StripeKit.cpp
 *
 * See header file for details.
 */

#include "StripeKit/StripeKit.h"

#include "stripekit_rev.h"

namespace stripekit {

std::string_view version() {
    return STRIPEKIT_VERSION;
}

int version_major() {
    return STRIPEKIT_VERSION_MAJOR;
}

int version_minor() {
    return STRIPEKIT_VERSION_MINOR;
}

int version_patch() {
    return STRIPEKIT_VERSION_PATCH;
}

std::string_view build_id() {
    return STRIPEKIT_BUILD_ID;
}

std::string_view build_date() {
    return STRIPEKIT_BUILD_DATE;
}

} // namespace stripekit
