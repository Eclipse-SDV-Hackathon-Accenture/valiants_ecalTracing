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
 * @brief  tcp writer
**/

#ifdef _MSC_VER
#pragma warning(push, 0) // disable proto warnings
#endif
#include <ecal/core/pb/layer.pb.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "config/ecal_config_reader_hlp.h"

#include <ecal/ecal_config.h>

#include "ecal_writer_tcp.h"
#include "ecal_tcp_pubsub_logger.h"

#include "ecal_utils/portable_endian.h"

namespace eCAL
{
  std::mutex                            CDataWriterTCP::g_tcp_writer_executor_mtx;
  std::shared_ptr<tcp_pubsub::Executor> CDataWriterTCP::g_tcp_writer_executor;

  CDataWriterTCP::CDataWriterTCP() : m_port(0)
  {
  }

  CDataWriterTCP::~CDataWriterTCP()
  {
    Destroy();
  }

  SWriterInfo CDataWriterTCP::GetInfo()
  {
    SWriterInfo info_;

    info_.name                 = "tcp";
    info_.description          = "tcp data writer";

    info_.has_mode_local       = true;
    info_.has_mode_cloud       = true;

    info_.has_qos_history_kind = false;
    info_.has_qos_reliability  = false;

    info_.send_size_max        = -1;

    return info_;
  }

  bool CDataWriterTCP::Create(const std::string& host_name_, const std::string& topic_name_, const std::string& topic_id_)
  {
    {
      const std::lock_guard<std::mutex> lock(g_tcp_writer_executor_mtx);
      if (!g_tcp_writer_executor)
      {
        g_tcp_writer_executor = std::make_shared<tcp_pubsub::Executor>(Config::GetTcpPubsubWriterThreadpoolSize(), TcpPubsubLogger);
      }
    }

    // create publisher
    m_publisher = std::make_shared<tcp_pubsub::Publisher>(g_tcp_writer_executor);
    m_port      = m_publisher->getPort();

    // writer parameter
    m_host_name  = host_name_;
    m_topic_name = topic_name_;
    m_topic_id   = topic_id_;

    return true;
  }

  bool CDataWriterTCP::Destroy()
  {
    if(!m_publisher) return false;

    // destroy publisher
    m_publisher = nullptr;
    m_port      = 0;

    return true;
  }

  bool CDataWriterTCP::Write(const void* const buf_, const SWriterAttr& attr_)
  {
    if (!m_publisher) return false;

    // create new sample (header information only, no payload)
    m_ecal_header.Clear();
    auto *ecal_sample_mutable_topic = m_ecal_header.mutable_topic();
    ecal_sample_mutable_topic->set_tname(m_topic_name);
    ecal_sample_mutable_topic->set_tid(m_topic_id);

    // append payload header (without payload)
    auto *ecal_sample_mutable_content = m_ecal_header.mutable_content();
    ecal_sample_mutable_content->set_id(attr_.id);
    ecal_sample_mutable_content->set_clock(attr_.clock);
    ecal_sample_mutable_content->set_time(attr_.time);
    ecal_sample_mutable_content->set_hash(attr_.hash);
    ecal_sample_mutable_content->set_size((google::protobuf::int32)attr_.len);

    // Initialize the padding with 1 element. It just must not be empty right now.
    m_ecal_header.set_padding(std::string("a"));

    // Compute size of "ECAL" pre-header
    constexpr size_t ecal_magic_size(4 * sizeof(char));

    // Get size of un-altered proto header size
#if GOOGLE_PROTOBUF_VERSION >= 3001000
    uint16_t proto_header_size = static_cast<uint16_t>(m_ecal_header.ByteSizeLong());
#else
    uint16_t proto_header_size = static_cast<uint16_t>(m_ecal_header.ByteSize());
#endif

    // Compute needed padding for aligning the payload
    constexpr size_t alignment_bytes     = 8;
    const     size_t minimal_header_size = ecal_magic_size +  sizeof(uint16_t)    +  proto_header_size;
    const     size_t padding_size        = (alignment_bytes - (minimal_header_size % alignment_bytes)) % alignment_bytes;

    // Add more bytes to the protobuf message to blow it up to the alignment
    // Aligning the user payload this way should be 100% compatible with previous
    // versions. It's most certainly bad style though and we should improve this 
    // in eCAL 6.
    // 
    // TODO: REMOVE ME FOR ECAL6
    m_ecal_header.set_padding(std::string(padding_size + 1, 'a'));
#if GOOGLE_PROTOBUF_VERSION >= 3001000
    proto_header_size = (uint16_t)m_ecal_header.ByteSizeLong();
#else
    proto_header_size = (uint16_t)m_ecal_header.ByteSize();
#endif

    //                     ECAL            +  header size field +  proto header
    m_header_buffer.resize(ecal_magic_size + sizeof(uint16_t)   +  proto_header_size);

    // add size
    *reinterpret_cast<uint16_t*>(&m_header_buffer[ecal_magic_size]) = htole16(proto_header_size);

    // serialize header message right after size field
    m_ecal_header.SerializeToArray((void*)(m_header_buffer.data() + ecal_magic_size + sizeof(uint16_t)), (int)proto_header_size);

    // add magic ecal header :-)
    m_header_buffer[0] = 'E';
    m_header_buffer[1] = 'C';
    m_header_buffer[2] = 'A';
    m_header_buffer[3] = 'L';

    // create tcp send buffer
    std::vector<std::pair<const char* const, const size_t>> send_vec;
    send_vec.reserve(2);

    // push header data
    send_vec.emplace_back(m_header_buffer.data(), m_header_buffer.size());
    // push payload data
    send_vec.emplace_back(static_cast<const char*>(buf_), attr_.len);

    // send it
    const bool success = m_publisher->send(send_vec);

    // return success
    return success;
  }

  std::string CDataWriterTCP::GetConnectionParameter()
  {
    // set tcp port
    eCAL::pb::ConnnectionPar connection_par;
    connection_par.mutable_layer_par_tcp()->set_port(m_port);
    return connection_par.SerializeAsString();
  }
}
