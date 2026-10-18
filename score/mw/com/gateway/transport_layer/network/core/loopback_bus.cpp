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
#include "score/mw/com/gateway/transport_layer/network/core/loopback_bus.h"

#include <utility>

namespace score::mw::com::gateway::network
{

std::pair<std::unique_ptr<LoopbackBus>, std::unique_ptr<LoopbackBus>> LoopbackBus::CreatePair()
{
    std::unique_ptr<LoopbackBus> a{new LoopbackBus{}};
    std::unique_ptr<LoopbackBus> b{new LoopbackBus{}};
    a->peer_ = b.get();
    b->peer_ = a.get();
    return {std::move(a), std::move(b)};
}

LoopbackBus::~LoopbackBus()
{
    Close();
}

BusStatus LoopbackBus::Open(FrameHandler on_frame)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (open_)
    {
        return BusStatus::kOk;
    }
    on_frame_ = std::move(on_frame);
    stop_ = false;
    open_ = true;
    receive_thread_ = std::thread{[this]() {
        ReceiveLoop();
    }};
    return BusStatus::kOk;
}

void LoopbackBus::Close()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_)
        {
            return;
        }
        open_ = false;
        stop_ = true;
    }
    cv_.notify_all();
    if (receive_thread_.joinable() && (receive_thread_.get_id() != std::this_thread::get_id()))
    {
        receive_thread_.join();
    }
}

void LoopbackBus::SetFaultHook(FaultHook hook)
{
    std::lock_guard<std::mutex> lock{hook_mutex_};
    fault_hook_ = std::move(hook);
}

BusStatus LoopbackBus::Publish(const std::uint8_t* data, std::size_t size)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_)
        {
            return BusStatus::kNotOpen;
        }
    }
    Fault fault{Fault::kDeliver};
    {
        std::lock_guard<std::mutex> lock{hook_mutex_};
        if (fault_hook_)
        {
            fault = fault_hook_(data, size);
        }
    }
    if (fault == Fault::kDrop)
    {
        return BusStatus::kOk;
    }
    std::vector<std::uint8_t> frame{data, data + size};
    if (fault == Fault::kDuplicate)
    {
        peer_->Enqueue(frame);
    }
    peer_->Enqueue(std::move(frame));
    return BusStatus::kOk;
}

void LoopbackBus::Enqueue(std::vector<std::uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_)
        {
            return;  // Peer not listening: frame is lost, like on a real network.
        }
        inbox_.push_back(std::move(frame));
    }
    cv_.notify_one();
}

void LoopbackBus::ReceiveLoop()
{
    while (true)
    {
        std::vector<std::uint8_t> frame{};
        {
            std::unique_lock<std::mutex> lock{mutex_};
            cv_.wait(lock, [this]() {
                return stop_ || !inbox_.empty();
            });
            if (stop_)
            {
                inbox_.clear();
                return;
            }
            frame = std::move(inbox_.front());
            inbox_.pop_front();
        }
        on_frame_(frame.data(), frame.size());
    }
}

}  // namespace score::mw::com::gateway::network
