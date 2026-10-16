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
#ifndef SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_DDS_CYCLONE_DDS_BUS_H
#define SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_DDS_CYCLONE_DDS_BUS_H

#include "score/mw/com/gateway/transport_layer/network/core/message_bus.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace score::mw::com::gateway::network
{

struct CycloneDdsBusConfig
{
    std::uint32_t domain_id{0U};
    /// \brief Topic this gateway reads (its inbox), e.g. "score_gateway_hpc".
    std::string local_topic;
    /// \brief Topic of the peer gateway's inbox.
    std::string remote_topic;
    /// \brief KEEP_LAST depth of reader and writer. Bounds the burst of samples buffered per side.
    std::int32_t history_depth{256};
    /// \brief Max time a reliable write blocks when the reader's history is full.
    std::chrono::milliseconds max_blocking_time{1000};
    /// \brief Optional Cyclone DDS XML configuration (network interface, peers, ...). Empty: CYCLONEDDS_URI or
    /// Cyclone defaults.
    std::string cyclone_config_xml;
    /// \brief If > 0, Open() waits up to this long for the peer's reader and writer to be matched. Requests sent
    /// before matching are lost and only recovered by RpcChannel retries.
    std::chrono::milliseconds wait_for_peer{0};
};

/// \brief IMessageBus on Cyclone DDS (C API): one reader on local_topic, one writer on remote_topic, both of type
/// score_gateway::Frame (sequence<octet>), RELIABLE / VOLATILE / KEEP_LAST(history_depth).
class CycloneDdsBus final : public IMessageBus
{
  public:
    explicit CycloneDdsBus(CycloneDdsBusConfig config);
    ~CycloneDdsBus() override;

    BusStatus Open(FrameHandler on_frame) override;
    BusStatus Publish(const std::uint8_t* data, std::size_t size) override;
    void Close() override;

  private:
    friend struct CycloneDdsBusAccess;

    void TakeAll(std::int32_t reader);
    void ReleaseEntities() noexcept;

    const CycloneDdsBusConfig config_;
    FrameHandler on_frame_;

    // dds_entity_t values; 0 = not created.
    std::int32_t domain_{0};
    std::int32_t participant_{0};
    std::int32_t reader_{0};
    std::int32_t writer_{0};

    std::mutex lifecycle_mutex_;
    std::mutex publish_mutex_;
    std::mutex callback_mutex_;
    std::atomic<bool> open_{false};
};

}  // namespace score::mw::com::gateway::network

#endif  // SCORE_MW_COM_GATEWAY_TRANSPORT_LAYER_NETWORK_DDS_CYCLONE_DDS_BUS_H
