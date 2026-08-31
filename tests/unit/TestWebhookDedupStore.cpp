/** TestWebhookDedupStore.cpp
 * 
 * Unit tests for in-memory webhook de-duplication store.
 */

#include "StripeKit/WebhookDedupStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace stripekit;

class WebhookDedupStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        now = std::chrono::system_clock::from_time_t(1700000000);
    }

    std::chrono::system_clock::time_point now;
    InMemoryWebhookDedupStore store;
};

TEST_F(WebhookDedupStoreTest, FirstClaimSucceeds) {
    const ClaimRequest request{
        .event_id = "evt_test_1",
        .now = now,
        .stale_inflight_timeout = std::chrono::minutes(15)
    };

    const ClaimResult result = store.claim(request);
    EXPECT_EQ(result.outcome, ClaimOutcome::Claimed);
    EXPECT_TRUE(result.should_process());
    EXPECT_EQ(result.detail, "first-seen");
}

TEST_F(WebhookDedupStoreTest, DuplicateInflightReturnsDuplicateInflight) {
    const ClaimRequest request{
        .event_id = "evt_test_2",
        .now = now,
        .stale_inflight_timeout = std::chrono::minutes(15)
    };

    EXPECT_EQ(store.claim(request).outcome, ClaimOutcome::Claimed);

    // Immediate second claim while in-flight
    const ClaimResult second_result = store.claim(request);
    EXPECT_EQ(second_result.outcome, ClaimOutcome::DuplicateInflight);
    EXPECT_FALSE(second_result.should_process());
    EXPECT_EQ(second_result.detail, "already-claimed-inflight");
}

TEST_F(WebhookDedupStoreTest, MarkProcessedAndSubsequentClaim) {
    const ClaimRequest request{
        .event_id = "evt_test_3",
        .now = now,
        .stale_inflight_timeout = std::chrono::minutes(15)
    };

    EXPECT_EQ(store.claim(request).outcome, ClaimOutcome::Claimed);

    const bool marked = store.mark_processed("evt_test_3", now + std::chrono::seconds(2));
    EXPECT_TRUE(marked);

    const ClaimResult third_result = store.claim(request);
    EXPECT_EQ(third_result.outcome, ClaimOutcome::DuplicateProcessed);
    EXPECT_FALSE(third_result.should_process());
    EXPECT_EQ(third_result.detail, "already-processed");
}

TEST_F(WebhookDedupStoreTest, MarkProcessedUnknownEventReturnsFalse) {
    const bool marked = store.mark_processed("evt_non_existent", now);
    EXPECT_FALSE(marked);
}

TEST_F(WebhookDedupStoreTest, StaleInflightIsReclaimed) {
    const ClaimRequest initial_request{
        .event_id = "evt_test_4",
        .now = now,
        .stale_inflight_timeout = std::chrono::minutes(15)
    };

    EXPECT_EQ(store.claim(initial_request).outcome, ClaimOutcome::Claimed);

    // 14 minutes later - still in flight
    const ClaimRequest inflight_request{
        .event_id = "evt_test_4",
        .now = now + std::chrono::minutes(14),
        .stale_inflight_timeout = std::chrono::minutes(15)
    };
    EXPECT_EQ(store.claim(inflight_request).outcome, ClaimOutcome::DuplicateInflight);

    // 15 minutes later - should be reclaimed
    const ClaimRequest expired_request{
        .event_id = "evt_test_4",
        .now = now + std::chrono::minutes(15),
        .stale_inflight_timeout = std::chrono::minutes(15)
    };
    const ClaimResult reclaimed = store.claim(expired_request);
    EXPECT_EQ(reclaimed.outcome, ClaimOutcome::Claimed);
    EXPECT_TRUE(reclaimed.should_process());
    EXPECT_EQ(reclaimed.detail, "reclaimed-stale-inflight");
}

TEST_F(WebhookDedupStoreTest, EraseExpiredRemovesOldRecords) {
    const ClaimRequest request1{
        .event_id = "evt_old_processed",
        .now = now,
        .stale_inflight_timeout = std::chrono::minutes(15)
    };
    store.claim(request1);
    store.mark_processed("evt_old_processed", now);

    const ClaimRequest request2{
        .event_id = "evt_fresh_processed",
        .now = now + std::chrono::hours(20),
        .stale_inflight_timeout = std::chrono::minutes(15)
    };
    store.claim(request2);
    store.mark_processed("evt_fresh_processed", now + std::chrono::hours(20));

    // Erase records older than 24 hours relative to now + 25 hours
    store.erase_expired(now + std::chrono::hours(25), std::chrono::hours(24));

    // request1 was processed at 'now', age is 25 hours >= 24h -> erased, so claiming succeeds as first-seen
    const ClaimResult reclaim_old = store.claim(ClaimRequest{
        .event_id = "evt_old_processed",
        .now = now + std::chrono::hours(25),
        .stale_inflight_timeout = std::chrono::minutes(15)
    });
    EXPECT_EQ(reclaim_old.outcome, ClaimOutcome::Claimed);
    EXPECT_EQ(reclaim_old.detail, "first-seen");

    // request2 was processed at 'now + 20h', age is 5 hours < 24h -> still DuplicateProcessed
    const ClaimResult check_fresh = store.claim(ClaimRequest{
        .event_id = "evt_fresh_processed",
        .now = now + std::chrono::hours(25),
        .stale_inflight_timeout = std::chrono::minutes(15)
    });
    EXPECT_EQ(check_fresh.outcome, ClaimOutcome::DuplicateProcessed);
}

TEST_F(WebhookDedupStoreTest, MapClaimOutcomeToResponseActions) {
    EXPECT_EQ(map_claim_outcome_to_response(ClaimOutcome::Claimed),
              WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(map_claim_outcome_to_response(ClaimOutcome::DuplicateProcessed),
              WebhookResponseAction::AcknowledgeSuccess);
    EXPECT_EQ(map_claim_outcome_to_response(ClaimOutcome::DuplicateInflight),
              WebhookResponseAction::RetryLater);
    EXPECT_EQ(map_claim_outcome_to_response(ClaimOutcome::RetryableStoreError),
              WebhookResponseAction::RetryLater);
    EXPECT_EQ(map_claim_outcome_to_response(ClaimOutcome::FatalStoreError),
              WebhookResponseAction::RetryLater);
}
