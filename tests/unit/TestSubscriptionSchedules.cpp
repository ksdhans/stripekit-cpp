/** TestSubscriptionSchedules.cpp
 *
 * Tests for subscription schedules and repricing safety fields.
 */
#include "TestFramework.h"
#include "TestHelpers.h"
#include "StripeKit/StripeApiClient.h"
#include <json/json.h>

using namespace stripekit;
using namespace stripekit::testing;

namespace {
/** Return a realistic two-phase schedule, including expanded references. */
Json::Value schedule_json() {
    Json::Value value;
    value["object"] = "subscription_schedule";
    value["id"] = "sub_sched";
    value["customer"]["id"] = "cus_test";
    value["subscription"] = "sub_test";
    value["status"] = "active";
    value["end_behavior"] = "release";
    value["current_phase"]["start_date"] = 100;
    value["current_phase"]["end_date"] = 200;
    value["metadata"]["operation"] = "saved";
    for (int i = 0; i < 2; ++i) {
        auto& phase = value["phases"][i];
        phase["start_date"] = 100 + i * 100;
        phase["end_date"] = 200 + i * 100;
        phase["proration_behavior"] = "none";
        phase["metadata"]["label"] = "phase";
        phase["items"][0]["price"]["id"] = i == 0 ? "price_old" : "price_new";
        phase["items"][0]["quantity"] = 3;
    }
    return value;
}

/** Serialize a fixture as a successful response. */
HttpResponse response(const Json::Value& value) {
    return { .status_code = 200, .body = Json::writeString(Json::StreamWriterBuilder{}, value) };
}

/** Return a valid update using both explicit dates and a phase duration. */
UpdateSubscriptionScheduleRequest update_request() {
    return { .phases = { { .start_date = 100,
                           .end_date = 200,
                           .items = { { "price/old", 3 }, { "price_extra", {} } },
                           .metadata = { { "label &", "first?" } } },
                         { .start_date = 200,
                           .duration = SubscriptionSchedulePhaseDuration{ "month", 1 },
                           .items = { { "price_new", 3 } } } },
             .metadata = { { "operation", "saved" } } };
}
} // namespace

class SubscriptionSchedulesTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(SubscriptionSchedulesTest, PauseSchedulesSupportCreationAndKeyedPartialUpdates) {
    http.set_response(response(schedule_json()));
    SubscriptionSchedulePauseScheduleRequest entry{
        .pause =
            SubscriptionSchedulePauseRequest{
                .pause_at = SubscriptionPauseTime{ .type = "now" },
                .settings =
                    SubscriptionPauseSettings{
                        .bill_for = { .unused_time_from = { "pause_at" },
                                      .outstanding_usage_through = { "none" } },
                        .invoicing_behavior = "invoice",
                        .type = "subscription" } },
        .resume =
            SubscriptionScheduleResumeRequest{
                .resume_at =
                    SubscriptionPauseTime{ .type = "duration",
                                           .duration = SubscriptionPauseDuration{ "month", 1 } },
                .settings = SubscriptionScheduleResumeSettings{ .billing_cycle_anchor = "resume_at",
                                                                .payment_behavior =
                                                                    "resume_on_payment_success",
                                                                .proration_behavior = "none" } }
    };
    auto& api = client.subscription_schedules();
    api.create_async({ .from_subscription = "sub/a?", .pause_schedules = { entry } }).result();
    EXPECT_EQ(
        http.get_recorded_requests().back().body,
        "from_subscription=sub%2Fa%3F"
        "&pause_schedules%5B0%5D%5Bpause%5D%5Bpause_at%5D%5Btype%5D=now"
        "&pause_schedules%5B0%5D%5Bpause%5D%5Bsettings%5D%5Bbill_for%5D%5Bunused_time_from%5D%"
        "5Btype%5D=pause_at"
        "&pause_schedules%5B0%5D%5Bpause%5D%5Bsettings%5D%5Bbill_for%5D%5Boutstanding_usage_"
        "through%5D%5Btype%5D=none"
        "&pause_schedules%5B0%5D%5Bpause%5D%5Bsettings%5D%5Binvoicing_behavior%5D=invoice"
        "&pause_schedules%5B0%5D%5Bpause%5D%5Bsettings%5D%5Btype%5D=subscription"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bresume_at%5D%5Btype%5D=duration"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bresume_at%5D%5Bduration%5D%5Binterval%5D=month"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bresume_at%5D%5Bduration%5D%5Binterval_count%5D=1"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bsettings%5D%5Bbilling_cycle_anchor%5D=resume_at"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bsettings%5D%5Bpayment_behavior%5D=resume_on_payment_"
        "success"
        "&pause_schedules%5B0%5D%5Bresume%5D%5Bsettings%5D%5Bproration_behavior%5D=none");
    entry = { .key = "pause/key?",
              .resume = SubscriptionScheduleResumeRequest{
                  .resume_at = SubscriptionPauseTime{ .type = "now" } } };
    UpdateSubscriptionScheduleRequest update;
    update.pause_schedules = std::vector{ entry };
    const RequestOptions options{ .idempotency_key = "edit-pause",
                                  .timeout = std::chrono::seconds(7),
                                  .headers = { { "Stripe-Version", default_stripe_api_version } } };
    int calls = 0;
    api.update("sched/a?", update, options, [&](CallbackResult<SubscriptionSchedule> value) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<SubscriptionSchedule>(value));
    });
    EXPECT_EQ(calls, 1);
    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path, "/v1/subscription_schedules/sched%2Fa%3F");
    EXPECT_EQ(sent.body,
              "pause_schedules%5B0%5D%5Bkey%5D=pause%2Fkey%3F"
              "&pause_schedules%5B0%5D%5Bresume%5D%5Bresume_at%5D%5Btype%5D=now");
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "edit-pause");
    EXPECT_EQ(sent.headers.at("Stripe-Version"), default_stripe_api_version);
    EXPECT_EQ(sent.options.timeout, options.timeout);
    update.pause_schedules =
        std::vector<SubscriptionSchedulePauseScheduleRequest>{ { .key = "existing",
                                                                 .clear_resume = true } };
    api.update_async("sched", update).result();
    EXPECT_EQ(http.get_recorded_requests().back().body,
              "pause_schedules%5B0%5D%5Bkey%5D=existing&pause_schedules%5B0%5D%5Bresume%5D=");
    update.pause_schedules->clear();
    api.update_async("sched", update).result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "pause_schedules=");
}

