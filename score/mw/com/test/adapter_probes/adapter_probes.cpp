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

/// Behaviour probes used to classify mw::com against AUTOSAR AP R20-11 ara::com requirements.
/// The probes observe and report; they only fail (non-zero exit / terminate) when the process itself breaks.
/// Every finding is printed as a single line: "PROBE_RESULT <requirement> key=value ...".
///
/// Modes (argv[1]):
///   rh_serial    - SWS_CM_00182: are EventReceiveHandler invocations of one event serialized?
///   reentrancy   - SWS_CM_00009: concurrent calls of different member functions on one proxy event instance.
///   offer_hold   - SWS_CM_00102 (part 1): offer the service instance and keep it offered for a while.
///   offer_second - SWS_CM_00102 (part 2): a second process tries to create/offer the same service instance.

#include "score/mw/com/runtime.h"
#include "score/mw/com/test/common_test_resources/assert_handler.h"
#include "score/mw/com/test/common_test_resources/big_datatype.h"
#include "score/string_manipulation/arguments/arguments.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using score::mw::com::test::BigDataProxy;
using score::mw::com::test::BigDataSkeleton;
using score::mw::com::test::MapApiLanesStamped;
using namespace std::chrono_literals;

const std::string kInstanceSpecifier{"score/cp60/MapApiLanesStamped"};
constexpr std::size_t kSampleSubscriptionCount{3U};

score::mw::com::InstanceSpecifier MakeSpecifier()
{
    auto result = score::mw::com::InstanceSpecifier::Create(std::string{kInstanceSpecifier});
    if (!result.has_value())
    {
        std::cerr << "Invalid instance specifier, terminating." << std::endl;
        std::exit(EXIT_FAILURE);
    }
    return result.value();
}

bool CreateAndOffer(const score::mw::com::InstanceSpecifier& spec, std::unique_ptr<BigDataSkeleton>& out)
{
    auto skeleton_result = BigDataSkeleton::Create(spec);
    if (!skeleton_result.has_value())
    {
        std::cout << "PROBE_INFO create_skeleton=error code=" << skeleton_result.error().Message() << std::endl;
        return false;
    }
    out = std::make_unique<BigDataSkeleton>(std::move(skeleton_result).value());
    const auto offer_result = out->OfferService();
    if (!offer_result.has_value())
    {
        std::cout << "PROBE_INFO offer=error code=" << offer_result.error().Message() << std::endl;
        return false;
    }
    return true;
}

std::vector<BigDataProxy::HandleType> FindOnce(const score::mw::com::InstanceSpecifier& spec)
{
    auto handles = BigDataProxy::FindService(spec);
    if (!handles.has_value())
    {
        return {};
    }
    return std::vector<BigDataProxy::HandleType>(handles.value().begin(), handles.value().end());
}

std::unique_ptr<BigDataProxy> CreateProxy(const score::mw::com::InstanceSpecifier& spec)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        auto handles = FindOnce(spec);
        if (!handles.empty())
        {
            auto proxy_result = BigDataProxy::Create(handles.front());
            if (proxy_result.has_value())
            {
                return std::make_unique<BigDataProxy>(std::move(proxy_result).value());
            }
        }
        std::this_thread::sleep_for(100ms);
    }
    std::cerr << "Could not find/create proxy, terminating." << std::endl;
    std::exit(EXIT_FAILURE);
}

void UpdateMax(std::atomic<int>& max_value, int candidate)
{
    int current = max_value.load();
    while ((candidate > current) && !max_value.compare_exchange_weak(current, candidate))
    {
    }
}

