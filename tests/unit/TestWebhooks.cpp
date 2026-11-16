/** TestWebhooks.cpp
 *
 * Unit tests for webhook construction and single-event processing pipeline.
 */

#include "TestFramework.h"
#include "TestHelpers.h"

#include "StripeKit/Errors.h"
#include "StripeKit/Webhooks.h"
#include "StripeKit/SqliteWebhookDedupStore.h"

#include <chrono>
#include <string>

using namespace stripekit;
using namespace stripekit::testing;

/** Simulates database failures while completing or releasing a claim. */
class FailingCompletionStore final : public WebhookDedupStore {
public:
    bool fail_release = false;
    InMemoryWebhookDedupStore records;

    /** Reserves an event using the normal in-memory store. */
    ClaimResult claim(const ClaimRequest& request) override {
        return records.claim(request);
    }

    /** Simulates a failed completion write. */
    bool mark_processed(std::string_view, std::chrono::system_clock::time_point) override {
        return false;
    }

    /** Releases normally, or simulates a database error. */
    bool release_claim(std::string_view id,
                       std::chrono::system_clock::time_point claimed_at) override {
        if (fail_release) {
            throw ConfigurationException("Release unavailable");
        }
        return records.release_claim(id, claimed_at);
    }

    /** Applies normal retention behavior. */
    void erase_expired(std::chrono::system_clock::time_point now,
                       std::chrono::hours retention) override {
        records.erase_expired(now, retention);
    }
};

class WebhooksTest : public ::testing::Test {
protected:
    void SetUp() override {
        secret = "whsec_webhooks_unit_test_secret_123456789";
        now = std::chrono::system_clock::from_time_t(1700000000);
        timestamp_unix = 1700000000;

        valid_payload = R"({
            "id": "evt_12345",
            "object": "event",
            "type": "invoice.paid",
            "api_version": "2026-10-28.endive",
            "created": 1700000000,
            "livemode": false,
            "request": {
                "id": "req_abc123"
            },
            "account": "acct_test456"
        })";

        valid_signature_header =
            make_stripe_signature_header(timestamp_unix, valid_payload, secret);
    }

    std::string secret;
    std::chrono::system_clock::time_point now;
    std::int64_t timestamp_unix = 0;
    std::string valid_payload;
    std::string valid_signature_header;
};

TEST_F(WebhooksTest, ConstructEventValid) {
    const webhooks::Event event = webhooks::construct_event(
        valid_payload,
        valid_signature_header,
        secret,
        webhooks::ConstructEventOptions{ .now = now, .tolerance = std::chrono::minutes(5) });

    EXPECT_EQ(event.id, "evt_12345");
    EXPECT_EQ(event.type, "invoice.paid");
    EXPECT_EQ(event.api_version, "2026-10-28.endive");
    EXPECT_EQ(event.created_unix, 1700000000);
    EXPECT_FALSE(event.livemode);
    EXPECT_EQ(event.request_id, "req_abc123");
    EXPECT_EQ(event.account, "acct_test456");
    EXPECT_EQ(event.payload_json, valid_payload);
}

TEST_F(WebhooksTest, ConstructEventInvalidSignatureThrows) {
    EXPECT_THROW(
        {
            webhooks::construct_event(valid_payload,
                                      "t=1700000000,v1=invalid_signature",
                                      secret,
                                      webhooks::ConstructEventOptions{
                                          .now = now, .tolerance = std::chrono::minutes(5) });
        },
        SignatureVerificationException);
}

TEST_F(WebhooksTest, ConstructEventInvalidJsonThrows) {
    const std::string bad_json = "not a valid json payload";
    const std::string sig_header = make_stripe_signature_header(timestamp_unix, bad_json, secret);

    EXPECT_THROW(
        {
            webhooks::construct_event(bad_json,
                                      sig_header,
                                      secret,
                                      webhooks::ConstructEventOptions{
                                          .now = now, .tolerance = std::chrono::minutes(5) });
        },
        ConfigurationException);
}