TEST_F(SubscriptionSchedulesTest,
       PauseSchedulesAllowFuturePauseAndResumeOnlyForPausedSubscriptions) {
    http.set_response(response(schedule_json()));
    SubscriptionSchedulePauseScheduleRequest entry{
        .pause =
            SubscriptionSchedulePauseRequest{
                .pause_at = SubscriptionPauseTime{ .type = "timestamp", .timestamp = 1900000000 } },
        .resume =
            SubscriptionScheduleResumeRequest{
                .resume_at = SubscriptionPauseTime{ .type = "timestamp", .timestamp = 1901000000 } }
    };
    UpdateSubscriptionScheduleRequest update;
    update.pause_schedules = std::vector{ entry };
    client.subscription_schedules().update_async("sched", update).result();
    const auto both = http.get_recorded_requests().back().body;
    EXPECT_NE(both.find("pause_at%5D%5Btimestamp%5D=1900000000"), std::string::npos);
    EXPECT_NE(both.find("resume_at%5D%5Btimestamp%5D=1901000000"), std::string::npos);
    entry.pause.reset();
    client.subscription_schedules().create_async({ "sub_paused", { entry } }).result();
    const auto resume_only = http.get_recorded_requests().back().body;
    EXPECT_EQ(resume_only.find("%5Bpause%5D"), std::string::npos);
    EXPECT_NE(resume_only.find("resume_at%5D%5Btimestamp%5D=1901000000"), std::string::npos);
    auto phases = update_request();
    phases.default_collection_method = "charge_automatically";
    phases.phases[0].collection_method = "charge_automatically";
    phases.pause_schedules = std::vector{ entry };
    client.subscription_schedules().update_async("sched", phases).result();
    const auto body = http.get_recorded_requests().back().body;
    EXPECT_NE(body.find("default_settings%5Bcollection_method%5D=charge_automatically"),
              std::string::npos);
    EXPECT_NE(body.find("phases%5B0%5D%5Bcollection_method%5D=charge_automatically"),
              std::string::npos);
}

