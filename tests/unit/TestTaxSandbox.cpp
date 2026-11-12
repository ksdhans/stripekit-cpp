/** TestTaxSandbox.cpp
 *
 * See TaxSandbox.h for details.
 */
#include "StripeKit/Testing/TaxSandbox.h"
#include "TestFramework.h"

#include <stdexcept>

using namespace stripekit::testing;

namespace {

/** Return the form value required by an expected provisioning request. */
std::string field(const TaxSandboxParams& params, std::string_view key) {
    for (const auto& [name, value] : params)
        if (name == key) return value;
    throw std::runtime_error("Missing test form field: " + std::string(key));
}

/** Create a decoded registration matching one of the baseline fixture locations. */
Json::Value registration(std::string country, std::string key, std::string status = "active") {
    Json::Value value;
    value["id"] = "txr_" + key;
    value["country"] = country;
    value["status"] = status;
    value["livemode"] = false;
    value["country_options"][key]["type"] = "standard";
    return value;
}

/** Return a decoded list page, optionally requiring another page. */
Json::Value page(std::initializer_list<Json::Value> registrations, bool has_more = false) {
    Json::Value result;
    result["data"] = Json::Value(Json::arrayValue);
    for (const auto& value : registrations)
        result["data"].append(value);
    result["has_more"] = has_more;
    return result;
}

/** In-memory REST helper that exercises provisioning without credentials or network calls. */
class FakeTaxApi {
public:
    /** Start with matching settings and baseline registrations, requiring no writes. */
    FakeTaxApi() {
        settings["livemode"] = false;
        settings["status"] = "active";
        settings["defaults"]["tax_behavior"] = fixtures.tax_behavior;
        settings["defaults"]["tax_code"] = fixtures.tax_code;
        auto& address = settings["head_office"]["address"];
        address["line1"] = fixtures.new_zealand.line1;
        address["city"] = fixtures.new_zealand.city;
        address["postal_code"] = fixtures.new_zealand.postal_code;
        address["country"] = fixtures.new_zealand.country;
    }

    /** Bind callbacks to this helper; keep it alive until provisioning returns. */
    TaxSandboxApi api() {
        return { .get = [&](std::string_view path) { return get(path); },
                 .post = [&](std::string_view path,
                             const TaxSandboxParams& params) { return post(path, params); } };
    }

    TaxTestFixtures fixtures;
    Json::Value settings;
    std::vector<Json::Value> pages{ page({ registration("NZ", "nz"), registration("GB", "gb") }) };
    std::vector<std::string> reads;
    std::vector<std::pair<std::string, TaxSandboxParams>> writes;
    std::function<Json::Value(std::string_view, const TaxSandboxParams&)> on_post;

private:
    /** Return settings or the next explicit registration page, rejecting extra requests. */
    Json::Value get(std::string_view path) {
        reads.emplace_back(path);
        if (path == "/v1/tax/settings") return settings;
        if (next_page >= pages.size()) throw std::runtime_error("Unexpected extra GET");
        return pages[next_page++];
    }

    /** Record writes and return an active fixture registration unless a test overrides it. */
    Json::Value post(std::string_view path, const TaxSandboxParams& params) {
        writes.emplace_back(path, params);
        if (on_post) return on_post(path, params);
        const auto country = field(params, "country");
        const auto key = country == "NZ" ? "nz" : "gb";
        EXPECT_EQ(field(params, "country_options[" + std::string(key) + "][type]"), "standard");
        EXPECT_EQ(field(params, "active_from"), "now");
        return registration(country, key);
    }

    std::size_t next_page = 0;
};

} // namespace

TEST(TaxSandboxTest, MatchingFixturesAndPaginatedRegistrationsNeedNoWrites) {
    FakeTaxApi api;
    api.pages = { page({ registration("NZ", "nz") }, true), page({ registration("GB", "gb") }) };
    ensure_tax_sandbox(api.api());
    ASSERT_EQ(api.reads.size(), 3U);
    EXPECT_EQ(api.reads.back(), "/v1/tax/registrations?limit=100&status=all&starting_after=txr_nz");
    EXPECT_TRUE(api.writes.empty());
}

TEST(TaxSandboxTest, MissingRegistrationsAreCreatedWithCountrySpecificOptions) {
    FakeTaxApi api;
    api.pages = { page({ registration("NZ", "nz", "expired") }) };
    ensure_tax_sandbox(api.api());
    ASSERT_EQ(api.writes.size(), 2U);
    EXPECT_EQ(api.writes[0].first, "/v1/tax/registrations");
    EXPECT_EQ(field(api.writes[0].second, "country"), "NZ");
    EXPECT_EQ(field(api.writes[1].second, "country"), "GB");
}

TEST(TaxSandboxTest, ExistingRegistrationIsReusedWhileMissingRegistrationIsCreated) {
    FakeTaxApi api;
    api.pages = { page({ registration("NZ", "nz"), registration("US", "us") }) };
    ensure_tax_sandbox(api.api());
    ASSERT_EQ(api.writes.size(), 1U);
    EXPECT_EQ(field(api.writes.front().second, "country"), "GB");
}

