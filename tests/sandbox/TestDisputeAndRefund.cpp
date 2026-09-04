/** TestDisputeAndRefund.cpp
 *
 * Sandbox end-to-end tests for the card dispute and refund signals host
 * applications use to revoke entitlements.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include <string>

using namespace stripekit;
using namespace stripekit::testing;

class SandboxDisputeAndRefundTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    SandboxEnvironment* environment = nullptr;
};

TEST_F(SandboxDisputeAndRefundTest, RefundedChargeEmitsChargeRefunded) {
    const std::string charge_id =
        create_confirmed_charge(*environment, "pm_card_visa", 2500);

    environment->stripe_api().require_post("/v1/refunds", {
        {"charge", charge_id},
        {"metadata[stripekit_run_id]", environment->run_id()}
    });

    const auto event = environment->wait_for_event("charge.refunded", charge_id);
    if (!event) {
        FAIL() << "Stripe did not deliver charge.refunded for charge " << charge_id;
    }

    const Json::Value payload = event->json();
    EXPECT_EQ(json_path(payload, "data.object.id").asString(), charge_id);
    EXPECT_TRUE(json_path(payload, "data.object.refunded").asBool());
    EXPECT_FALSE(json_path(payload, "livemode").asBool());
}

TEST_F(SandboxDisputeAndRefundTest, DisputeLifecycleEmitsCreatedAndClosed) {
    const std::string charge_id =
        create_confirmed_charge(*environment, "pm_card_createDispute", 2500);

    const auto opened = environment->wait_for_event("charge.dispute.created", charge_id);
    if (!opened) {
        FAIL() << "Stripe did not deliver charge.dispute.created for charge " << charge_id;
    }

    const std::string dispute_id = json_path(opened->json(), "data.object.id").asString();
    EXPECT_FALSE(dispute_id.empty());
    EXPECT_EQ(json_path(opened->json(), "data.object.charge").asString(), charge_id);

    // In a sandbox, this evidence text resolves the dispute in the merchant's favour.
    environment->stripe_api().require_post("/v1/disputes/" + dispute_id, {
        {"evidence[uncategorized_text]", "winning_evidence"},
        {"submit", "true"}
    });

    const auto closed = environment->wait_for_event("charge.dispute.closed", dispute_id);
    if (!closed) {
        FAIL() << "Stripe did not deliver charge.dispute.closed for dispute " << dispute_id;
    }

    const Json::Value payload = closed->json();
    EXPECT_EQ(json_path(payload, "data.object.id").asString(), dispute_id);
    EXPECT_EQ(json_path(payload, "data.object.status").asString(), "won");
    EXPECT_FALSE(json_path(payload, "livemode").asBool());
}