TEST_F(SubscriptionSchedulesTest, PauseSchedulesRejectAmbiguousTimesAndConflictingChanges) {
    int calls = 0;
    for (int invalid = 0; invalid < 16; ++invalid) {
        SubscriptionSchedulePauseScheduleRequest entry{
            .pause =
                SubscriptionSchedulePauseRequest{
                    .pause_at =
                        SubscriptionPauseTime{ .type = "timestamp", .timestamp = 1900000000 } },
            .resume =
                SubscriptionScheduleResumeRequest{
                    .resume_at =
                        SubscriptionPauseTime{ .type = "timestamp", .timestamp = 1901000000 } }
        };
        switch (invalid) {
        case 0:
            entry.pause->pause_at->timestamp.reset();
            break;
        case 1:
            entry.pause->pause_at->timestamp = 0;
            break;
        case 2:
            entry.pause->pause_at->type = "now";
            break;
        case 3:
            entry.pause->pause_at->type = "invalid";
            break;
        case 4:
            entry.pause->pause_at =
                SubscriptionPauseTime{ .type = "duration",
                                       .duration = SubscriptionPauseDuration{ "month", 1 } };
            break;
        case 5:
            entry.resume->resume_at->timestamp = 1900000000;
            break;
        case 6:
            entry.resume->resume_at =
                SubscriptionPauseTime{ .type = "duration",
                                       .duration = SubscriptionPauseDuration{ "hour", 1 } };
            break;
        case 7:
            entry.resume->resume_at =
                SubscriptionPauseTime{ .type = "duration",
                                       .duration = SubscriptionPauseDuration{ "month", 0 } };
            break;
        case 8:
            entry.key = std::string(41, 'a');
            break;
        case 9:
            entry.clear_resume = true;
            break;
        case 10:
            entry = {};
            break;
        case 11:
            entry.pause->settings =
                SubscriptionPauseSettings{ .bill_for = { .unused_time_from = { "now" } } };
            break;
        case 12:
            entry.resume->settings =
                SubscriptionScheduleResumeSettings{ .billing_cycle_anchor = "now" };
            break;
        case 13:
            entry.resume->settings =
                SubscriptionScheduleResumeSettings{ .payment_behavior = "allow_incomplete" };
            break;
        case 14:
            entry.resume->settings =
                SubscriptionScheduleResumeSettings{ .proration_behavior = "invalid" };
            break;
        case 15:
            entry = { .clear_resume = true };
            break;
        }
        UpdateSubscriptionScheduleRequest request;
        request.pause_schedules = std::vector{ entry };
        EXPECT_THROW(client.subscription_schedules().update_async("sched", request).result(),
                     ConfigurationException);
        client.subscription_schedules().update(
            "sched", request, {}, [&](CallbackResult<SubscriptionSchedule> value) {
                ++calls;
                EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(value));
            });
        EXPECT_THROW(client.subscription_schedules().create_async({ "sub", { entry } }).result(),
                     ConfigurationException);
    }
    UpdateSubscriptionScheduleRequest too_many;
    too_many.pause_schedules = std::vector<SubscriptionSchedulePauseScheduleRequest>(2);
    EXPECT_THROW(client.subscription_schedules().update_async("sched", too_many).result(),
                 ConfigurationException);
    EXPECT_EQ(calls, 16);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(SubscriptionSchedulesTest, PauseScheduleResponsesExposeSettingsActionStatusesAndErrors) {
    auto value = schedule_json();
    value["billing_mode"]["type"] = "flexible";
    value["default_settings"]["collection_method"] = "charge_automatically";
    value["phases"][0]["collection_method"] = "charge_automatically";
    auto& entry = value["pause_schedules"][0];
    entry["key"] = "saved-key";
    entry["pause"]["pause_at"] = 1900000000;
    entry["pause"]["settings"]["bill_for"]["unused_time_from"]["type"] = "pause_at";
    entry["pause"]["settings"]["invoicing_behavior"] = "invoice";
    entry["pause"]["status"]["type"] = "succeeded";
    entry["resume"]["resume_at"] = 1901000000;
    entry["resume"]["settings"]["billing_cycle_anchor"] = "resume_at";
    entry["resume"]["settings"]["payment_behavior"] = "resume_on_payment_success";
    entry["resume"]["settings"]["proration_behavior"] = "create_prorations";
    JsonStripeResponseParser parser;
    for (const auto* status :
         { "scheduled", "pending", "requires_action", "succeeded", "error", "future" }) {
        entry["resume"]["status"]["type"] = status;
        entry["resume"]["status"]["error"]["code"] = "resume_failed";
        entry["resume"]["status"]["error"]["message"] = "No payment method";
        const auto parsed = parser.parse_subscription_schedule(response(value));
        EXPECT_EQ(parsed.billing_mode->type, "flexible");
        EXPECT_EQ(parsed.default_collection_method, "charge_automatically");
        EXPECT_EQ(parsed.phases[0].collection_method, "charge_automatically");
        const auto& saved = parsed.pause_schedules.at(0);
        EXPECT_EQ(saved.key, "saved-key");
        EXPECT_EQ(saved.pause->pause_at_unix, 1900000000);
        EXPECT_EQ(saved.pause->settings->bill_for.unused_time_from.type, "pause_at");
        EXPECT_EQ(saved.resume->resume_at_unix, 1901000000);
        EXPECT_EQ(saved.resume->settings->payment_behavior, "resume_on_payment_success");
        EXPECT_EQ(saved.resume->status.type, status);
        EXPECT_EQ(saved.resume->status.error->code, "resume_failed");
        EXPECT_EQ(saved.resume->status.error->message, "No payment method");
    }
    const auto original = value;
    for (int invalid = 0; invalid < 8; ++invalid) {
        value = original;
        auto& changed = value["pause_schedules"][0];
        switch (invalid) {
        case 0:
            value["pause_schedules"] = false;
            break;
        case 1:
            changed["key"] = "";
            break;
        case 2:
            changed["pause"]["pause_at"] = "1900000000";
            break;
        case 3:
            changed["resume"]["resume_at"] = Json::nullValue;
            break;
        case 4:
            changed["resume"]["status"] = false;
            break;
        case 5:
            changed["pause"]["settings"]["bill_for"] = false;
            break;
        case 6:
            changed["resume"]["settings"]["payment_behavior"] = false;
            break;
        case 7:
            changed["resume"]["status"]["error"]["message"] = false;
            break;
        }
        EXPECT_THROW(parser.parse_subscription_schedule(response(value)), ConfigurationException);
    }
}