TEST_F(WebhooksTest, ConstructEventMissingIdThrows) {
    const std::string payload_missing_id = R"({"type": "invoice.paid"})";
    const std::string sig_header =
        make_stripe_signature_header(timestamp_unix, payload_missing_id, secret);

    EXPECT_THROW(
        {
            webhooks::construct_event(payload_missing_id,
                                      sig_header,
                                      secret,
                                      webhooks::ConstructEventOptions{
                                          .now = now, .tolerance = std::chrono::minutes(5) });
        },
        ConfigurationException);
}

TEST_F(WebhooksTest, ProcessEventOnceHandled) {
    InMemoryWebhookDedupStore dedup_store;
    TestLogger logger;
    bool handler_called = false;

    const webhooks::Event event{ .id = "evt_handled_1", .type = "invoice.paid" };

    auto task = webhooks::process_event_once(
        event,
        dedup_store,
        [&](const webhooks::Event& ev) -> Task<webhooks::ProcessResult> {
            handler_called = true;
            EXPECT_EQ(ev.id, "evt_handled_1");
            co_return webhooks::ProcessResult::Handled;
        },
        webhooks::ProcessEventOptions{ .now = now, .logger = &logger });

    const webhooks::ProcessEventOnceResult result = task.result();
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(result.claim_outcome, ClaimOutcome::Claimed);
    EXPECT_EQ(result.response_action, WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(result.event_id, "evt_handled_1");
    EXPECT_EQ(result.event_type, "invoice.paid");
}

TEST_F(WebhooksTest, ProcessEventOnceIgnored) {
    InMemoryWebhookDedupStore dedup_store;
    bool handler_called = false;

    const webhooks::Event event{ .id = "evt_ignored_1", .type = "customer.created" };

    auto task = webhooks::process_event_once(
        event,
        dedup_store,
        [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
            handler_called = true;
            co_return webhooks::ProcessResult::Ignored;
        },
        webhooks::ProcessEventOptions{ .now = now });

    const webhooks::ProcessEventOnceResult result = task.result();
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(result.claim_outcome, ClaimOutcome::Claimed);
    EXPECT_EQ(result.response_action, WebhookResponseAction::AcknowledgeSuccess);
}

TEST_F(WebhooksTest, ProcessEventOnceDuplicateProcessedDoesNotCallHandler) {
    InMemoryWebhookDedupStore dedup_store;
    int handler_call_count = 0;

    const webhooks::Event event{ .id = "evt_dup_1", .type = "invoice.paid" };

    auto handler = [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
        ++handler_call_count;
        co_return webhooks::ProcessResult::Handled;
    };

    // First call
    auto task1 = webhooks::process_event_once(
        event, dedup_store, handler, webhooks::ProcessEventOptions{ .now = now });
    EXPECT_EQ(task1.result().response_action, WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(handler_call_count, 1);

    // Second call: duplicate processed
    auto task2 = webhooks::process_event_once(
        event, dedup_store, handler, webhooks::ProcessEventOptions{ .now = now });
    const auto result2 = task2.result();
    EXPECT_EQ(result2.claim_outcome, ClaimOutcome::DuplicateProcessed);
    EXPECT_EQ(result2.response_action, WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(handler_call_count, 1); // Handler was not invoked again
}

TEST_F(WebhooksTest, ProcessEventOnceDuplicateInflightReturnsRetryLater) {
    InMemoryWebhookDedupStore dedup_store;
    // Pre-claim without marking processed
    dedup_store.claim(ClaimRequest{ .event_id = "evt_inflight_1", .now = now });

    bool handler_called = false;
    const webhooks::Event event{ .id = "evt_inflight_1", .type = "invoice.paid" };

    auto task = webhooks::process_event_once(
        event,
        dedup_store,
        [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
            handler_called = true;
            co_return webhooks::ProcessResult::Handled;
        },
        webhooks::ProcessEventOptions{ .now = now });

    const auto result = task.result();
    EXPECT_FALSE(handler_called);
    EXPECT_EQ(result.claim_outcome, ClaimOutcome::DuplicateInflight);
    EXPECT_EQ(result.response_action, WebhookResponseAction::RetryLater);
}

TEST_F(WebhooksTest, ProcessEventOnceHandlerThrowsReturnsRetryLater) {
    InMemoryWebhookDedupStore dedup_store;
    TestLogger logger;

    const webhooks::Event event{ .id = "evt_throw_1", .type = "invoice.paid" };

    auto task = webhooks::process_event_once(
        event,
        dedup_store,
        [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
            throw std::runtime_error("Database connection dropped");
        },
        webhooks::ProcessEventOptions{ .now = now, .logger = &logger });

    const auto result = task.result();
    EXPECT_EQ(result.claim_outcome, ClaimOutcome::Claimed);
    EXPECT_EQ(result.response_action, WebhookResponseAction::RetryLater);

    // Verify error was logged
    const auto log_entries = logger.get_entries();
    EXPECT_FALSE(log_entries.empty());
    EXPECT_EQ(log_entries[0].level, LogLevel::Error);
}

TEST_F(WebhooksTest, ProcessEventOnceHandlerRetryableFailureReturnsRetryLater) {
    InMemoryWebhookDedupStore dedup_store;

    const webhooks::Event event{ .id = "evt_retry_1", .type = "invoice.paid" };

    auto task = webhooks::process_event_once(
        event,
        dedup_store,
        [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
            co_return webhooks::ProcessResult::RetryableFailure;
        },
        webhooks::ProcessEventOptions{ .now = now });

    const auto result = task.result();
    EXPECT_EQ(result.response_action, WebhookResponseAction::RetryLater);
}

TEST_F(WebhooksTest, FailedHandlersCanRetryImmediatelyWithDefaultTimeout) {
    InMemoryWebhookDedupStore store;
    for (const auto failure :
         { webhooks::ProcessResult::RetryableFailure, webhooks::ProcessResult::FatalFailure }) {
        const webhooks::Event event{ .id = failure == webhooks::ProcessResult::RetryableFailure
                                               ? "evt_retry"
                                               : "evt_fatal",
                                     .type = "invoice.paid" };
        int attempts = 0;
        auto handler = [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
            co_return ++attempts == 1 ? failure : webhooks::ProcessResult::Handled;
        };
        EXPECT_EQ(webhooks::process_event_once(
                      event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                      .result()
                      .response_action,
                  WebhookResponseAction::RetryLater);
        EXPECT_EQ(webhooks::process_event_once(
                      event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                      .result()
                      .response_action,
                  WebhookResponseAction::AcknowledgeSuccess);
        EXPECT_EQ(attempts, 2);
        EXPECT_EQ(store.claim(ClaimRequest{ .event_id = event.id, .now = now }).outcome,
                  ClaimOutcome::DuplicateProcessed);
    }
}

TEST_F(WebhooksTest, ThrowingHandlerCanRetryImmediately) {
    InMemoryWebhookDedupStore store;
    const webhooks::Event event{ .id = "evt_throw_retry", .type = "invoice.paid" };
    int attempts = 0;
    auto handler = [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
        if (++attempts == 1) {
            throw std::runtime_error("Temporary database failure");
        }
        co_return webhooks::ProcessResult::Handled;
    };
    EXPECT_EQ(webhooks::process_event_once(
                  event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                  .result()
                  .response_action,
              WebhookResponseAction::RetryLater);
    EXPECT_EQ(webhooks::process_event_once(
                  event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                  .result()
                  .response_action,
              WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(attempts, 2);
}

TEST_F(WebhooksTest, FailedCompletionWriteReleasesClaim) {
    FailingCompletionStore store;
    const webhooks::Event event{ .id = "evt_completion_failed", .type = "invoice.paid" };
    auto handler = [](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
        co_return webhooks::ProcessResult::Handled;
    };
    TestLogger logger;
    EXPECT_EQ(
        webhooks::process_event_once(
            event, store, handler, webhooks::ProcessEventOptions{ .now = now, .logger = &logger })
            .result()
            .response_action,
        WebhookResponseAction::RetryLater);
    EXPECT_EQ(store.records.claim(ClaimRequest{ .event_id = event.id, .now = now }).outcome,
              ClaimOutcome::Claimed);
    ASSERT_EQ(logger.get_entries().size(), 1u);
    EXPECT_EQ(logger.get_entries()[0].level, LogLevel::Error);
}

TEST_F(WebhooksTest, FailedReleaseIsLoggedAndStillRequestsRetry) {
    FailingCompletionStore store;
    store.fail_release = true;
    const webhooks::Event event{ .id = "evt_release_failed", .type = "invoice.paid" };
    TestLogger logger;
    EXPECT_EQ(webhooks::process_event_once(
                  event,
                  store,
                  [](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
                      co_return webhooks::ProcessResult::RetryableFailure;
                  },
                  webhooks::ProcessEventOptions{ .now = now, .logger = &logger })
                  .result()
                  .response_action,
              WebhookResponseAction::RetryLater);
    EXPECT_EQ(store.records.claim(ClaimRequest{ .event_id = event.id, .now = now }).outcome,
              ClaimOutcome::DuplicateInflight);
    const auto logs = logger.get_entries();
    ASSERT_EQ(logs.size(), 2u);
    EXPECT_EQ(logs[1].level, LogLevel::Error);
    EXPECT_NE(logs[1].message.find("claim release failed"), std::string::npos);
}

#if defined(STRIPEKIT_WITH_SQLITE3)
TEST_F(WebhooksTest, SqliteFailedHandlerCanRetryImmediately) {
    SqliteWebhookDedupStore store(":memory:");
    const webhooks::Event event{ .id = "evt_sqlite_retry", .type = "invoice.paid" };
    int attempts = 0;
    auto handler = [&](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
        co_return ++attempts == 1 ? webhooks::ProcessResult::RetryableFailure
                                  : webhooks::ProcessResult::Handled;
    };
    EXPECT_EQ(webhooks::process_event_once(
                  event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                  .result()
                  .response_action,
              WebhookResponseAction::RetryLater);
    EXPECT_EQ(webhooks::process_event_once(
                  event, store, handler, webhooks::ProcessEventOptions{ .now = now })
                  .result()
                  .response_action,
              WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(attempts, 2);
}
#endif

TEST_F(WebhooksTest, ProcessEventOnceWithPayloadAndSignatureOverload) {
    InMemoryWebhookDedupStore dedup_store;
    bool handler_called = false;

    auto task = webhooks::process_event_once(
        valid_payload,
        valid_signature_header,
        secret,
        dedup_store,
        [&](const webhooks::Event& ev) -> Task<webhooks::ProcessResult> {
            handler_called = true;
            EXPECT_EQ(ev.id, "evt_12345");
            co_return webhooks::ProcessResult::Handled;
        },
        webhooks::ProcessEventOptions{ .now = now });

    const auto result = task.result();
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(result.response_action, WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(result.claim_outcome, ClaimOutcome::Claimed);
}

TEST_F(WebhooksTest, ProcessEventOnceWithPayloadBadSignatureThrows) {
    InMemoryWebhookDedupStore dedup_store;

    EXPECT_THROW(
        {
            auto task = webhooks::process_event_once(
                valid_payload,
                "t=1700000000,v1=bad_sig",
                secret,
                dedup_store,
                [](const webhooks::Event&) -> Task<webhooks::ProcessResult> {
                    co_return webhooks::ProcessResult::Handled;
                },
                webhooks::ProcessEventOptions{ .now = now });
            task.result();
        },
        SignatureVerificationException);
}