int ProbeReceiveHandlerSerialization()
{
    const auto spec = MakeSpecifier();
    std::unique_ptr<BigDataSkeleton> skeleton;
    if (!CreateAndOffer(spec, skeleton))
    {
        return EXIT_FAILURE;
    }
    auto proxy = CreateProxy(spec);
    if (!proxy->map_api_lanes_stamped_.Subscribe(kSampleSubscriptionCount).has_value())
    {
        std::cerr << "Subscribe failed" << std::endl;
        return EXIT_FAILURE;
    }

    std::atomic<int> in_handler{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> invocations{0};
    const auto set_result = proxy->map_api_lanes_stamped_.SetReceiveHandler([&]() {
        const int now = ++in_handler;
        UpdateMax(max_concurrent, now);
        ++invocations;
        std::this_thread::sleep_for(5ms);  // widen the window in which a second invocation could overlap
        --in_handler;
    });
    if (!set_result.has_value())
    {
        std::cerr << "SetReceiveHandler failed" << std::endl;
        return EXIT_FAILURE;
    }

    const MapApiLanesStamped sample{};
    int send_errors = 0;
    for (int i = 0; i < 300; ++i)
    {
        if (!skeleton->map_api_lanes_stamped_.Send(sample).has_value())
        {
            ++send_errors;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(500ms);
    score::cpp::ignore = proxy->map_api_lanes_stamped_.UnsetReceiveHandler();
    proxy->map_api_lanes_stamped_.Unsubscribe();
    skeleton->StopOfferService();

    std::cout << "PROBE_RESULT SWS_CM_00182 max_concurrent_invocations=" << max_concurrent.load()
              << " invocations=" << invocations.load() << " sends=300 send_errors=" << send_errors
              << " verdict=" << (max_concurrent.load() <= 1 ? "serialized" : "NOT_serialized") << std::endl;
    return EXIT_SUCCESS;
}

int ProbeReentrancy()
{
    const auto spec = MakeSpecifier();
    std::unique_ptr<BigDataSkeleton> skeleton;
    if (!CreateAndOffer(spec, skeleton))
    {
        return EXIT_FAILURE;
    }
    auto proxy = CreateProxy(spec);
    auto& event = proxy->map_api_lanes_stamped_;
    if (!event.Subscribe(kSampleSubscriptionCount).has_value())
    {
        std::cerr << "Subscribe failed" << std::endl;
        return EXIT_FAILURE;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<long> calls{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&]() {
        const MapApiLanesStamped sample{};
        while (!stop)
        {
            score::cpp::ignore = skeleton->map_api_lanes_stamped_.Send(sample);
            std::this_thread::sleep_for(1ms);
        }
    });
    threads.emplace_back([&]() {  // GetNewSamples (only this thread)
        while (!stop)
        {
            auto r = event.GetNewSamples([](score::mw::com::SamplePtr<MapApiLanesStamped>) {}, 1U);
            if (!r.has_value())
            {
                ++errors;
            }
            ++calls;
        }
    });
    threads.emplace_back([&]() {  // GetSubscriptionState + GetFreeSampleCount
        while (!stop)
        {
            score::cpp::ignore = event.GetSubscriptionState();
            score::cpp::ignore = event.GetFreeSampleCount();
            ++calls;
        }
    });
    threads.emplace_back([&]() {  // GetNumNewSamplesAvailable
        while (!stop)
        {
            if (!event.GetNumNewSamplesAvailable().has_value())
            {
                ++errors;
            }
            ++calls;
        }
    });
    threads.emplace_back([&]() {  // Set/Unset receive handler concurrently with the above
        while (!stop)
        {
            score::cpp::ignore = event.SetReceiveHandler([]() {});
            std::this_thread::sleep_for(2ms);
            score::cpp::ignore = event.UnsetReceiveHandler();
            ++calls;
        }
    });
    std::this_thread::sleep_for(3s);
    stop = true;
    for (auto& t : threads)
    {
        t.join();
    }
    event.Unsubscribe();
    skeleton->StopOfferService();
    std::cout << "PROBE_RESULT SWS_CM_00009 completed=1 calls=" << calls.load() << " errors=" << errors.load()
              << " verdict=" << (errors.load() == 0 ? "no_errors" : "errors_reported") << std::endl;
    return EXIT_SUCCESS;
}

int ProbeOfferHold()
{
    const auto spec = MakeSpecifier();
    std::unique_ptr<BigDataSkeleton> skeleton;
    const bool ok = CreateAndOffer(spec, skeleton);
    std::cout << "PROBE_RESULT SWS_CM_00102 first_offer=" << (ok ? "ok" : "failed") << std::endl;
    if (!ok)
    {
        return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(8s);
    skeleton->StopOfferService();
    return EXIT_SUCCESS;
}

int ProbeOfferSecond()
{
    const auto spec = MakeSpecifier();
    std::unique_ptr<BigDataSkeleton> skeleton;
    const bool ok = CreateAndOffer(spec, skeleton);
    std::this_thread::sleep_for(500ms);
    const auto handles = FindOnce(spec);
    std::cout << "PROBE_RESULT SWS_CM_00102 second_offer=" << (ok ? "accepted" : "rejected")
              << " visible_instances=" << handles.size()
              << " verdict=" << (ok ? "duplicate_NOT_detected" : "duplicate_rejected") << std::endl;
    if (ok)
    {
        skeleton->StopOfferService();
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, const char** argv)
{
    score::mw::com::test::SetupAssertHandler();
    const std::string mode = (argc > 1) ? std::string{argv[1]} : std::string{};
    // Only argv[0] is handed to the runtime so that the probe selector is not interpreted as a runtime option.
    score::mw::com::runtime::InitializeRuntime(score::string_manipulation::GetArguments(1, argv));

    if (mode == "rh_serial")
    {
        return ProbeReceiveHandlerSerialization();
    }
    if (mode == "reentrancy")
    {
        return ProbeReentrancy();
    }
    if (mode == "offer_hold")
    {
        return ProbeOfferHold();
    }
    if (mode == "offer_second")
    {
        return ProbeOfferSecond();
    }
    std::cerr << "unknown mode: " << mode << std::endl;
    return EXIT_FAILURE;
}
