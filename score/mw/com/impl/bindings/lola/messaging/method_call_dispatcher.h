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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_CALL_DISPATCHER_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_CALL_DISPATCHER_H

#include "score/mw/com/impl/bindings/lola/messaging/method_reply.h"

#include <score/callback.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace score::mw::com::impl::lola
{

/// \brief Runs method call handlers on its own threads instead of the message passing thread (issue #767).
///
/// The message passing thread only queues the call. A handler may then call other services' methods, or wait for a
/// thread that does, because the replies to those calls are delivered by the message passing thread, which is free.
///
/// `base_threads` handlers run at a time; with 1 they run one at a time in arrival order, as they did on the message
/// passing thread. A handler that makes a blocking method call lends its slot to a spare thread for that time, so a
/// call that comes back into this process (A calls B, B calls A) is served. Up to `max_threads` threads exist; when a
/// blocking call would leave none to serve, it is refused instead of hanging.
///
/// The queue and all threads are created up front; nothing is allocated afterwards.
class MethodCallDispatcher final
{
  public:
    /// The job that runs a handler; called with false when it is rejected instead of run. It owns the reply, so a job
    /// that is dropped without being called still answers the caller.
    using Job = score::cpp::callback<void(bool run), 192U>;

    MethodCallDispatcher(std::size_t base_threads, std::size_t max_threads, std::size_t queue_capacity) noexcept;
    ~MethodCallDispatcher() noexcept;

    MethodCallDispatcher(const MethodCallDispatcher&) = delete;
    MethodCallDispatcher& operator=(const MethodCallDispatcher&) = delete;
    MethodCallDispatcher(MethodCallDispatcher&&) = delete;
    MethodCallDispatcher& operator=(MethodCallDispatcher&&) = delete;

    /// \return false if the queue is full or the dispatcher is stopping; the job is then left with the caller.
    bool Submit(Job& job) noexcept;

    /// \brief Stops taking jobs, drops the queued ones and joins the threads. Running jobs finish first.
    void Stop() noexcept;

    /// \brief The dispatcher whose thread is calling, or nullptr.
    static MethodCallDispatcher* Current() noexcept;

    /// \brief Called by a handler's thread before it blocks on a method call.
    /// \return false if no thread would be left to serve calls; the caller must not block then.
    bool EnterBlocking() noexcept;
    void LeaveBlocking() noexcept;

  private:
    void Run() noexcept;
    bool CanTakeJob() const noexcept;

    std::mutex mutex_{};
    std::condition_variable condition_{};
    std::vector<Job> queue_;
    std::size_t head_{0U};
    std::size_t size_{0U};
    std::size_t base_threads_;
    std::size_t max_threads_;
    std::size_t running_{0U};  ///< threads inside a job
    std::size_t blocked_{0U};  ///< of those, threads waiting for a method call they made
    bool stopping_{false};
    std::vector<std::thread> threads_{};
};

/// \brief Marks the calling dispatcher thread as blocked for its lifetime (no-op on other threads).
class BlockingCallScope final
{
  public:
    BlockingCallScope() noexcept;
    ~BlockingCallScope() noexcept;
    BlockingCallScope(const BlockingCallScope&) = delete;
    BlockingCallScope& operator=(const BlockingCallScope&) = delete;
    BlockingCallScope(BlockingCallScope&&) = delete;
    BlockingCallScope& operator=(BlockingCallScope&&) = delete;

    /// \brief false if the call must be refused because it would leave no thread to serve calls.
    bool Allowed() const noexcept
    {
        return allowed_;
    }

  private:
    MethodCallDispatcher* dispatcher_;
    bool allowed_{true};
};

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_MESSAGING_METHOD_CALL_DISPATCHER_H
