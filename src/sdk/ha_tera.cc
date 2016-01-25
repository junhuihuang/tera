#include "sdk/mutate_impl.h"
#include "sdk/read_impl.h"
#include "sdk/client_impl.h"
#include "sdk/table_impl.h"
#include "sdk/ha_tera.h"
#include "utils/timer.h"
#include "sdk/callback_check.h"

DECLARE_bool(tera_sdk_ha_ddl_enable);
DECLARE_int32(tera_sdk_ha_timestamp_diff);
DECLARE_bool(tera_sdk_ha_get_random_mode);

namespace tera {

class PutCallbackChecker : public CallChecker {
public:
    PutCallbackChecker(const std::vector<TableImpl*> &clusters, RowMutationImpl* row_mutate)
        : _has_call(false),
          _cluster_index(0),
          _clusters(clusters),
          _row_mutate(row_mutate),
          _failed_count(0) {}

    virtual ~PutCallbackChecker() {}

    // 对于异步写，直到顺序写完所有的tera集群，才调用callback
    bool NeedCall(ErrorCode::ErrorCodeType code) {
        if (_has_call) {
            return false;
        } else {
            if (++_cluster_index >= _clusters.size()) {
                if (_row_mutate->GetError().GetType() != ErrorCode::kOK) {
                    LOG(WARNING) << "Async put failed! reason:" << _row_mutate->GetError().GetReason()
                                 << " at tera:" << (_cluster_index-1);
                    _failed_count++;

                    // 所有集群写完后，只要有一个集群成功，则认为成功
                    if (_failed_count < _clusters.size()) {
                        _row_mutate->Reset();
                    }
                }
                _has_call = true;
                return true;
            } else {
                if (_row_mutate->GetError().GetType() != ErrorCode::kOK) {
                    LOG(WARNING) << "Async put failed! reason:" << _row_mutate->GetError().GetReason()
                                 << " at tera:" << (_cluster_index-1);
                    _failed_count++;
                }
                _row_mutate->Reset();
                _clusters[_cluster_index]->ApplyMutation(_row_mutate);
                return false;
            }
        }
    }

private:
    bool _has_call;
    size_t _cluster_index;
    std::vector<TableImpl*> _clusters;
    RowMutationImpl* _row_mutate;
    uint32_t _failed_count;
};

class GetCallbackChecker : public CallChecker {
public:
    GetCallbackChecker(const std::vector<TableImpl*> &clusters, RowReaderImpl* row_reader)
        : _has_call(false),
          _cluster_index(0),
          _clusters(clusters),
          _row_reader(row_reader) {}

    virtual ~GetCallbackChecker() {}

    // 对于异步读，如果读失败，则尝试从下一个集群读；
    // 否则，返回true
    bool NeedCall(ErrorCode::ErrorCodeType code) {
        if (_has_call) {
            return false;
        } else if (code == ErrorCode::kOK) {
            _has_call = true;
            return true;
        } else {
            if (++_cluster_index >= _clusters.size()) {
                _has_call = true;
                return true;
            } else {
                _row_reader->Reset();
                _clusters[_cluster_index]->Get(_row_reader);
                return false;
            }
        }
    }

private:
    bool _has_call;
    size_t _cluster_index;
    std::vector<TableImpl*> _clusters;
    RowReaderImpl* _row_reader;
};

class LGetCallbackChecker : public CallChecker {
public:
    LGetCallbackChecker(const std::vector<TableImpl*> &clusters, RowReaderImpl* row_reader)
        : _has_call(false),
          _cluster_index(0),
          _clusters(clusters),
          _row_reader(row_reader) {}

    virtual ~LGetCallbackChecker() {}