TEST_F(SubscriptionSchedulesTest, RequestsEncodeDatesQuantitiesMetadataAndOptionsInBothForms) {
    http.set_response(response(schedule_json()));
    const RequestOptions options{ .idempotency_key = "saved",
                                  .timeout = std::chrono::seconds(7),
                                  .headers = { { "Stripe-Version", default_stripe_api_version } } };
    auto& api = client.subscription_schedules();
    const auto schedule = api.create_async({ "sub/a?" }, options).result();
    EXPECT_EQ(schedule.id, "sub_sched");
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscription_schedules");
    EXPECT_EQ(http.get_recorded_requests().back().body, "from_subscription=sub%2Fa%3F");
    api.retrieve_async("sched/a?", options).result();
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscription_schedules/sched%2Fa%3F");
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Get);
    EXPECT_TRUE(http.get_recorded_requests().back().body.empty());
    auto request = update_request();
    api.update_async("sched/a?", request, options).result();
    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path, "/v1/subscription_schedules/sched%2Fa%3F");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.body,
              "end_behavior=release&proration_behavior=none&metadata%5Boperation%5D=saved"
              "&phases%5B0%5D%5Bstart_date%5D=100&phases%5B0%5D%5Bend_date%5D=200"
              "&phases%5B0%5D%5Bproration_behavior%5D=none&phases%5B0%5D%5Bmetadata%5D%5Blabel%20%"
              "26%5D=first%3F"
              "&phases%5B0%5D%5Bitems%5D%5B0%5D%5Bprice%5D=price%2Fold&phases%5B0%5D%5Bitems%5D%"
              "5B0%5D%5Bquantity%5D=3"
              "&phases%5B0%5D%5Bitems%5D%5B1%5D%5Bprice%5D=price_extra"
              "&phases%5B1%5D%5Bstart_date%5D=200&phases%5B1%5D%5Bduration%5D%5Binterval%5D=month"
              "&phases%5B1%5D%5Bduration%5D%5Binterval_count%5D=1"
              "&phases%5B1%5D%5Bproration_behavior%5D=none&phases%5B1%5D%5Bitems%5D%5B0%5D%5Bprice%"
              "5D=price_new"
              "&phases%5B1%5D%5Bitems%5D%5B0%5D%5Bquantity%5D=3");
    int calls = 0;
    const auto callback = [&](CallbackResult<SubscriptionSchedule> value) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<SubscriptionSchedule>(value));
    };
    api.create({ "sub_test" }, options, callback);
    api.retrieve("sub_sched", options, callback);
    request.end_behavior = SubscriptionScheduleEndBehavior::Cancel;
    request.proration_behavior = ProrationBehavior::AlwaysInvoice;
    request.phases[0].proration_behavior = ProrationBehavior::CreateProrations;
    api.update("sub_sched", request, options, callback);
    EXPECT_EQ(calls, 3);
    EXPECT_TRUE(http.get_recorded_requests().back().body.starts_with(
        "end_behavior=cancel&proration_behavior=always_invoice"));
    EXPECT_TRUE(http.get_recorded_requests().back().body.find(
                    "proration_behavior%5D=create_prorations") != std::string::npos);
    for (const auto& request : http.get_recorded_requests()) {
        EXPECT_EQ(request.options.timeout, options.timeout);
        EXPECT_EQ(request.options.idempotency_key, options.idempotency_key);
        EXPECT_EQ(request.headers.at("Stripe-Version"), default_stripe_api_version);
        if (request.method == HttpMethod::Post)
            EXPECT_EQ(request.headers.at("Idempotency-Key"), "saved");
    }
}

