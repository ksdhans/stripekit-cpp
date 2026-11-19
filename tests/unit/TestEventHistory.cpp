/** TestEventHistory.cpp
 *
 * See EventHistory.h for details.
 */
#include "StripeKit/Testing/EventHistory.h"
#include "TestFramework.h"

#include <stdexcept>
#include <vector>

using namespace stripekit::testing;

namespace {

/** Return a Stripe event without requiring network access or credentials. */
Json::Value history_event(std::string id, std::string invoice_id) {
    Json::Value event;
    event["id"] = id;
    event["type"] = "invoice.paid";
    event["created"] = Json::Int64(1800000001);
    event["data"]["object"]["id"] = invoice_id;
    event["data"]["object"]["customer_email"] = "must-not-be-logged@example.com";
    return event;
}

/** Return a decoded Stripe list page. */
Json::Value history_page(std::initializer_list<Json::Value> events, bool has_more = false) {
    Json::Value page;
    page["data"] = Json::Value(Json::arrayValue);
    for (const auto& event : events)
        page["data"].append(event);
    page["has_more"] = has_more;
    return page;
}

} // namespace

TEST(EventHistoryTest, FindsExpectedObjectOnLaterPageWithoutLoggingPayloads) {
    std::vector<std::string> paths;
    const auto report = event_history_diagnostics(
        [&](std::string_view path) {
            paths.emplace_back(path);
            return paths.size() == 1 ? history_page({ history_event("evt/a?", "in_other") }, true)
                                     : history_page({ history_event("evt_match", "in_target") });
        },
        "invoice.paid",
        "in_target",
        1800000000);
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[0], "/v1/events?limit=100&type=invoice.paid&created%5Bgte%5D=1800000000");
    EXPECT_EQ(paths[1], paths[0] + "&starting_after=evt%2Fa%3F");
    EXPECT_TRUE(report.find("Found invoice.paid (evt_match), created=1800000001") !=
                std::string::npos);
    EXPECT_TRUE(report.find("webhook wait timed out") != std::string::npos);
    EXPECT_TRUE(report.find("evt/a?") == std::string::npos);
    EXPECT_TRUE(report.find("must-not-be-logged") == std::string::npos);
}

TEST(EventHistoryTest, ReportsNoMatchForOtherObjectsAndEventTypes) {
    auto other_type = history_event("evt_wrong_type", "in_target");
    other_type["type"] = "invoice.created";
    const auto report = event_history_diagnostics(
        [&](std::string_view) {
            return history_page({ history_event("evt_other", "in_other"), other_type });
        },
        "invoice.paid",
        "in_target",
        1800000000);
    EXPECT_TRUE(report.find("No matching event found in 2 events") != std::string::npos);
    const auto empty = event_history_diagnostics(
        [](std::string_view) { return history_page({}); }, "invoice.paid", "in_target", 1800000000);
    EXPECT_TRUE(empty.find("No matching event found in 0 events") != std::string::npos);
}

TEST(EventHistoryTest, CapsHistoryRequestsAndReportsIncompleteSearch) {
    int calls = 0;
    const auto report = event_history_diagnostics(
        [&](std::string_view) {
            ++calls;
            auto page = history_page({}, true);
            for (int index = 0; index < 100; ++index)
                page["data"].append(
                    history_event("evt_" + std::to_string(calls * 100 + index), "in_other"));
            return page;
        },
        "invoice.paid",
        "in_target",
        1800000000);
    EXPECT_EQ(calls, 3);
    EXPECT_TRUE(report.find("Search incomplete: page limit reached") != std::string::npos);
    EXPECT_TRUE(report.find("No matching event") == std::string::npos);
}

TEST(EventHistoryTest, ReportsLookupErrorsRatherThanClaimingTheEventIsMissing) {
    const auto report = event_history_diagnostics(
        [](std::string_view) -> Json::Value { throw std::runtime_error("HTTP 403"); },
        "invoice.paid",
        "in_target",
        1800000000);
    EXPECT_TRUE(report.find("Event history lookup failed: HTTP 403") != std::string::npos);
    EXPECT_TRUE(report.find("No matching event") == std::string::npos);
    for (const auto& page :
         { Json::Value(), history_page({}, true), history_page({ Json::Value("invalid") }) }) {
        const auto malformed = event_history_diagnostics(
            [&](std::string_view) { return page; }, "invoice.paid", "in_target", 1800000000);
        EXPECT_TRUE(malformed.find("Event history lookup failed:") != std::string::npos);
    }
}

TEST(EventHistoryTest, RejectsPaginationThatDoesNotAdvance) {
    int calls = 0;
    const auto report = event_history_diagnostics(
        [&](std::string_view) {
            ++calls;
            return history_page({ history_event("evt_repeated", "in_other") }, true);
        },
        "invoice.paid",
        "in_target",
        1800000000);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(report.find("pagination did not advance") != std::string::npos);
}
