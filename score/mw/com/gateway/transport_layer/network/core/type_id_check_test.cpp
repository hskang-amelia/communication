/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "score/mw/com/gateway/transport_layer/network/core/type_id_check.h"

#include <gtest/gtest.h>

namespace score::mw::com::gateway::network
{
namespace
{

TEST(TypeIdCheckTest, CompareCoversAllCases)
{
    EXPECT_EQ(CompareTypeId("A@1", "A@1"), TypeIdVerdict::kMatch);
    EXPECT_EQ(CompareTypeId("A@1", "A@2"), TypeIdVerdict::kMismatch);
    EXPECT_EQ(CompareTypeId("", "A@1"), TypeIdVerdict::kMissingLocal);
    EXPECT_EQ(CompareTypeId("A@1", ""), TypeIdVerdict::kMissingRemote);
    EXPECT_EQ(CompareTypeId("", ""), TypeIdVerdict::kMissingBoth);
}

TEST(TypeIdCheckTest, MismatchIsRejectedUnderEveryPolicy)
{
    EXPECT_FALSE(IsAccepted(TypeIdVerdict::kMismatch, TypeIdPolicy::kWarn));
    EXPECT_FALSE(IsAccepted(TypeIdVerdict::kMismatch, TypeIdPolicy::kStrict));
    EXPECT_TRUE(IsAccepted(TypeIdVerdict::kMatch, TypeIdPolicy::kWarn));
    EXPECT_TRUE(IsAccepted(TypeIdVerdict::kMatch, TypeIdPolicy::kStrict));
}

TEST(TypeIdCheckTest, MissingIdsDependOnPolicy)
{
    for (const auto verdict :
         {TypeIdVerdict::kMissingLocal, TypeIdVerdict::kMissingRemote, TypeIdVerdict::kMissingBoth})
    {
        EXPECT_TRUE(IsAccepted(verdict, TypeIdPolicy::kWarn)) << ToString(verdict);
        EXPECT_FALSE(IsAccepted(verdict, TypeIdPolicy::kStrict)) << ToString(verdict);
    }
}

TEST(TypeIdCheckTest, DefaultPolicyIsStrict)
{
    EXPECT_EQ(TypeIdConfig{}.policy, TypeIdPolicy::kStrict);
}

TEST(TypeIdCheckTest, FindReturnsEmptyForUnknownEntries)
{
    TypeIdConfig config{};
    config.type_ids["/vehicle/drive"]["mode"] = "DriveMode@1.2";
    EXPECT_EQ(config.Find("/vehicle/drive", "mode"), "DriveMode@1.2");
    EXPECT_EQ(config.Find("/vehicle/drive", "other"), "");
    EXPECT_EQ(config.Find("/vehicle/other", "mode"), "");
}

}  // namespace
}  // namespace score::mw::com::gateway::network