TEST_F(SubscriptionSchedulesTest, ScheduleUpdatesPreserveAutomaticTaxAndReusableDiscounts) {
    http.set_response(response(schedule_json()));
    auto request = update_request();
    request.default_automatic_tax_enabled = true;
    request.phases[0].automatic_tax_enabled = true;
    request.phases[0].discounts = { { .coupon_id = "coupon_phase" } };
    request.phases[0].items[0].discounts = { { .coupon_id = "coupon_item" } };
    client.subscription_schedules().update_async("sched_tax", request).result();
    const auto body = http.get_recorded_requests().back().body;
    EXPECT_NE(body.find("default_settings%5Bautomatic_tax%5D%5Benabled%5D=true"),
              std::string::npos);
    EXPECT_NE(body.find("phases%5B0%5D%5Bautomatic_tax%5D%5Benabled%5D=true"), std::string::npos);
    EXPECT_NE(body.find("phases%5B0%5D%5Bdiscounts%5D%5B0%5D%5Bcoupon%5D=coupon_phase"),
              std::string::npos);
    EXPECT_NE(body.find("phases%5B0%5D%5Bitems%5D%5B0%5D%5Bdiscounts%5D%5B0%5D%5Bcoupon%5D="
                        "coupon_item"),
              std::string::npos);

    auto fixture = schedule_json();
    fixture["default_settings"]["automatic_tax"]["enabled"] = true;
    fixture["default_settings"]["automatic_tax"]["disabled_reason"] = "requires_location_inputs";
    fixture["phases"][0]["automatic_tax"]["enabled"] = true;
    fixture["phases"][0]["discounts"][0]["coupon"]["id"] = "coupon_phase";
    fixture["phases"][0]["items"][0]["discounts"][0]["promotion_code"] = "promo_item";
    const auto parsed = JsonStripeResponseParser{}.parse_subscription_schedule(response(fixture));
    ASSERT_TRUE(parsed.default_automatic_tax.has_value());
    EXPECT_EQ(parsed.default_automatic_tax->disabled_reason, "requires_location_inputs");
    ASSERT_TRUE(parsed.phases[0].automatic_tax.has_value());
    EXPECT_TRUE(parsed.phases[0].automatic_tax->enabled);
    ASSERT_EQ(parsed.phases[0].discounts.size(), 1U);
    EXPECT_EQ(parsed.phases[0].discounts[0].coupon_id, "coupon_phase");
    ASSERT_EQ(parsed.phases[0].items[0].discounts.size(), 1U);
    EXPECT_EQ(parsed.phases[0].items[0].discounts[0].promotion_code_id, "promo_item");
}

