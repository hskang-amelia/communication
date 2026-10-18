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
// One side of the two-process integration test. Replays the gateway message sequence over a real middleware:
//
//   source                                   destination
//     | -- ProvideService(svc, events) ------> |  (dispatcher sends a nested request back)
//     | <----- RegisterNotification(evt) ----- |
//     | -- EventSample x N (seq + checksum) -> |
//     | -- EventSample (1 MiB) --------------> |
//     | -- OfferService x K (latency probe) -> |
//     | -- StopOfferService -----------------> |  destination verifies and exits
//
// Prints one "RESULT {json}" line. Exit code 0 = pass.
#include "score/mw/com/gateway/transport_layer/network/core/rpc_channel.h"

#if defined(HAVE_ZENOH_PICO)
#include "score/mw/com/gateway/transport_layer/network/zenoh/zenoh_pico_bus.h"
#endif
#if defined(HAVE_CYCLONE_DDS)
#include "score/mw/com/gateway/transport_layer/network/dds/cyclone_dds_bus.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace net = score::mw::com::gateway::network;
using Clock = std::chrono::steady_clock;

namespace
{

struct Options
{
    std::string backend;
    std::string role;
    std::uint32_t samples{2000U};
    std::size_t sample_size{256U};
    std::size_t large_size{1U << 20U};
    std::uint32_t probes{200U};
    std::uint16_t port{17447U};
    std::uint32_t domain{42U};
};

bool ParseOptions(int argc, char** argv, Options& options)
{
    if (argc < 3)
    {
        return false;
    }
    options.backend = argv[1];
    options.role = argv[2];
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string key{argv[i]};
        const unsigned long value = std::strtoul(argv[i + 1], nullptr, 10);
        if (key == "--samples")
        {
            options.samples = static_cast<std::uint32_t>(value);
        }
        else if (key == "--size")
        {
            options.sample_size = static_cast<std::size_t>(value);
        }
        else if (key == "--large")
        {
            options.large_size = static_cast<std::size_t>(value);
        }
        else if (key == "--probes")
        {
            options.probes = static_cast<std::uint32_t>(value);
        }
        else if (key == "--port")
        {
            options.port = static_cast<std::uint16_t>(value);
        }
        else if (key == "--domain")
        {
            options.domain = static_cast<std::uint32_t>(value);
        }
        else
        {
            return false;
        }
    }
    return ((options.role == "source") || (options.role == "destination")) && (options.sample_size >= 8U);
}

std::unique_ptr<net::IMessageBus> CreateBus(const Options& options)
{
    const bool source = (options.role == "source");
#if defined(HAVE_ZENOH_PICO)
    if (options.backend == "zenoh")
    {
        net::ZenohPicoBusConfig config{};
        config.mode = "peer";
        const std::string endpoint = "tcp/127.0.0.1:" + std::to_string(options.port);
        // The destination listens, the source connects (and retries until the destination is up).
        (source ? config.connect : config.listen) = endpoint;
        config.local_key = source ? "score/gateway/test/source/inbox" : "score/gateway/test/destination/inbox";
        config.remote_key = source ? "score/gateway/test/destination/inbox" : "score/gateway/test/source/inbox";
        config.max_frame_size = net::ZenohPicoBus::LibraryMaxFrameSize();
        config.open_timeout = std::chrono::milliseconds{20000};
        return std::make_unique<net::ZenohPicoBus>(config);
    }
#endif
#if defined(HAVE_CYCLONE_DDS)
    if (options.backend == "dds")
    {
        net::CycloneDdsBusConfig config{};
        config.domain_id = options.domain;
        config.local_topic = source ? "score_gateway_test_source" : "score_gateway_test_destination";
        config.remote_topic = source ? "score_gateway_test_destination" : "score_gateway_test_source";
        config.wait_for_peer = std::chrono::milliseconds{0};
        return std::make_unique<net::CycloneDdsBus>(config);
    }
#endif
    return nullptr;
}

std::uint32_t Checksum(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t hash{2166136261U};
    for (std::size_t i = 0U; i < size; ++i)
    {
        hash = (hash ^ data[i]) * 16777619U;
    }
    return hash;
}

/// Sample layout: [u32 index][u32 checksum of the rest][filler...]
std::vector<std::uint8_t> MakeSample(std::uint32_t index, std::size_t size)
{
    std::vector<std::uint8_t> sample(size);
    for (std::size_t i = 8U; i < size; ++i)
    {
        sample[i] = static_cast<std::uint8_t>((i * 7U) ^ index);
    }
    const std::uint32_t checksum = Checksum(sample.data() + 8U, size - 8U);
    std::memcpy(sample.data(), &index, sizeof(index));
    std::memcpy(sample.data() + 4U, &checksum, sizeof(checksum));
    return sample;
}

