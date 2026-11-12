/** TaxSandbox.cpp
 *
 * See header file for details.
 */
#include "StripeKit/Testing/TaxSandbox.h"

#include <stdexcept>

namespace stripekit::testing {

std::array<TaxTestProfile, 4> TaxTestFixtures::profiles() const {
    return { TaxTestProfile{ "nz", new_zealand, "nzd", true, false },
             TaxTestProfile{ "uk_no_vat_id", united_kingdom, "gbp", true, false },
             TaxTestProfile{ "uk_with_vat_id", united_kingdom, "gbp", false, true },
             TaxTestProfile{ "au_unregistered", australia, "aud", false, false } };
}

void append_address(TaxSandboxParams& params,
                    const TaxTestAddress& address,
                    std::string_view prefix) {
    const std::string key(prefix);
    params.emplace_back(key + "[line1]", address.line1);
    params.emplace_back(key + "[city]", address.city);
    if (!address.state.empty()) params.emplace_back(key + "[state]", address.state);
    params.emplace_back(key + "[postal_code]", address.postal_code);
    params.emplace_back(key + "[country]", address.country);
}

namespace {

/** Reject missing mode information and live-account objects before provisioning. */
void require_test_mode(const Json::Value& object) {
    if (!object.isObject() || !object["livemode"].isBool())
        throw std::runtime_error(
            "Stripe Tax response did not identify whether the account is in live mode");
    if (object["livemode"].asBool())
        throw std::runtime_error(
            "Refusing to run tax tests: the API key is associated with a live account, not a "
            "test sandbox");
}

/** Configure the fictional business and confirm Stripe Tax is ready in test mode. */
void ensure_tax_settings(const TaxSandboxApi& api, const TaxTestFixtures& fixtures) {
    const auto matches_fixtures = [&](const Json::Value& settings) {
        return settings["status"] == "active" && settings["defaults"].isObject() &&
               settings["head_office"].isObject() &&
               settings["head_office"]["address"].isObject() &&
               settings["defaults"]["tax_behavior"] == fixtures.tax_behavior &&
               settings["defaults"]["tax_code"] == fixtures.tax_code &&
               settings["head_office"]["address"]["line1"] == fixtures.new_zealand.line1 &&
               settings["head_office"]["address"]["city"] == fixtures.new_zealand.city &&
               settings["head_office"]["address"]["postal_code"] ==
                   fixtures.new_zealand.postal_code &&
               settings["head_office"]["address"]["country"] == fixtures.new_zealand.country;
    };
    auto settings = api.get("/v1/tax/settings");
    require_test_mode(settings);
    if (!matches_fixtures(settings)) {
        TaxSandboxParams params{ { "defaults[tax_behavior]", fixtures.tax_behavior },
                                 { "defaults[tax_code]", fixtures.tax_code } };
        append_address(params, fixtures.new_zealand, "head_office[address]");
        settings = api.post("/v1/tax/settings", params);
        require_test_mode(settings);
    }
    if (!matches_fixtures(settings))
        throw std::runtime_error(
            "Stripe Tax settings could not be activated with the sandbox test fixtures");
}

/** Check country-specific registration options without accepting missing or malformed fields. */
bool matches_registration_type(const Json::Value& registration,
                               const TaxTestRegistration& expected) {
    const auto& options = registration["country_options"];
    return options.isObject() && options[expected.country_key].isObject() &&
           options[expected.country_key]["type"] == expected.type;
}

/** Reuse active baseline registrations and reject conflicts in required and control locations. */
void ensure_tax_registrations(const TaxSandboxApi& api, const TaxTestFixtures& fixtures) {
    std::array<bool, 2> active{};
    std::string starting_after;
    do {
        std::string path = "/v1/tax/registrations?limit=100&status=all";
        if (!starting_after.empty()) path += "&starting_after=" + starting_after;
        const auto response = api.get(path);
        if (!response.isObject())
            throw std::runtime_error("Stripe returned a malformed tax registration list");
        const auto& data = response["data"];
        const auto& has_more = response["has_more"];
        if (!data.isArray() || !has_more.isBool())
            throw std::runtime_error("Stripe returned a malformed tax registration list");
        for (const auto& registration : data) {
            if (!registration.isObject() || !registration["country"].isString() ||
                !registration["status"].isString())
                throw std::runtime_error("Stripe returned a malformed tax registration");
            const auto country = registration["country"].asString();
            const auto status = registration["status"].asString();
            if (status != "active" && status != "scheduled") continue;
            require_test_mode(registration);
            if (country == fixtures.australia.country)
                throw std::runtime_error(
                    "The no-tax control location has an active or scheduled registration; "
                    "expire it in this sandbox before running the tax tests");
            for (std::size_t i = 0; i < fixtures.registrations.size(); ++i) {
                const auto& expected = fixtures.registrations[i];
                if (country != expected.country) continue;
                if (status == "scheduled")
                    throw std::runtime_error(
                        "A required tax location has a scheduled registration; resolve it in "
                        "this sandbox before running the tax tests");
                if (!matches_registration_type(registration, expected))
                    throw std::runtime_error(
                        "A required tax location has an incompatible registration type; "
                        "resolve it in this sandbox before running the tax tests");
                active[i] = true;
            }
        }
        if (!has_more.asBool()) break;
        if (data.empty() || !data[data.size() - 1]["id"].isString() ||
            data[data.size() - 1]["id"].asString().empty() ||
            data[data.size() - 1]["id"].asString() == starting_after)
            throw std::runtime_error(
                "Stripe returned an invalid cursor while listing tax registrations");
        starting_after = data[data.size() - 1]["id"].asString();
    } while (true);

    for (std::size_t i = 0; i < fixtures.registrations.size(); ++i) {
        if (active[i]) continue;
        const auto& expected = fixtures.registrations[i];
        const auto registration =
            api.post("/v1/tax/registrations",
                     { { "country", expected.country },
                       { "country_options[" + expected.country_key + "][type]", expected.type },
                       { "active_from", "now" } });
        require_test_mode(registration);
        if (registration["country"] != expected.country || registration["status"] != "active" ||
            !matches_registration_type(registration, expected))
            throw std::runtime_error(
                "Stripe could not create the required active sandbox tax registration");
    }
}

} // namespace

void ensure_tax_sandbox(const TaxSandboxApi& api, const TaxTestFixtures& fixtures) {
    if (!api.get || !api.post)
        throw std::runtime_error("Stripe Tax sandbox setup requires GET and POST callbacks");
    ensure_tax_settings(api, fixtures);
    ensure_tax_registrations(api, fixtures);
}

} // namespace stripekit::testing
