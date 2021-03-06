// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/tserver/twodc_output_client.h"

#include <shared_mutex>

#include "yb/cdc/cdc_util.h"
#include "yb/cdc/cdc_rpc.h"
#include "yb/client/client.h"
#include "yb/client/meta_cache.h"
#include "yb/gutil/strings/join.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/tserver/cdc_consumer.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/net/net_util.h"

DEFINE_test_flag(bool, twodc_write_hybrid_time_override, false,
  "Override external_hybrid_time with initialHybridTimeValue for testing.");

DECLARE_int32(cdc_write_rpc_timeout_ms);

DEFINE_bool(cdc_force_remote_tserver, false,
            "Avoid local tserver apply optimization for CDC and force remote RPCs.");
TAG_FLAG(cdc_force_remote_tserver, runtime);

DECLARE_int32(cdc_read_rpc_timeout_ms);

namespace yb {
namespace tserver {
namespace enterprise {

using rpc::Rpc;

class TwoDCOutputClient : public cdc::CDCOutputClient {
 public:
  TwoDCOutputClient(
      CDCConsumer* cdc_consumer,
      const cdc::ConsumerTabletInfo& consumer_tablet_info,
      const std::shared_ptr<client::YBClient>& local_client,
      std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk,
      bool use_local_tserver) :
      cdc_consumer_(cdc_consumer),
      consumer_tablet_info_(consumer_tablet_info),
      local_client_(local_client),
      apply_changes_clbk_(std::move(apply_changes_clbk)),
      use_local_tserver_(use_local_tserver) {}

  ~TwoDCOutputClient() = default;

  CHECKED_STATUS ApplyChanges(const cdc::GetChangesResponsePB* resp) override;

  void WriteCDCRecordDone(
      const Status& status, const WriteResponsePB& response, const size_t record_idx,
      rpc::Rpcs::Handle handle);

 private:
  void TabletLookupCallback(
      const size_t record_idx, const Result<client::internal::RemoteTabletPtr>& tablet);

  void TabletLookupCallbackFastTrack(const size_t record_idx);

  void WriteIfAllRecordsProcessed();

  void SendCDCWriteToTablet(const size_t record_idx);

  // Increment processed record count.
  // Returns true if all records are processed, false if there are still some pending records.
  bool IncProcessedRecordCount();

  void HandleResponse();
  void HandleError(const Status& s, const cdc::CDCRecordPB& bad_record, bool done);

  bool UseLocalTserver();

  CDCConsumer* cdc_consumer_;
  cdc::ConsumerTabletInfo consumer_tablet_info_;
  std::shared_ptr<client::YBClient> local_client_;
  std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk_;

  bool use_local_tserver_;

  std::shared_ptr<client::YBTable> table_;

  // Used to protect error_status_, op_id_, done_processing_ and record counts.
  mutable rw_spinlock lock_;
  Status error_status_ GUARDED_BY(lock_);
  OpIdPB op_id_ GUARDED_BY(lock_) = consensus::MinimumOpId();
  bool done_processing_ GUARDED_BY(lock_) = false;

  uint32_t processed_record_count_ GUARDED_BY(lock_) = 0;
  uint32_t record_count_ GUARDED_BY(lock_) = 0;

  // Map of CDCRecord index -> RemoteTablet.
  std::unordered_map<size_t, client::internal::RemoteTablet*> records_;

  // This will cache the response to an ApplyChanges() request.
  cdc::GetChangesResponsePB resp_;


};

Status TwoDCOutputClient::ApplyChanges(const cdc::GetChangesResponsePB* resp) {
  // ApplyChanges is called in a single threaded manner.
  // For all the changes in GetChangesResponsePB, we first fan out and find the tablet for
  // every record key.
  // Then we apply the records in the same order in which we received them.
  // Once all changes have been applied (successfully or not), we invoke the callback which will
  // then either poll for next set of changes (in case of successful application) or will try to
  // re-apply.
  DCHECK(resp->has_checkpoint());
  resp_.Clear();

  // Init class variables that threads will use.
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    DCHECK(consensus::OpIdEquals(op_id_, consensus::MinimumOpId()));
    op_id_ = resp->checkpoint().op_id();
    error_status_ = Status::OK();
    records_.clear();
    done_processing_ = false;
    processed_record_count_ = 0;
    record_count_ = resp->records_size();
  }