bool VerifySample(const std::vector<std::uint8_t>& sample, std::uint32_t& index)
{
    if (sample.size() < 8U)
    {
        return false;
    }
    std::uint32_t checksum{0U};
    std::memcpy(&index, sample.data(), sizeof(index));
    std::memcpy(&checksum, sample.data() + 4U, sizeof(checksum));
    return checksum == Checksum(sample.data() + 8U, sample.size() - 8U);
}

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto position = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U));
    return values[position];
}

class Event
{
  public:
    void Set()
    {
        std::lock_guard<std::mutex> lock{mutex_};
        set_ = true;
        cv_.notify_all();
    }

    bool Wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock{mutex_};
        return cv_.wait_for(lock, timeout, [this]() {
            return set_;
        });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool set_{false};
};

int RunSource(net::RpcChannel& channel, const Options& options, Event& registered)
{
    const auto provide = channel.SendRequest(
        net::ProvideService{"/test/svc", {{"evt", options.sample_size, 8U, "Evt@1"}, {"large", options.large_size, 8U}}});
    if (provide != net::ChannelStatus::kOk)
    {
        std::printf("RESULT {\"role\":\"source\",\"ok\":false,\"error\":\"provide: %s\"}\n", net::ToString(provide));
        return 1;
    }
    if (!registered.Wait(std::chrono::milliseconds{10000}))
    {
        std::printf("RESULT {\"role\":\"source\",\"ok\":false,\"error\":\"no RegisterNotification\"}\n");
        return 1;
    }

    const auto stream_start = Clock::now();
    for (std::uint32_t i = 0U; i < options.samples; ++i)
    {
        const auto sample = MakeSample(i, options.sample_size);
        if (channel.SendEventSample("/test/svc", "evt", sample.data(), sample.size()) != net::ChannelStatus::kOk)
        {
            std::printf("RESULT {\"role\":\"source\",\"ok\":false,\"error\":\"sample %u\"}\n", i);
            return 1;
        }
    }
    const double stream_ms = std::chrono::duration<double, std::milli>(Clock::now() - stream_start).count();

    const auto large = MakeSample(0xFFFFFFFFU, options.large_size);
    const auto large_status = channel.SendEventSample("/test/svc", "large", large.data(), large.size());

    std::vector<double> latencies_us{};
    for (std::uint32_t i = 0U; i < options.probes; ++i)
    {
        const auto start = Clock::now();
        if (channel.SendRequest(net::OfferService{"/test/svc"}) != net::ChannelStatus::kOk)
        {
            std::printf("RESULT {\"role\":\"source\",\"ok\":false,\"error\":\"probe %u\"}\n", i);
            return 1;
        }
        latencies_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }

    const auto stop = channel.SendRequest(net::StopOfferService{"/test/svc"});
    const auto stats = channel.Statistics();
    const bool ok = (stop == net::ChannelStatus::kOk) && (large_status == net::ChannelStatus::kOk);
    std::printf(
        "RESULT {\"role\":\"source\",\"ok\":%s,\"stream_ms\":%.1f,\"samples\":%u,\"sample_size\":%zu,"
        "\"rtt_p50_us\":%.0f,\"rtt_p99_us\":%.0f,\"rtt_max_us\":%.0f,\"retries\":%llu,\"large\":\"%s\"}\n",
        ok ? "true" : "false",
        stream_ms,
        options.samples,
        options.sample_size,
        Percentile(latencies_us, 0.50),
        Percentile(latencies_us, 0.99),
        Percentile(latencies_us, 1.0),
        static_cast<unsigned long long>(stats.retries),
        net::ToString(large_status));
    return ok ? 0 : 1;
}