TEST(TaxSandboxTest, IncompleteSettingsAreConfiguredAndVerified) {
    FakeTaxApi api;
    const auto active_settings = api.settings;
    api.settings["status"] = "pending";
    api.on_post = [&](std::string_view path, const TaxSandboxParams& params) {
        EXPECT_EQ(path, "/v1/tax/settings");
        EXPECT_EQ(field(params, "defaults[tax_code]"), api.fixtures.tax_code);
        EXPECT_EQ(field(params, "defaults[tax_behavior]"), api.fixtures.tax_behavior);
        EXPECT_EQ(field(params, "head_office[address][country]"), api.fixtures.new_zealand.country);
        EXPECT_EQ(field(params, "head_office[address][postal_code]"),
                  api.fixtures.new_zealand.postal_code);
        return active_settings;
    };
    ensure_tax_sandbox(api.api());
    EXPECT_EQ(api.writes.size(), 1U);
}

TEST(TaxSandboxTest, LiveOrMissingModeSettingsAreRejectedBeforeAnyWrite) {
    for (const auto& mode : { Json::Value(true), Json::Value(), Json::Value("false") }) {
        FakeTaxApi api;
        api.settings["livemode"] = mode;
        EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
        EXPECT_TRUE(api.writes.empty());
        EXPECT_EQ(api.reads.size(), 1U);
    }
}

TEST(TaxSandboxTest, UpdatedSettingsMustBeActiveMatchingAndTestMode) {
    for (const auto* invalid : { "status", "livemode", "defaults" }) {
        FakeTaxApi api;
        auto updated = api.settings;
        updated.removeMember(invalid);
        api.settings["status"] = "pending";
        api.on_post = [&](std::string_view, const TaxSandboxParams&) { return updated; };
        EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
        EXPECT_EQ(api.reads.size(), 1U);
    }
}

TEST(TaxSandboxTest, ConflictingRegistrationsAreRejectedWithoutWrites) {
    for (const auto& conflict : { registration("AU", "au"),
                                  registration("AU", "au", "scheduled"),
                                  registration("NZ", "nz", "scheduled"),
                                  registration("GB", "gb", "scheduled") }) {
        FakeTaxApi api;
        api.pages = { page({ conflict }) };
        EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
        EXPECT_TRUE(api.writes.empty());
    }
    FakeTaxApi api;
    auto incompatible = registration("NZ", "nz");
    incompatible["country_options"]["nz"]["type"] = "unsupported";
    api.pages = { page({ incompatible }) };
    EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
    EXPECT_TRUE(api.writes.empty());
}

TEST(TaxSandboxTest, MalformedOrLiveRegistrationsAndInvalidPaginationAreRejected) {
    auto live = registration("NZ", "nz");
    live["livemode"] = true;
    auto missing_mode = registration("NZ", "nz");
    missing_mode.removeMember("livemode");
    for (const auto& invalid : { Json::Value(),
                                 Json::Value("malformed"),
                                 page({ Json::Value() }),
                                 page({ live }),
                                 page({ missing_mode }),
                                 page({}, true) }) {
        FakeTaxApi api;
        api.pages = { invalid };
        EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
        EXPECT_TRUE(api.writes.empty());
    }
    FakeTaxApi malformed_options;
    auto invalid_options = registration("NZ", "nz");
    invalid_options["country_options"] = "malformed";
    malformed_options.pages = { page({ invalid_options }) };
    EXPECT_THROW(ensure_tax_sandbox(malformed_options.api()), std::runtime_error);
    EXPECT_TRUE(malformed_options.writes.empty());
    FakeTaxApi api;
    api.pages = { page({ registration("NZ", "nz") }, true),
                  page({ registration("NZ", "nz") }, true) };
    EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
    EXPECT_EQ(api.reads.size(), 3U);
}

TEST(TaxSandboxTest, NewlyCreatedRegistrationMustMatchTheFixtureAndTestMode) {
    for (const auto* invalid : { "country", "status", "livemode", "country_options" }) {
        FakeTaxApi api;
        api.pages = { page({}) };
        api.on_post = [&](std::string_view, const TaxSandboxParams&) {
            auto result = registration("NZ", "nz");
            result.removeMember(invalid);
            return result;
        };
        EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
        EXPECT_EQ(api.writes.size(), 1U);
    }
}

TEST(TaxSandboxTest, MissingCallbacksAndTransportFailuresAreExplicit) {
    EXPECT_THROW(ensure_tax_sandbox({}), std::runtime_error);
    FakeTaxApi api;
    auto callbacks = api.api();
    callbacks.get = [](std::string_view) -> Json::Value {
        throw std::runtime_error("Simulated HTTP failure");
    };
    EXPECT_THROW(ensure_tax_sandbox(callbacks), std::runtime_error);
    EXPECT_TRUE(api.writes.empty());
    api.pages = { page({}) };
    api.on_post = [](std::string_view, const TaxSandboxParams&) -> Json::Value {
        throw std::runtime_error("Simulated HTTP failure");
    };
    EXPECT_THROW(ensure_tax_sandbox(api.api()), std::runtime_error);
}

TEST(TaxSandboxTest, CustomerProfilesAndAddressEncodingShareTheFixtureContract) {
    const TaxTestFixtures fixtures;
    const auto profiles = fixtures.profiles();
    EXPECT_TRUE(profiles[0].expected_tax);
    EXPECT_TRUE(profiles[1].expected_tax);
    EXPECT_FALSE(profiles[2].expected_tax);
    EXPECT_TRUE(profiles[2].add_uk_vat_id);
    EXPECT_FALSE(profiles[3].expected_tax);
    EXPECT_EQ(profiles[3].address.country, fixtures.australia.country);
    TaxSandboxParams params;
    append_address(params, profiles[3].address, "shipping[address]");
    EXPECT_EQ(field(params, "shipping[address][country]"), fixtures.australia.country);
    EXPECT_EQ(field(params, "shipping[address][state]"), fixtures.australia.state);
}