  // Ensure we have records.
  if (resp->records_size() == 0) {
    HandleResponse();
    return Status::OK();
  }

  // Ensure we have a connection to the consumer table cached.
  if (!table_) {
    Status s = local_client_->OpenTable(consumer_tablet_info_.table_id, &table_);
    if (!s.ok()) {
      cdc::OutputClientResponse response;
      response.status = s;
      apply_changes_clbk_(response);
      return s;
    }
  }

  // Inspect all records in the response.
  for (int i = 0; i < resp->records_size(); i++) {
    if (resp->records(i).key_size() == 0) {
      // Transaction status record, ignore for now.
      // Support for handling transactions will be added in future.
      IncProcessedRecordCount();
    } else {
      auto record_pb = resp_.add_records();
      *record_pb = resp->records(i);
    }
  }

  for (int i = 0; i < resp_.records_size(); i++) {
    // All KV-pairs within a single CDC record will be for the same row.
    // key(0).key() will contain the hash code for that row. We use this to lookup the tablet.
    if (UseLocalTserver()) {
      TabletLookupCallbackFastTrack(i);
    } else {
      local_client_->LookupTabletByKey(
          table_.get(),
          PartitionSchema::EncodeMultiColumnHashValue(
              boost::lexical_cast<uint16_t>(resp_.records(i).key(0).key())),
          CoarseMonoClock::now() + MonoDelta::FromMilliseconds(FLAGS_cdc_read_rpc_timeout_ms),
          std::bind(&TwoDCOutputClient::TabletLookupCallback, this, i, std::placeholders::_1));
    }
  }

  if (resp_.records_size() == 0) {
    // Nothing to process, return success.
    HandleResponse();
  }
  return Status::OK();
}

bool TwoDCOutputClient::UseLocalTserver() {
  return use_local_tserver_ && !FLAGS_cdc_force_remote_tserver;
}


void TwoDCOutputClient::WriteIfAllRecordsProcessed() {
  bool done = IncProcessedRecordCount();
  if (done) {

    // Found tablets for all records, now we should write the records.
    // But first, check if there were any errors during tablet lookup for any record.
    bool has_error = false;
    {
      std::lock_guard<decltype(lock_)> l(lock_);
      if (!error_status_.ok()) {
        has_error = true;
      }
    }

    if (has_error) {
      // Return error, if any, without applying records.
      HandleResponse();
    } else {
      // Apply the writes on consumer.
      SendCDCWriteToTablet(0);
    }
  }
}

void TwoDCOutputClient::TabletLookupCallback(
    const size_t record_idx,
    const Result<client::internal::RemoteTabletPtr>& tablet) {
  if (!tablet.ok()) {
    bool done = IncProcessedRecordCount();
    HandleError(tablet.status(), resp_.records(record_idx), done);
    return;
  }

  {
    std::lock_guard<decltype(lock_)> l(lock_);
    records_.emplace(record_idx, tablet->get());
  }

  WriteIfAllRecordsProcessed();
}

void TwoDCOutputClient::TabletLookupCallbackFastTrack(const size_t record_idx) {
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    // We don't need a remote tablet if we already know the tablet id, set the value in record_ to
    // nullptr.
    records_.emplace(record_idx, nullptr);
  }