struct DestinationState
{
    std::mutex mutex;
    bool provided{false};
    std::uint32_t received{0U};
    std::uint32_t next_index{0U};
    std::uint32_t corrupt{0U};
    std::uint32_t out_of_order{0U};
    bool large_ok{false};
    std::uint32_t offers{0U};
    net::ChannelStatus nested{net::ChannelStatus::kNotStarted};
    Clock::time_point first_sample{};
    Clock::time_point last_sample{};
};

}  // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!ParseOptions(argc, argv, options))
    {
        std::fprintf(stderr,
                     "usage: bus_peer <zenoh|dds> <source|destination> [--samples N] [--size B] [--large B] "
                     "[--probes N] [--port P] [--domain D]\n");
        return 2;
    }
    auto bus = CreateBus(options);
    if (!bus)
    {
        std::fprintf(stderr, "backend '%s' not built\n", options.backend.c_str());
        return 2;
    }

    net::RpcChannelConfig config{};
    config.ack_timeout = std::chrono::milliseconds{200};
    config.max_attempts = 50U;  // Covers the peer starting a few seconds later.
    config.max_dispatch_queue = 1U << 16U;
    net::RpcChannel channel{*bus, config, net::RpcChannel::GenerateSessionId()};

    Event registered{};
    Event stopped{};
    DestinationState state{};
    const bool source = (options.role == "source");

    const auto start_status = channel.Start([&](const net::Frame& frame) {
        if (source)
        {
            if (std::holds_alternative<net::RegisterNotification>(frame.body))
            {
                registered.Set();
            }
            return;
        }
        std::unique_lock<std::mutex> lock{state.mutex};
        if (const auto* provide = std::get_if<net::ProvideService>(&frame.body))
        {
            state.provided = (provide->events.size() == 2U) && (provide->events[0].name == "evt") &&
                             (provide->events[0].type_id == "Evt@1") && provide->events[1].type_id.empty();
            lock.unlock();
            // Nested request while the source is still blocked in SendRequest(ProvideService).
            const auto nested = channel.SendRequest(net::RegisterNotification{
                {"/test/svc", net::WireElementKind::kEvent, "evt"}});
            lock.lock();
            state.nested = nested;
        }
        else if (const auto* sample = std::get_if<net::EventSample>(&frame.body))
        {
            std::uint32_t index{0U};
            const bool valid = VerifySample(sample->payload, index);
            if (sample->element == "large")
            {
                state.large_ok = valid && (index == 0xFFFFFFFFU) && (sample->payload.size() == options.large_size);
                return;
            }
            if (state.received == 0U)
            {
                state.first_sample = Clock::now();
            }
            state.last_sample = Clock::now();
            ++state.received;
            state.corrupt += valid ? 0U : 1U;
            state.out_of_order += (index == state.next_index) ? 0U : 1U;
            state.next_index = index + 1U;
        }
        else if (std::holds_alternative<net::OfferService>(frame.body))
        {
            ++state.offers;
        }
        else if (std::holds_alternative<net::StopOfferService>(frame.body))
        {
            stopped.Set();
        }
    });
    if (start_status != net::ChannelStatus::kOk)
    {
        std::printf("RESULT {\"role\":\"%s\",\"ok\":false,\"error\":\"start: %s\"}\n",
                    options.role.c_str(),
                    net::ToString(start_status));
        return 1;
    }

    int exit_code{0};
    if (source)
    {
        exit_code = RunSource(channel, options, registered);
    }
    else
    {
        const bool got_stop = stopped.Wait(std::chrono::milliseconds{60000});
        std::lock_guard<std::mutex> lock{state.mutex};
        const auto stats = channel.Statistics();
        const double stream_ms =
            std::chrono::duration<double, std::milli>(state.last_sample - state.first_sample).count();
        const bool ok = got_stop && state.provided && (state.nested == net::ChannelStatus::kOk) &&
                        (state.received == options.samples) && (state.corrupt == 0U) &&
                        (state.out_of_order == 0U) && state.large_ok && (state.offers == options.probes);
        std::printf(
            "RESULT {\"role\":\"destination\",\"ok\":%s,\"received\":%u,\"expected\":%u,\"corrupt\":%u,"
            "\"out_of_order\":%u,\"large_ok\":%s,\"offers\":%u,\"nested\":\"%s\",\"receive_ms\":%.1f,"
            "\"duplicates_dropped\":%llu,\"decode_failures\":%llu,\"queue_overflows\":%llu}\n",
            ok ? "true" : "false",
            state.received,
            options.samples,
            state.corrupt,
            state.out_of_order,
            state.large_ok ? "true" : "false",
            state.offers,
            net::ToString(state.nested),
            stream_ms,
            static_cast<unsigned long long>(stats.duplicates_dropped),
            static_cast<unsigned long long>(stats.decode_failures),
            static_cast<unsigned long long>(stats.queue_overflows));
        exit_code = ok ? 0 : 1;
    }
    std::fflush(stdout);
    channel.Stop();
    return exit_code;
}
