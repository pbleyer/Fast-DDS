// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * RTPSReader.cpp
 *
 */

#include <fastdds/rtps/reader/RTPSReader.h>
#include <fastdds/rtps/history/ReaderHistory.h>
#include <fastdds/dds/log/Log.hpp>
#include <rtps/reader/ReaderHistoryState.hpp>

#include <fastdds/rtps/reader/ReaderListener.h>
#include <fastdds/rtps/resources/ResourceEvent.h>
#include <fastrtps_deprecated/participant/ParticipantImpl.h>

#include <foonathan/memory/namespace_alias.hpp>

#include <typeinfo>
#include <algorithm>
#include <chrono>

namespace eprosima {
namespace fastrtps {
namespace rtps {

RTPSReader::RTPSReader(
        RTPSParticipantImpl* pimpl,
        const GUID_t& guid,
        const ReaderAttributes& att,
        ReaderHistory* hist,
        ReaderListener* rlisten)
    : Endpoint(pimpl, guid, att.endpoint)
    , mp_history(hist)
    , mp_listener(rlisten)
    , m_acceptMessagesToUnknownReaders(true)
    , m_acceptMessagesFromUnkownWriters(false)
    , m_expectsInlineQos(att.expectsInlineQos)
    , history_state_(new ReaderHistoryState(att.matched_writers_allocation.initial))
    , liveliness_kind_(att.liveliness_kind_)
    , liveliness_lease_duration_(att.liveliness_lease_duration)
{
    mp_history->mp_reader = this;
    mp_history->mp_mutex = &mp_mutex;

    logInfo(RTPS_READER, "RTPSReader created correctly");
}

RTPSReader::~RTPSReader()
{
    logInfo(RTPS_READER, "Removing reader " << this->getGuid().entityId; );
    delete history_state_;
    mp_history->mp_reader = nullptr;
    mp_history->mp_mutex = nullptr;
}

bool RTPSReader::reserveCache(
        CacheChange_t** change,
        uint32_t dataCdrSerializedSize)
{
    return mp_history->reserve_Cache(change, dataCdrSerializedSize);
}

void RTPSReader::releaseCache(
        CacheChange_t* change)
{
    return mp_history->release_Cache(change);
}

ReaderListener* RTPSReader::getListener() const
{
    return mp_listener;
}

bool RTPSReader::setListener(
        ReaderListener* target)
{
    mp_listener = target;
    return true;
}

History::const_iterator RTPSReader::findCacheInFragmentedProcess(
        const SequenceNumber_t& sequence_number,
        const GUID_t& writer_guid,
        CacheChange_t** change,
        History::const_iterator hint) const
{
    History::const_iterator ret_val = mp_history->get_change_nts(sequence_number, writer_guid, change, hint);

    if (nullptr != *change && (*change)->is_fully_assembled())
    {
        *change = nullptr;
    }

    return ret_val;
}

void RTPSReader::add_persistence_guid(
        const GUID_t& guid,
        const GUID_t& persistence_guid)
{
    GUID_t persistence_guid_to_store = (c_Guid_Unknown == persistence_guid) ? guid : persistence_guid;
    std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
    history_state_->persistence_guid_map[guid] = persistence_guid_to_store;
    history_state_->persistence_guid_count[persistence_guid_to_store]++;
}

bool RTPSReader::may_remove_history_record(
        bool removed_by_lease)
{
    return !removed_by_lease;
}

void RTPSReader::remove_persistence_guid(
        const GUID_t& guid,
        const GUID_t& persistence_guid,
        bool removed_by_lease)
{
    std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
    GUID_t persistence_guid_stored = (c_Guid_Unknown == persistence_guid) ? guid : persistence_guid;
    history_state_->persistence_guid_map.erase(guid);
    auto count = --history_state_->persistence_guid_count[persistence_guid_stored];
    if (count <= 0 && may_remove_history_record(removed_by_lease))
    {
        history_state_->history_record.erase(persistence_guid_stored);
        history_state_->persistence_guid_count.erase(persistence_guid_stored);
    }
}

SequenceNumber_t RTPSReader::update_last_notified(
        const GUID_t& guid,
        const SequenceNumber_t& seq)
{
    SequenceNumber_t ret_val;
    std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
    GUID_t guid_to_look = guid;
    auto p_guid = history_state_->persistence_guid_map.find(guid);
    if (p_guid != history_state_->persistence_guid_map.end())
    {
        guid_to_look = p_guid->second;
    }

    auto p_seq = history_state_->history_record.find(guid_to_look);
    if (p_seq != history_state_->history_record.end())
    {
        ret_val = p_seq->second;
    }

    if (ret_val < seq)
    {
        set_last_notified(guid_to_look, seq);
        new_notification_cv_.notify_all();
    }

    return ret_val;
}

SequenceNumber_t RTPSReader::get_last_notified(
        const GUID_t& guid)
{
    SequenceNumber_t ret_val;
    std::lock_guard<RecursiveTimedMutex> guard(mp_mutex);
    GUID_t guid_to_look = guid;
    auto p_guid = history_state_->persistence_guid_map.find(guid);
    if (p_guid != history_state_->persistence_guid_map.end())
    {
        guid_to_look = p_guid->second;
    }

    auto p_seq = history_state_->history_record.find(guid_to_look);
    if (p_seq != history_state_->history_record.end())
    {
        ret_val = p_seq->second;
    }

    return ret_val;
}

void RTPSReader::set_last_notified(
        const GUID_t& peristence_guid,
        const SequenceNumber_t& seq)
{
    history_state_->history_record[peristence_guid] = seq;
}

bool RTPSReader::wait_for_unread_cache(
        const eprosima::fastrtps::Duration_t& timeout)
{
    auto time_out = std::chrono::steady_clock::now() + std::chrono::seconds(timeout.seconds) +
            std::chrono::nanoseconds(timeout.nanosec);

    std::unique_lock<RecursiveTimedMutex> lock(mp_mutex, std::defer_lock);

    if (lock.try_lock_until(time_out))
    {
        if (new_notification_cv_.wait_until(lock, time_out, [&]()
                    {
                        return total_unread_ > 0;
                    }))
        {
            return true;
        }
    }

    return false;
}

uint64_t RTPSReader::get_unread_count() const
{
    std::unique_lock<RecursiveTimedMutex> lock(mp_mutex);
    return total_unread_;
}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