  WriteIfAllRecordsProcessed();
}

void TwoDCOutputClient::SendCDCWriteToTablet(const size_t record_idx) {
  client::internal::RemoteTablet* tablet;
  {
    std::shared_lock<decltype(lock_)> l(lock_);
    tablet = records_[record_idx];
  }

  WriteRequestPB req;
  req.set_tablet_id(UseLocalTserver() ? consumer_tablet_info_.tablet_id : tablet->tablet_id());
  if (PREDICT_FALSE(FLAGS_twodc_write_hybrid_time_override)) {
    // Used only for testing external hybrid time.
    req.set_external_hybrid_time(yb::kInitialHybridTimeValue);
  } else {
    req.set_external_hybrid_time(resp_.records(record_idx).time());
  }

  for (const auto& kv_pair : resp_.records(record_idx).changes()) {
    auto* write_pair = req.mutable_write_batch()->add_write_pairs();
    write_pair->set_key(kv_pair.key());
    write_pair->set_value(kv_pair.value().binary_value());
  }

  auto deadline = CoarseMonoClock::Now() +
                  MonoDelta::FromMilliseconds(FLAGS_cdc_write_rpc_timeout_ms);
  auto write_rpc_handle = cdc_consumer_->rpcs()->Prepare();
  if (write_rpc_handle != cdc_consumer_->rpcs()->InvalidHandle()) {
    *write_rpc_handle = CreateCDCWriteRpc(
        deadline,
        tablet,
        local_client_.get(),
        &req,
        std::bind(&TwoDCOutputClient::WriteCDCRecordDone, this,
                  std::placeholders::_1, std::placeholders::_2, record_idx, write_rpc_handle),
        UseLocalTserver());
    (**write_rpc_handle).SendRpc();
  } else {
    LOG(WARNING) << "Invalid handle for CDC write, tablet ID: " << tablet->tablet_id();
  }
}

void TwoDCOutputClient::WriteCDCRecordDone(
    const Status& status, const WriteResponsePB& response, size_t record_idx,
    rpc::Rpcs::Handle handle) {
  auto retained = cdc_consumer_->rpcs()->Unregister(handle);
  if (!status.ok()) {
    HandleError(status, resp_.records(record_idx), true /* done */);
    return;
  } else if (response.has_error()) {
    HandleError(StatusFromPB(response.error().status()), resp_.records(record_idx),
                true /* done */);
    return;
  }

  if (record_idx == resp_.records_size() - 1) {
    // Last record, return response to caller.
    HandleResponse();
  } else {
    SendCDCWriteToTablet(record_idx + 1);
  }
}

void TwoDCOutputClient::HandleError(const Status& s, const cdc::CDCRecordPB& bad_record,
                                    bool done) {
  LOG(ERROR) << "Error while applying replicated record: " << s
             << ", consumer tablet: " << consumer_tablet_info_.tablet_id;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    error_status_ = s;
  }
  if (done) {
    HandleResponse();
  }
}

void TwoDCOutputClient::HandleResponse() {
  cdc::OutputClientResponse response;
  {
    std::lock_guard<decltype(lock_)> l(lock_);
    response.status = error_status_;
    if (response.status.ok()) {
      response.last_applied_op_id = op_id_;
    }
    op_id_ = consensus::MinimumOpId();
  }
  apply_changes_clbk_(response);
}

bool TwoDCOutputClient::IncProcessedRecordCount() {
  std::lock_guard<rw_spinlock> l(lock_);
  processed_record_count_++;
  if (processed_record_count_ == record_count_) {
    done_processing_ = true;
  }
  return done_processing_;
}

std::unique_ptr<cdc::CDCOutputClient> CreateTwoDCOutputClient(
    CDCConsumer* cdc_consumer,
    const cdc::ConsumerTabletInfo& consumer_tablet_info,
    const std::shared_ptr<client::YBClient>& local_client,
    std::function<void(const cdc::OutputClientResponse& response)> apply_changes_clbk,
    bool use_local_tserver) {
  return std::make_unique<TwoDCOutputClient>(cdc_consumer, consumer_tablet_info, local_client,
                                             std::move(apply_changes_clbk), use_local_tserver);
}

} // namespace enterprise
} // namespace tserver
} // namespace yb
