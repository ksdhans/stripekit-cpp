/** TaxSandbox.h
 *
 * Shared fictional fixtures and Stripe Tax provisioning for opt-in sandbox tests.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */
#pragma once

#include <json/json.h>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stripekit::testing {

/** A fictional customer address used to exercise Stripe Tax in the sandbox. */
struct TaxTestAddress {
    std::string line1;
    std::string city;
    std::string state;
    std::string postal_code;
    std::string country;
};

/** One customer profile and its expected positive or zero-tax result, not a fixed rate. */
struct TaxTestProfile {
    std::string_view label;
    TaxTestAddress address;
    std::string_view currency;
    bool expected_tax;
    bool add_uk_vat_id;
};

/** Country-specific options for a required baseline tax registration. */
struct TaxTestRegistration {
    std::string country;
    std::string country_key;
    std::string type;
};

/** Fictional fixture contract shared by StripeKit and application sandbox tests. */
struct TaxTestFixtures {
    std::string tax_code = "txcd_10103001";
    std::string tax_behavior = "exclusive";
    std::string uk_vat_id = "GB123456789";
    TaxTestAddress new_zealand{
        .line1 = "1 Example Street", .city = "Auckland", .postal_code = "1010", .country = "NZ"
    };
    TaxTestAddress united_kingdom{
        .line1 = "1 Example Street", .city = "London", .postal_code = "SW1A 1AA", .country = "GB"
    };
    TaxTestAddress australia{ .line1 = "1 Example Street",
                              .city = "Sydney",
                              .state = "NSW",
                              .postal_code = "2000",
                              .country = "AU" };
    std::array<TaxTestRegistration, 2> registrations{
        TaxTestRegistration{ "NZ", "nz", "standard" }, TaxTestRegistration{ "GB", "gb", "standard" }
    };

    /** Return the shared taxable, VAT-ID, and unregistered-location customer profiles. */
    std::array<TaxTestProfile, 4> profiles() const;
};

/** Key/value pairs sent as an application/x-www-form-urlencoded request body. */
using TaxSandboxParams = std::vector<std::pair<std::string, std::string>>;

/**
 * Adapts a suite's existing Stripe REST helper without owning credentials or a server.
 * Both callbacks must return decoded successful responses and throw on HTTP or transport errors.
 */
struct TaxSandboxApi {
    std::function<Json::Value(std::string_view)> get;
    std::function<Json::Value(std::string_view, const TaxSandboxParams&)> post;
};

/** Add fictional address fields to a form request under the supplied parameter prefix. */
void append_address(TaxSandboxParams& params,
                    const TaxTestAddress& address,
                    std::string_view prefix = "address");

/**
 * Configure test-mode tax settings and reuse or create the required baseline registrations.
 * Call only after the suite's sandbox opt-in and credential checks. Callers must serialize
 * suites sharing an account. Settings and registrations persist after the tests.
 * HTTP and transport failures from the callbacks propagate to the caller.
 *
 * @param api Suite-owned REST callbacks; no webhook server or test runner is required.
 * @param fixtures Shared fictional business and customer details.
 *
 * @throws std::runtime_error If responses are malformed, live-mode, or conflict with the fixtures.
 */
void ensure_tax_sandbox(const TaxSandboxApi& api, const TaxTestFixtures& fixtures = {});

} // namespace stripekit::testing
