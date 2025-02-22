/* ========================= eCAL LICENSE =================================
 *
 * Copyright (C) 2016 - 2019 Continental Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================= eCAL LICENSE =================================
*/

/**
 * @brief  udp multicast reader and layer
**/

#include "ecal_reader_udp_mc.h"

#include "ecal_global_accessors.h"
#include "pubsub/ecal_subgate.h"

#include "io/udp/udp_configurations.h"

namespace eCAL
{
  ////////////////
  // READER
  ////////////////
  bool CDataReaderUDP::HasSample(const std::string& sample_name_)
  {
    if (g_subgate() == nullptr) return(false);
    return(g_subgate()->HasSample(sample_name_));
  }

  bool CDataReaderUDP::ApplySample(const eCAL::pb::Sample& ecal_sample_, eCAL::pb::eTLayerType layer_)
  {
    if (g_subgate() == nullptr) return false;
    return g_subgate()->ApplySample(ecal_sample_, layer_);
  }

  ////////////////
  // LAYER
  ////////////////
  CUDPReaderLayer::CUDPReaderLayer() : 
                   started(false),
                   local_mode(false)
  {}

  CUDPReaderLayer::~CUDPReaderLayer()
  {
    thread.Stop();
  }

  void CUDPReaderLayer::Initialize()
  {
    // set local mode
    local_mode = !Config::IsNetworkEnabled();

    // set network attributes
    SReceiverAttr attr;
    attr.address   = UDP::GetPayloadAddress();
    attr.port      = UDP::GetPayloadPort();
    attr.broadcast = !Config::IsNetworkEnabled();
    attr.loopback  = true;
    attr.rcvbuf    = Config::GetUdpMulticastRcvBufSizeBytes();

    // create udp receiver
    rcv.Create(attr);
  }

  void CUDPReaderLayer::AddSubscription(const std::string& /*host_name_*/, const std::string& topic_name_, const std::string& /*topic_id_*/, QOS::SReaderQOS /*qos_*/)
  {
    if (!started)
    {
      thread.Start(0, std::bind(&CDataReaderUDP::Receive, &reader, &rcv, CMN_PAYLOAD_RECEIVE_THREAD_CYCLE_TIME_MS));
      started = true;
    }

    // we use udp broadcast in local mode
    if (local_mode) return;

    // add topic name based multicast address
    const std::string mcast_address = UDP::GetTopicPayloadAddress(topic_name_);
    if (topic_name_mcast_map.find(mcast_address) == topic_name_mcast_map.end())
    {
      topic_name_mcast_map.emplace(std::pair<std::string, int>(mcast_address, 0));
      rcv.AddMultiCastGroup(mcast_address.c_str());
    }
    topic_name_mcast_map[mcast_address]++;
  }

  void CUDPReaderLayer::RemSubscription(const std::string& /*host_name_*/, const std::string& topic_name_, const std::string& /*topic_id_*/)
  {
    // we use udp broadcast in local mode
    if (local_mode) return;

    const std::string mcast_address = UDP::GetTopicPayloadAddress(topic_name_);
    if (topic_name_mcast_map.find(mcast_address) == topic_name_mcast_map.end())
    {
      // this should never happen
    }
    else
    {
      topic_name_mcast_map[mcast_address]--;
      if (topic_name_mcast_map[mcast_address] == 0)
      {
        rcv.RemMultiCastGroup(mcast_address.c_str());
        topic_name_mcast_map.erase(mcast_address);
      }
    }
  }
}