TEST_F(SubscriptionSchedulesTest, ValidationRejectsAmbiguousOrInvalidUpdatesWithoutSending) {
    auto& api = client.subscription_schedules();
    EXPECT_THROW(api.retrieve_async("").result(), ConfigurationException);
    EXPECT_THROW(api.create_async({}).result(), ConfigurationException);
    EXPECT_THROW(api.update_async("", update_request()).result(), ConfigurationException);
    int calls = 0;
    const auto failure = [&](CallbackResult<SubscriptionSchedule> value) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(value));
    };
    api.retrieve("", {}, failure);
    api.create({}, {}, failure);
    api.update("", update_request(), {}, failure);
    for (int invalid = 0; invalid < 17; ++invalid) {
        auto request = update_request();
        switch (invalid) {
        case 0:
            request.phases.clear();
            break;
        case 1:
            request.phases.resize(11);
            break;
        case 2:
            request.phases[0].start_date = 0;
            break;
        case 3:
            request.phases[0].end_date = 100;
            break;
        case 4:
            request.phases[0].end_date.reset();
            break;
        case 5:
            request.phases[0].duration = SubscriptionSchedulePhaseDuration{ "month", 1 };
            break;
        case 6:
            request.phases[1].duration->interval_count = 0;
            break;
        case 7:
            request.phases[1].start_date = 201;
            break;
        case 8:
            request.phases[0].items.clear();
            break;
        case 9:
            request.phases[0].items.resize(21);
            break;
        case 10:
            request.phases[0].items[0].price_id.clear();
            break;
        case 11:
            request.phases[0].items[0].quantity = 0;
            break;
        case 12:
            request.end_behavior = static_cast<SubscriptionScheduleEndBehavior>(99);
            break;
        case 13:
            request.proration_behavior = static_cast<ProrationBehavior>(99);
            break;
        case 14:
            request.phases[1].proration_behavior = static_cast<ProrationBehavior>(99);
            break;
        case 15:
            request.phases[0].discounts = { {} };
            break;
        case 16:
            request.phases[1].duration->interval = "invalid";
            break;
        }
        EXPECT_THROW(api.update_async("sched", request).result(), ConfigurationException);
        api.update("sched", request, {}, failure);
    }
    EXPECT_EQ(calls, 20);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(SubscriptionSchedulesTest, ParsesActiveReleasedAndFutureValuesAndRejectsMalformedSchedules) {
    JsonStripeResponseParser parser;
    auto value = schedule_json();
    const auto parsed = parser.parse_subscription_schedule(response(value));
    EXPECT_EQ(parsed.customer_id, "cus_test");
    EXPECT_EQ(parsed.subscription_id, "sub_test");
    EXPECT_EQ(parsed.current_phase->end_date_unix, 200);
    EXPECT_EQ(parsed.phases[1].items[0].price_id, "price_new");
    EXPECT_EQ(parsed.phases[0].items[0].quantity.value(), 3);
    EXPECT_EQ(parsed.phases[0].metadata.at("label"), "phase");
    EXPECT_EQ(parsed.metadata.at("operation"), "saved");
    value["status"] = "released";
    value["end_behavior"] = "future_policy";
    value["subscription"] = Json::nullValue;
    value["current_phase"] = Json::nullValue;
    value["released_subscription"]["id"] = "sub_test";
    value["phases"][0]["items"][0]["quantity"] = Json::nullValue;
    const auto released = parser.parse_subscription_schedule(response(value));
    EXPECT_EQ(released.status, "released");
    EXPECT_EQ(released.end_behavior, "future_policy");
    EXPECT_TRUE(released.subscription_id.empty());
    EXPECT_EQ(released.released_subscription_id, "sub_test");
    EXPECT_FALSE(released.current_phase.has_value());
    EXPECT_FALSE(released.phases[0].items[0].quantity.has_value());
    for (int invalid = 0; invalid < 12; ++invalid) {
        value = schedule_json();
        switch (invalid) {
        case 0:
            value.removeMember("id");
            break;
        case 1:
            value["object"] = "subscription";
            break;
        case 2:
            value["customer"] = false;
            break;
        case 3:
            value["status"] = "";
            break;
        case 4:
            value.removeMember("end_behavior");
            break;
        case 5:
            value["phases"] = Json::nullValue;
            break;
        case 6:
            value["current_phase"]["end_date"] = 100;
            break;
        case 7:
            value["phases"][0]["start_date"] = "100";
            break;
        case 8:
            value["phases"][0]["items"] = Json::Value(Json::arrayValue);
            break;
        case 9:
            value["phases"][0]["items"][0]["price"] = Json::nullValue;
            break;
        case 10:
            value["phases"][0]["items"][0]["quantity"] = -1;
            break;
        case 11:
            value["phases"][0]["proration_behavior"] = false;
            break;
        }
        EXPECT_THROW(parser.parse_subscription_schedule(response(value)), ConfigurationException);
    }
}

