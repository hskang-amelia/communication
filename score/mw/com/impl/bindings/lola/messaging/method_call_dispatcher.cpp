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
#include "score/mw/com/impl/bindings/lola/messaging/method_call_dispatcher.h"

#include <algorithm>

namespace score::mw::com::impl::lola
{

namespace
{
thread_local MethodCallDispatcher* current_dispatcher{nullptr};
}

MethodCallDispatcher::MethodCallDispatcher(const std::size_t base_threads,
                                           const std::size_t max_threads,
                                           const std::size_t queue_capacity) noexcept
    : queue_(std::max<std::size_t>(queue_capacity, 1U)),
      base_threads_{std::max<std::size_t>(base_threads, 1U)},
      max_threads_{std::max(max_threads, std::max<std::size_t>(base_threads, 1U))}
{
    threads_.reserve(max_threads_);
    for (std::size_t i = 0U; i < max_threads_; ++i)
    {
        threads_.emplace_back([this]() noexcept {
            Run();
        });
    }
}

MethodCallDispatcher::~MethodCallDispatcher() noexcept
{
    Stop();
}

bool MethodCallDispatcher::Submit(Job& job) noexcept
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_ || (size_ == queue_.size()))
        {
            return false;
        }
        queue_[(head_ + size_) % queue_.size()] = std::move(job);
        ++size_;
    }
    condition_.notify_all();
    return true;
}

void MethodCallDispatcher::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& thread : threads_)
    {
        if (thread.joinable() && (thread.get_id() != std::this_thread::get_id()))
        {
            thread.join();
        }
    }
    // Queued jobs are destroyed unrun; each one's MethodReply then answers kSkeletonAlreadyDestroyed.
    std::lock_guard<std::mutex> lock{mutex_};
    for (; size_ > 0U; --size_)
    {
        queue_[head_] = Job{};
        head_ = (head_ + 1U) % queue_.size();
    }
}

MethodCallDispatcher* MethodCallDispatcher::Current() noexcept
{
    return current_dispatcher;
}

bool MethodCallDispatcher::EnterBlocking() noexcept
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if ((blocked_ + 1U) >= max_threads_)
        {
            return false;
        }
        ++blocked_;
    }
    condition_.notify_all();
    return true;
}

void MethodCallDispatcher::LeaveBlocking() noexcept
{
    std::lock_guard<std::mutex> lock{mutex_};
    --blocked_;
}

bool MethodCallDispatcher::CanTakeJob() const noexcept
{
    return (size_ > 0U) && ((running_ - blocked_) < base_threads_);
}

void MethodCallDispatcher::Run() noexcept
{
    current_dispatcher = this;
    for (;;)
    {
        Job job{};
        {
            std::unique_lock<std::mutex> lock{mutex_};
            condition_.wait(lock, [this]() {
                return stopping_ || CanTakeJob();
            });
            if (stopping_)
            {
                return;
            }
            job = std::move(queue_[head_]);
            queue_[head_] = Job{};
            head_ = (head_ + 1U) % queue_.size();
            --size_;
            ++running_;
        }
        job(true);
        job = Job{};
        {
            std::lock_guard<std::mutex> lock{mutex_};
            --running_;
        }
        condition_.notify_all();
    }
}

BlockingCallScope::BlockingCallScope() noexcept : dispatcher_{MethodCallDispatcher::Current()}
{
    if (dispatcher_ != nullptr)
    {
        allowed_ = dispatcher_->EnterBlocking();
    }
}

BlockingCallScope::~BlockingCallScope() noexcept
{
    if ((dispatcher_ != nullptr) && allowed_)
    {
        dispatcher_->LeaveBlocking();
    }
}

}  // namespace score::mw::com::impl::lola