    /// 比较两个集群，选择时间戳比较大的结果
    bool NeedCall(ErrorCode::ErrorCodeType code) {
        if (_has_call) {
            return false;
        } else if (code == ErrorCode::kOK) {
            _results.push_back(_row_reader->GetResult());
        }
        if (++_cluster_index >= _clusters.size()) {
            // 合并结果
            if (_results.size() > 0) {
                RowResult final_result;
                HATableImpl::MergeResult(_results, final_result, _row_reader->GetMaxVersions());
                _row_reader->SetResult(final_result);
            }
            _has_call = true;
            return true;
        } else {
            _row_reader->Reset();
            _clusters[_cluster_index]->Get(_row_reader);
            return false;
        }
    }

private:
    bool _has_call;
    size_t _cluster_index;
    std::vector<TableImpl*> _clusters;
    RowReaderImpl* _row_reader;
    std::vector<RowResult> _results;
};

void HATableImpl::Add(TableImpl *t) {
    _tables.push_back(t);
}

RowMutation* HATableImpl::NewRowMutation(const std::string& row_key) {
    return new RowMutationImpl(NULL, row_key);
}

RowReader* HATableImpl::NewRowReader(const std::string& row_key) {
    return new RowReaderImpl(NULL, row_key);
}

void HATableImpl::ApplyMutation(RowMutation* row_mu) {
    size_t failed_count = 0;

    RowMutationImpl* row_mu_impl = dynamic_cast<RowMutationImpl*>(row_mu);
    // 如果是异步操作，则设置回调的检查器
    if (row_mu->IsAsync()) {
        if (_tables.size() > 0) {
            row_mu_impl->SetCallChecker(new PutCallbackChecker(_tables, row_mu_impl));
            _tables[0]->ApplyMutation(row_mu);
        }
    } else {
        for (size_t i = 0; i < _tables.size(); i++) {
            _tables[i]->ApplyMutation(row_mu);
            if (row_mu->GetError().GetType() != ErrorCode::kOK) {
                failed_count++;
                LOG(WARNING) << "ApplyMutation failed! "
                             << row_mu->GetError().GetType()
                             << " at tera:" << i;
            }
            // 如果所有集群都失败了，则认为失败
            if (failed_count < _tables.size()) {
                // 重置除用户数据外的数据，以用于后面的写
                row_mu_impl->Reset();
            }
        }
    }
}

void HATableImpl::ApplyMutation(const std::vector<RowMutation*>& row_mu_list) {
    std::vector<size_t> failed_count_list;
    failed_count_list.resize(row_mu_list.size());

    std::vector<RowMutation*> async_mu;
    std::vector<RowMutation*> sync_mu;
    for (size_t i = 0; i < row_mu_list.size(); i++) {
        if (row_mu_list[i]->IsAsync()) {
            async_mu.push_back(row_mu_list[i]);
        } else {
            sync_mu.push_back(row_mu_list[i]);
        }
    }

    // 处理异步写
    for (size_t i = 0; i < async_mu.size(); i++) {
        ApplyMutation(async_mu[i]);
    }

    if (sync_mu.size() <= 0) {
        return;
    }

    // 处理同步写
    for (size_t i = 0; i < _tables.size(); i++) {
        _tables[i]->ApplyMutation(sync_mu);
        for (size_t j = 0; j < sync_mu.size(); j++) {
            RowMutation* row_mu = sync_mu[j];
            if (row_mu->GetError().GetType() != ErrorCode::kOK) {
                LOG(WARNING) << j << " ApplyMutation failed! "
                             << row_mu->GetError().GetType()
                             << " at tera:" << i;
                failed_count_list[i]++;
            }
            // 如果所有集群都失败了，则认为失败
            if (failed_count_list[i] < _tables.size()) {
                // 重置除用户数据外的数据，以用于后面的写
                RowMutationImpl* row_mu_impl = dynamic_cast<RowMutationImpl*>(row_mu);
                row_mu_impl->Reset();
            }
        }
    }
}

bool HATableImpl::Put(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const std::string& value,
                      ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Put(row_key, family, qualifier, value, err);
        if (!ok) {
            LOG(WARNING) << "Put failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::Put(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const int64_t value,
                      ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Put(row_key, family, qualifier, value, err);
        if (!ok) {
            LOG(WARNING) << "Put failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::Put(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const std::string& value,
                      int32_t ttl, ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Put(row_key, family, qualifier, value, ttl, err);
        if (!ok) {
            LOG(WARNING) << "Put failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::Put(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, const std::string& value,
                      int64_t timestamp, int32_t ttl, ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Put(row_key, family, qualifier, value, timestamp, ttl, err);
        if (!ok) {
            LOG(WARNING) << "Put failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::Add(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, int64_t delta,
                      ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Add(row_key, family, qualifier, delta, err);
        if (!ok) {
            LOG(WARNING) << "Add failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::AddInt64(const std::string& row_key, const std::string& family,
                           const std::string& qualifier, int64_t delta,
                           ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->AddInt64(row_key, family, qualifier, delta, err);
        if (!ok) {
            LOG(WARNING) << "AddInt64 failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::PutIfAbsent(const std::string& row_key,
                              const std::string& family,
                              const std::string& qualifier,
                              const std::string& value,
                              ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->PutIfAbsent(row_key, family, qualifier, value, err);
        if (!ok) {
            LOG(WARNING) << "PutIfAbsent failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HATableImpl::Append(const std::string& row_key, const std::string& family,
                         const std::string& qualifier, const std::string& value,
                         ErrorCode* err) {
    size_t failed_count = 0;
    for (size_t i = 0; i < _tables.size(); i++) {
        bool ok = _tables[i]->Append(row_key, family, qualifier, value, err);
        if (!ok) {
            LOG(WARNING) << "Append failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        }
    }

    if (failed_count >= _tables.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

// 从两个集群里获取时间戳比较新的数据, latest-get
void HATableImpl::LGet(RowReader* row_reader) {
    size_t failed_count = 0;

    RowReaderImpl* row_reader_impl = dynamic_cast<RowReaderImpl*>(row_reader);
    // 如果是异步操作，则设置回调的检查器
    if (row_reader_impl->IsAsync()) {
        row_reader_impl->SetCallChecker(new LGetCallbackChecker(_tables, row_reader_impl));
        if (_tables.size() > 0) {
            _tables[0]->Get(row_reader);
        }
    } else {
        // 同步Get
        std::vector<RowResult> results;
        for (size_t i = 0; i < _tables.size(); i++) {
            _tables[i]->Get(row_reader);
            if (row_reader_impl->GetError().GetType() != ErrorCode::kOK) {
                LOG(WARNING) << "Get failed! " << row_reader_impl->GetError().GetReason()
                             << " at tera:" << i;
                failed_count++;
                if (failed_count < _tables.size()) {
                    row_reader_impl->Reset();
                }
            } else {
                results.push_back(row_reader_impl->GetResult());
                row_reader_impl->Reset();
            }
        }
        if (results.size() > 0) {
            RowResult final_result;
            HATableImpl::MergeResult(results, final_result, row_reader_impl->GetMaxVersions());
            row_reader_impl->SetResult(final_result);
        }
    }
}

void HATableImpl::LGet(const std::vector<RowReader*>& row_readers) {
    for (size_t i = 0; i < row_readers.size(); i++) {
        LGet(row_readers[i]);
    }
}

void HATableImpl::Get(RowReader* row_reader) {
    size_t failed_count = 0;

    std::vector<TableImpl*> table_set = _tables;
    // 如果是随机Get，则每次对tables进行排序
    if (FLAGS_tera_sdk_ha_get_random_mode) {
        HATableImpl::ShuffleArray(table_set);
    }

    RowReaderImpl* row_reader_impl = dynamic_cast<RowReaderImpl*>(row_reader);
    // 如果是异步操作，则设置回调的检查器
    if (row_reader->IsAsync()) {
        if (table_set.size() > 0) {
            row_reader_impl->SetCallChecker(new GetCallbackChecker(table_set, row_reader_impl));
            table_set[0]->Get(row_reader);
        }
    } else {
        // 同步Get
        for (size_t i = 0; i < table_set.size(); i++) {
            table_set[i]->Get(row_reader);
            if (row_reader->GetError().GetType() != ErrorCode::kOK) {
                LOG(WARNING) << "Get failed! " << row_reader->GetError().GetReason()
                             << " at tera:" << i;
                failed_count++;
            } else {
                break;
            }
            if (failed_count < table_set.size()) {
                row_reader_impl->Reset();
            }
        }
    }
}

// 可能有一批数据来自集群1，另一批数据来自集群2
void HATableImpl::Get(const std::vector<RowReader*>& row_readers) {

    std::vector<TableImpl*> table_set = _tables;
    // 如果是随机Get，则每次对tables进行排序
    if (FLAGS_tera_sdk_ha_get_random_mode) {
        HATableImpl::ShuffleArray(table_set);
    }

    std::vector<RowReader*> async_readers;
    std::vector<RowReader*> sync_readers;
    for (size_t i = 0; i < row_readers.size(); i++) {
        if (row_readers[i]->IsAsync()) {
            async_readers.push_back(row_readers[i]);
        } else {
            sync_readers.push_back(row_readers[i]);
        }
    }

    // 处理异步读
    for (size_t i = 0; i < async_readers.size(); i++) {
        Get(async_readers[i]);
    }

    if (sync_readers.size() <= 0) {
        return;
    }

    // 处理同步读
    std::vector<size_t> failed_count_list;
    failed_count_list.resize(sync_readers.size());

    for (size_t i = 0; i < table_set.size(); i++) {
        if (sync_readers.size() <= 0) {
            continue;
        }
        std::vector<RowReader*> need_read = sync_readers;
        table_set[i]->Get(need_read);
        sync_readers.clear();
        for (size_t j = 0; j < need_read.size(); j++) {
            RowReader* row_reader = need_read[j];
            if (row_reader->GetError().GetType() != ErrorCode::kOK) {
                LOG(WARNING) << j << " Get failed! error: "
                             << row_reader->GetError().GetType()
                             << ", " << row_reader->GetError().GetReason()
                             << " at tera:" << i;
                failed_count_list[j] += 1;

                // 如果所有集群都失败了，则认为失败
                if (failed_count_list[j] < table_set.size()) {
                    // 重置除用户数据外的数据，以用于后面的写
                    RowReaderImpl* row_reader_impl = dynamic_cast<RowReaderImpl*>(row_reader);
                    row_reader_impl->Reset();
                    sync_readers.push_back(row_reader);
                }
            } //fail
        }
    }
}

bool HATableImpl::Get(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, std::string* value,
                      ErrorCode* err, uint64_t snapshot_id) {

    std::vector<TableImpl*> table_set = _tables;
    // 如果是随机Get，则每次对tables进行排序
    if (FLAGS_tera_sdk_ha_get_random_mode) {
        HATableImpl::ShuffleArray(table_set);
    }

    size_t failed_count = 0;
    for (size_t i = 0; i < table_set.size(); i++) {
        bool ok = table_set[i]->Get(row_key, family, qualifier, value, err, snapshot_id);
        if (!ok) {
            LOG(WARNING) << "Get failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            break;
        }
    }
    return (failed_count>=table_set.size()) ? false : true;
}

bool HATableImpl::Get(const std::string& row_key, const std::string& family,
                      const std::string& qualifier, int64_t* value,
                      ErrorCode* err, uint64_t snapshot_id) {

    std::vector<TableImpl*> table_set = _tables;
    // 如果是随机Get，则每次对tables进行排序
    if (FLAGS_tera_sdk_ha_get_random_mode) {
        HATableImpl::ShuffleArray(table_set);
    }

    size_t failed_count = 0;
    for (size_t i = 0; i < table_set.size(); i++) {
        bool ok = table_set[i]->Get(row_key, family, qualifier, value, err, snapshot_id);
        if (!ok) {
            LOG(WARNING) << "Get failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            break;
        }
    }
    return (failed_count>=table_set.size()) ? false : true;
}

bool HATableImpl::IsPutFinished() {
    for (size_t i = 0; i < _tables.size(); i++) {
        if (!_tables[i]->IsPutFinished()) {
            return false;
        }
    }
    return true;
}

bool HATableImpl::IsGetFinished() {
    for (size_t i = 0; i < _tables.size(); i++) {
        if (!_tables[i]->IsGetFinished()) {
            return false;
        }
    }
    return true;
}

ResultStream* HATableImpl::Scan(const ScanDescriptor& desc, ErrorCode* err) {
    for (size_t i = 0; i < _tables.size(); i++) {
        ResultStream* rs = _tables[i]->Scan(desc, err);
        if (rs == NULL) {
            LOG(WARNING) << "Scan failed! " << err->GetReason() << " at tera:" << i;
        } else {
            return rs;
        }
    }
    return NULL;
}

const std::string HATableImpl::GetName() {
    for (size_t i = 0; i < _tables.size(); i++) {
        return _tables[i]->GetName();
    }
    return "";
}

bool HATableImpl::Flush() {
    return false;
}

bool HATableImpl::CheckAndApply(const std::string& rowkey, const std::string& cf_c,
                                const std::string& value, const RowMutation& row_mu,
                                ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

int64_t HATableImpl::IncrementColumnValue(const std::string& row, const std::string& family,
                                          const std::string& qualifier, int64_t amount,
                                          ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return 0L;
}

void HATableImpl::SetWriteTimeout(int64_t timeout_ms) {
    for (size_t i = 0; i < _tables.size(); i++) {
        _tables[i]->SetWriteTimeout(timeout_ms);
    }
}

void HATableImpl::SetReadTimeout(int64_t timeout_ms) {
    for (size_t i = 0; i < _tables.size(); i++) {
        _tables[i]->SetReadTimeout(timeout_ms);
    }
}

bool HATableImpl::LockRow(const std::string& rowkey, RowLock* lock, ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

bool HATableImpl::GetStartEndKeys(std::string* start_key, std::string* end_key,
                                  ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

bool HATableImpl::GetTabletLocation(std::vector<TabletInfo>* tablets,
                                    ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

bool HATableImpl::GetDescriptor(TableDescriptor* desc, ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

void HATableImpl::SetMaxMutationPendingNum(uint64_t max_pending_num) {
    for (size_t i = 0; i < _tables.size(); i++) {
        _tables[i]->SetMaxMutationPendingNum(max_pending_num);
    }
}

void HATableImpl::SetMaxReaderPendingNum(uint64_t max_pending_num) {
    for (size_t i = 0; i < _tables.size(); i++) {
        _tables[i]->SetMaxReaderPendingNum(max_pending_num);
    }
}

Table* HATableImpl::GetClusterHandle(size_t i) {
    return (i < _tables.size()) ? _tables[i] : NULL;
}

void HATableImpl::MergeResult(const std::vector<RowResult>& results, RowResult& res, uint32_t max_size) {
    std::vector<int> results_pos;
    results_pos.resize(results.size());
    for (uint32_t i = 0; i < results.size(); i++) {
        results_pos[i] = 0;
    }
    for (uint32_t i = 0; i < max_size; i++) {
        // 获取时间戳最大的结果
        bool found = false;
        uint32_t candidate_index;
        int64_t timestamp;
        for (uint32_t j = 0; j < results.size(); j++) {
            if (results_pos[j] < results[j].key_values_size()) {
                int64_t tmp_ts = results[j].key_values(results_pos[j]).timestamp();
                if (!found || tmp_ts > timestamp) {
                    timestamp = tmp_ts;
                    candidate_index = j;
                    found = true;
                }
            }
        }
        if (!found) {
            break;
        }
        // 移动数组下标
        for (uint32_t j = 0; j < results.size() && j != candidate_index; j++) {
            if (results_pos[j] < results[j].key_values_size()) {
                int64_t tmp_ts = results[j].key_values(results_pos[j]).timestamp();
                // 时间戳相近，说明是同一批次
                if (abs(timestamp-tmp_ts) < FLAGS_tera_sdk_ha_timestamp_diff) {
                    results_pos[j] += 1;
                }
            }
        }
        // 保存结果
        KeyValuePair* kv = res.add_key_values();
        *kv = results[candidate_index].key_values(results_pos[candidate_index]);
        results_pos[candidate_index] += 1;
    }
}
void HATableImpl::ShuffleArray(std::vector<TableImpl*>& table_set) {
    int64_t seed = get_micros();
    srandom(seed);
    for (size_t i = table_set.size()-1; i > 0; i--) {
        int rnd = random()%(i+1);

        // swap
        TableImpl* t = table_set[rnd];
        table_set[rnd] = table_set[i];
        table_set[i] = t;
    }
}

HAClientImpl::HAClientImpl(const std::string& user_identity,
                           const std::string& user_passcode,
                           const std::vector<std::string>& zk_clusters,
                           const std::vector<std::string>& zk_paths) {
    assert(zk_clusters.size() == zk_paths.size());

    for (size_t i = 0; i < zk_clusters.size(); i++) {
        ClientImpl* ci = new ClientImpl(user_identity, user_passcode,
                                        zk_clusters[i], zk_paths[i]);
        _clients.push_back(ci);
    }
}

HAClientImpl::~HAClientImpl() {
    for (size_t i = 0; i < _clients.size(); i++) {
        delete _clients[i];
    }
    _clients.clear();
}

bool HAClientImpl::CreateTable(const TableDescriptor& desc, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->CreateTable(desc, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "CreateTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "CreateTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::CreateTable(const TableDescriptor& desc,
                               const std::vector<std::string>& tablet_delim,
                               ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->CreateTable(desc, tablet_delim, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "CreateTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "CreateTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::UpdateTable(const TableDescriptor& desc, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->UpdateTable(desc, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "UpdateTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "UpdateTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::DeleteTable(std::string name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->DeleteTable(name, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "DeleteTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "DeleteTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::DisableTable(std::string name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->DisableTable(name, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "DisableTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "DisableTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::EnableTable(std::string name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->EnableTable(name, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "EnableTable failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "EnableTable failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::CreateUser(const std::string& user,
                              const std::string& password, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->CreateUser(user, password, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "CreateUser failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "CreateUser failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}


bool HAClientImpl::DeleteUser(const std::string& user, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->DeleteUser(user, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "DeleteUser failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "DeleteUser failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::ChangePwd(const std::string& user,
                             const std::string& password, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->ChangePwd(user, password, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "ChangePwd failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "ChangePwd failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::ShowUser(const std::string& user, std::vector<std::string>& user_groups,
                            ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->ShowUser(user, user_groups, err);
        if (!ok) {
            LOG(WARNING) << "ShowUser failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::AddUserToGroup(const std::string& user,
                                  const std::string& group, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->AddUserToGroup(user, group, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "AddUserToGroup failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "AddUserToGroup failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::DeleteUserFromGroup(const std::string& user,
                                       const std::string& group, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->DeleteUserFromGroup(user, group, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "DeleteUserFromGroup failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "DeleteUserFromGroup failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

Table* HAClientImpl::OpenTable(const std::string& table_name, ErrorCode* err) {
    size_t failed_count = 0;
    HATableImpl* ha_table = NULL;
    for (size_t i = 0; i < _clients.size(); i++) {
        Table* t = _clients[i]->OpenTable(table_name, err);
        TableImpl* ti = dynamic_cast<TableImpl*>(t);
        if (ti == NULL) {
            LOG(WARNING) << "OpenTable failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            if (ha_table == NULL) {
                ha_table = new HATableImpl();
            }
            ha_table->Add(ti);
        }
    }

    if (failed_count >= _clients.size()) {
        return NULL;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return ha_table;
    }
}

bool HAClientImpl::GetTabletLocation(const std::string& table_name,
                                     std::vector<TabletInfo>* tablets,
                                     ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->GetTabletLocation(table_name, tablets, err);
        if (!ok) {
            LOG(WARNING) << "GetTabletLocation failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

TableDescriptor* HAClientImpl::GetTableDescriptor(const std::string& table_name,
                                                  ErrorCode* err) {
    TableDescriptor* td = NULL;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        td = _clients[i]->GetTableDescriptor(table_name, err);
        if (td == NULL) {
            LOG(WARNING) << "GetTableDescriptor failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            break;
        }
    }

    return (failed_count >= _clients.size()) ? NULL : td;
}

bool HAClientImpl::List(std::vector<TableInfo>* table_list,
                        ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->List(table_list, err);
        if (!ok) {
            LOG(WARNING) << "List failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}


bool HAClientImpl::List(const std::string& table_name,
                        TableInfo* table_info,
                        std::vector<TabletInfo>* tablet_list,
                        ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->List(table_name, table_info, tablet_list, err);
        if (!ok) {
            LOG(WARNING) << "List failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::IsTableExist(const std::string& table_name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->IsTableExist(table_name, err);
        if (!ok) {
            LOG(WARNING) << "IsTableExist failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::IsTableEnabled(const std::string& table_name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->IsTableEnabled(table_name, err);
        if (!ok) {
            LOG(WARNING) << "IsTableEnabled failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::IsTableEmpty(const std::string& table_name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->IsTableEmpty(table_name, err);
        if (!ok) {
            LOG(WARNING) << "IsTableEmpty failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::GetSnapshot(const std::string& name, uint64_t* snapshot, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->GetSnapshot(name, snapshot, err);
        if (!ok) {
            LOG(WARNING) << "GetSnapshot failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            // 对于读操作，只要一个成功就行
            break;
        }
    }

    return (failed_count >= _clients.size()) ? false : true;
}

bool HAClientImpl::DelSnapshot(const std::string& name, uint64_t snapshot,ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->DelSnapshot(name, snapshot, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "DelSnapshot failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "DelSnapshot failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::Rollback(const std::string& name, uint64_t snapshot,
                            const std::string& rollback_name, ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool  fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->Rollback(name, snapshot, rollback_name, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "AddUserToGroup failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "Rollback failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

// command分为safemode, tablet, meta, reload config
// arg_list: safemode的参数可以是enter,leave, get
// tablet命令的参数可以:move, split, merge
// meta的命令参数:backup
// reload config无参数
// 所有这些命令都操作需要操作所有的tera集群
// bool_result和str_result用第一个成功返回的结果
bool HAClientImpl::CmdCtrl(const std::string& command,
                           const std::vector<std::string>& arg_list,
                           bool* bool_result,
                           std::string* str_result,
                           ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool t_bool_result;
    std::string t_str_result;
    bool has_set = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->CmdCtrl(command, arg_list, &t_bool_result, &t_str_result, err);
        if (!ok) {
            LOG(WARNING) << "CmdCtrl failed! " << err->GetReason() << " at tera:" << i;
            failed_count++;
        } else {
            if (!has_set) {
                has_set = true;
                *bool_result = t_bool_result;
                *str_result = t_str_result;
            }
        }
    }

    if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

bool HAClientImpl::Rename(const std::string& old_table_name,
                          const std::string& new_table_name,
                          ErrorCode* err) {
    bool ok = false;
    size_t failed_count = 0;
    bool fail_fast = false;
    for (size_t i = 0; i < _clients.size(); i++) {
        ok = _clients[i]->Rename(old_table_name, new_table_name, err);
        if (!ok) {
            if (FLAGS_tera_sdk_ha_ddl_enable) {
                LOG(ERROR) << "AddUserToGroup failed! " << err->GetReason()
                           << " at tera:" << i << ", STOP try other cluster!";
                fail_fast = true;
                break;
            } else {
                LOG(WARNING) << "Rename failed! " << err->GetReason() << " at tera:" << i;
                failed_count++;
            }
        }
    }

    if (fail_fast) {
        return false;
    } else if (failed_count >= _clients.size()) {
        return false;
    } else {
        err->SetFailed(ErrorCode::kOK, "success");
        return true;
    }
}

Client* HAClientImpl::GetClusterClient(size_t i) {
    return (i < _clients.size()) ? _clients[i] : NULL;
}
}