TEST_F(SubscriptionSchedulesTest, ParsesSubscriptionSafetySettingsAndRejectsMalformedValues) {
    JsonStripeResponseParser parser;
    Json::Value value;
    value["id"] = "sub";
    value["status"] = "active";
    value["customer"]["id"] = "cus";
    value["schedule"]["id"] = "sched";
    value["discounts"].append("di_first");
    value["discounts"].append(Json::Value(Json::objectValue));
    value["discounts"][1]["id"] = "di_other";
    value["default_tax_rates"].append("txr");
    value["automatic_tax"]["enabled"] = true;
    value["items"]["has_more"] = false;
    auto& item = value["items"]["data"][0];
    item["id"] = "si";
    item["price"] = "price";
    item["discounts"].append("di_item");
    item["tax_rates"].append("txr_item");
    const auto parsed = parser.parse_subscription(response(value));
    EXPECT_EQ(parsed.customer_id, "cus");
    EXPECT_EQ(parsed.schedule_id, "sched");
    EXPECT_EQ(parsed.discount_ids.size(), 2U);
    EXPECT_EQ(parsed.default_tax_rate_ids[0], "txr");
    EXPECT_TRUE(parsed.automatic_tax_enabled);
    EXPECT_EQ(parsed.items[0].discount_ids[0], "di_item");
    EXPECT_EQ(parsed.items[0].tax_rate_ids[0], "txr_item");
    value["automatic_tax"]["enabled"] = false;
    value["automatic_tax"]["disabled_reason"] = "requires_location_inputs";
    const auto disabled = parser.parse_subscription(response(value));
    EXPECT_FALSE(disabled.automatic_tax_enabled);
    EXPECT_EQ(disabled.automatic_tax_disabled_reason, "requires_location_inputs");
    const auto original = value;
    for (const auto* field : { "schedule", "discounts", "default_tax_rates", "automatic_tax" }) {
        value = original;
        value[field] = false;
        EXPECT_THROW(parser.parse_subscription(response(value)), ConfigurationException);
    }
    value = original;
    value["automatic_tax"]["enabled"] = "false";
    EXPECT_THROW(parser.parse_subscription(response(value)), ConfigurationException);
    value = original;
    value["items"]["data"][0]["discounts"] = false;
    EXPECT_THROW(parser.parse_subscription(response(value)), ConfigurationException);
    value = original;
    value["items"]["data"][0]["tax_rates"].append(Json::nullValue);
    EXPECT_THROW(parser.parse_subscription(response(value)), ConfigurationException);
    value = original;
    for (const auto* field :
         { "schedule", "discount", "discounts", "default_tax_rates", "automatic_tax" })
        value.removeMember(field);
    const auto absent = parser.parse_subscription(response(value));
    EXPECT_TRUE(absent.schedule_id.empty());
    EXPECT_TRUE(absent.discount_ids.empty());
    EXPECT_TRUE(absent.default_tax_rate_ids.empty());
    EXPECT_FALSE(absent.automatic_tax_enabled);
}

TEST_F(SubscriptionSchedulesTest, EveryOperationPropagatesProviderTransportAndParsingFailures) {
    auto& api = client.subscription_schedules();
    int calls = 0;
    const auto callback = [&](CallbackResult<SubscriptionSchedule> value) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(value));
    };
    for (int failure = 0; failure < 3; ++failure) {
        const auto configure = [&] {
            if (failure == 0)
                http.set_response(
                    { .status_code = 400,
                      .body =
                          R"({"error":{"type":"invalid_request_error","message":"Rejected"}})" });
            else if (failure == 1)
                http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
            else http.set_response({ .status_code = 200, .body = "{}" });
        };
        configure();
        EXPECT_THROW(api.retrieve_async("sched").result(), std::exception);
        configure();
        EXPECT_THROW(api.create_async({ "sub" }).result(), std::exception);
        configure();
        EXPECT_THROW(api.update_async("sched", update_request()).result(), std::exception);
        configure();
        api.retrieve("sched", {}, callback);
        configure();
        api.create({ "sub" }, {}, callback);
        configure();
        api.update("sched", update_request(), {}, callback);
    }
    EXPECT_EQ(calls, 9);
}
