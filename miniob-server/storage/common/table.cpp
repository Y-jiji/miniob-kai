/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its
affiliates. All rights reserved. miniob is licensed under Mulan PSL v2. You can
use this software according to the terms and conditions of the Mulan PSL v2. You
may obtain a copy of Mulan PSL v2 at: http://license.coscl.org.cn/MulanPSL2 THIS
SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Wangyunlai on 2021/5/13.
//

#include <algorithm>
#include <limits.h>
#include <string.h>

#include <common/defs.h>
#include <common/lang/string.h>
#include <common/log/log.h>
#include <storage/common/bplus_tree_index.h>
#include <storage/common/condition_filter.h>
#include <storage/common/index.h>
#include <storage/common/meta_util.h>
#include <storage/common/record_manager.h>
#include <storage/common/table.h>
#include <storage/common/table_meta.h>
#include <storage/default/disk_buffer_pool.h>
#include <storage/transaction/transaction.h>

Table::Table()
    : data_buffer_pool_(nullptr), file_id_(-1), record_handler_(nullptr) {}

Table::~Table() {
    if (record_handler_ != nullptr) {
        delete record_handler_;
        record_handler_ = nullptr;
    }

    if (data_buffer_pool_ != nullptr && file_id_ >= 0) {
        data_buffer_pool_->close_file(file_id_);
        data_buffer_pool_ = nullptr;
    }

    for (std::vector<Index*>::iterator it = indexes_.begin();
         it != indexes_.end(); ++it) {
        Index* index = *it;
        delete index;
    }
    indexes_.clear();

    LOG_INFO("Table has been closed: %s", name());
}

ResultCode Table::create(const char* path, const char* name, const char* base_dir,
                 int attribute_count, const AttrInfo attributes[]) {

    if (common::is_blank(name)) {
        LOG_WARN("Name cannot be empty");
        return ResultCode::INVALID_ARGUMENT;
    }
    LOG_INFO("Begin to create table %s:%s", base_dir, name);

    if (attribute_count <= 0 || nullptr == attributes) {
        LOG_WARN("Invalid arguments. table_name=%s, attribute_count=%d, "
                 "attributes=%p",
                 name, attribute_count, attributes);
        return ResultCode::INVALID_ARGUMENT;
    }

    ResultCode rc = ResultCode::SUCCESS;

    // 使用 table_name.table记录一个表的元数据
    // 判断表文件是否已经存在
    int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (EEXIST == errno) {
            LOG_ERROR("Failed to create table file, it has been created. %s, "
                      "EEXIST, %s",
                      path, strerror(errno));
            return ResultCode::SCHEMA_TABLE_EXIST;
        }
        LOG_ERROR("Create table file failed. filename=%s, errmsg=%d:%s", path,
                  errno, strerror(errno));
        return ResultCode::IOERR;
    }

    close(fd);

    // 创建文件
    if ((rc = table_meta_.init(name, attribute_count, attributes)) !=
        ResultCode::SUCCESS) {
        LOG_ERROR("Failed to init table meta. name:%s, ret:%d", name, rc);
        return rc; // delete table file
    }

    std::fstream fs;
    fs.open(path, std::ios_base::out | std::ios_base::binary);
    if (!fs.is_open()) {
        LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s",
                  path, strerror(errno));
        return ResultCode::IOERR;
    }

    // 记录元数据到文件中
    table_meta_.serialize(fs);
    fs.close();

    std::string data_file = table_data_file(base_dir, name);
    data_buffer_pool_     = theGlobalDiskBufferPool();
    rc                    = data_buffer_pool_->create_file(data_file.c_str());
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR(
            "Failed to create disk buffer pool of data file. file name=%s",
            data_file.c_str());
        return rc;
    }

    rc = init_record_handler(base_dir);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR(
            "Failed to create table %s due to init record handler failed.",
            data_file.c_str());
        // don't need to remove the data_file
        return rc;
    }

    base_dir_ = base_dir;
    LOG_INFO("Successfully create table %s:%s", base_dir, name);
    return rc;
}

ResultCode Table::destroy(const char* dir) {
    // 刷新所有脏页
    ResultCode rc = sync();
    if (rc != ResultCode::SUCCESS)
        return rc;

    // TODO 删除描述表元数据的文件

    // TODO 删除表数据文件

    // TODO 清理所有的索引相关文件数据与索引元数据

    return ResultCode::GENERIC_ERROR;
}

ResultCode Table::open(const char* meta_file, const char* base_dir) {
    // 加载元数据文件
    std::fstream fs;
    std::string  meta_file_path =
        std::string(base_dir) + common::FILE_PATH_SPLIT_STR + meta_file;
    fs.open(meta_file_path, std::ios_base::in | std::ios_base::binary);
    if (!fs.is_open()) {
        LOG_ERROR("Failed to open meta file for read. file name=%s, errmsg=%s",
                  meta_file_path.c_str(), strerror(errno));
        return ResultCode::IOERR;
    }
    if (table_meta_.deserialize(fs) < 0) {
        LOG_ERROR("Failed to deserialize table meta. file name=%s",
                  meta_file_path.c_str());
        fs.close();
        return ResultCode::GENERIC_ERROR;
    }
    fs.close();

    // 加载数据文件
    ResultCode rc = init_record_handler(base_dir);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to open table %s due to init record handler failed.",
                  base_dir);
        // don't need to remove the data_file
        return rc;
    }

    base_dir_           = base_dir;

    const int index_num = table_meta_.index_num();
    for (int i = 0; i < index_num; i++) {
        const IndexMeta* index_meta = table_meta_.index(i);
        const FieldMeta* field_meta = table_meta_.field(index_meta->field());
        if (field_meta == nullptr) {
            LOG_ERROR("Found invalid index meta info which has a non-exists "
                      "field. table=%s, index=%s, field=%s",
                      name(), index_meta->name(), index_meta->field());
            // skip cleanup
            //  do all cleanup action in destructive Table function
            return ResultCode::GENERIC_ERROR;
        }

        BplusTreeIndex* index = new BplusTreeIndex();
        std::string     index_file =
            table_index_file(base_dir, name(), index_meta->name());
        rc = index->open(index_file.c_str(), *index_meta, *field_meta);
        if (rc != ResultCode::SUCCESS) {
            delete index;
            LOG_ERROR(
                "Failed to open index. table=%s, index=%s, file=%s, rc=%d:%s",
                name(), index_meta->name(), index_file.c_str(), rc, strrc(rc));
            // skip cleanup
            //  do all cleanup action in destructive Table function.
            return rc;
        }
        indexes_.push_back(index);
    }
    return rc;
}

ResultCode Table::commit_insert(Transaction* transaction, const RID& rid) {
    Record record;
    ResultCode     rc = record_handler_->get_record(&rid, &record);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to get record %s: %s", this->name(),
                  rid.to_string().c_str());
        return rc;
    }

    return transaction->commit_insert(this, record);
}

ResultCode Table::rollback_insert(Transaction* transaction, const RID& rid) {

    Record record;
    ResultCode     rc = record_handler_->get_record(&rid, &record);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to get record %s: %s", this->name(),
                  rid.to_string().c_str());
        return rc;
    }

    // remove all indexes
    rc = delete_entry_of_indexes(record.data, rid, false);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to delete indexes of record(rid=%d.%d) while "
                  "rollback insert, rc=%d:%s",
                  rid.page_num, rid.slot_num, rc, strrc(rc));
        return rc;
    }

    rc = record_handler_->delete_record(&rid);
    return rc;
}

ResultCode Table::insert_record(Transaction* transaction, Record* record) {
    ResultCode rc = ResultCode::SUCCESS;

    if (transaction != nullptr) {
        transaction->init_transaction_info(this, *record);
    }
    rc = record_handler_->insert_record(record->data, table_meta_.record_size(),
                                        &record->rid);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Insert record failed. table name=%s, rc=%d:%s",
                  table_meta_.name(), rc, strrc(rc));
        return rc;
    }

    if (transaction != nullptr) {
        rc = transaction->insert_record(this, record);
        if (rc != ResultCode::SUCCESS) {
            LOG_ERROR("Failed to log operation(insertion) to transaction");

            ResultCode rc2 = record_handler_->delete_record(&record->rid);
            if (rc2 != ResultCode::SUCCESS) {
                LOG_ERROR("Failed to rollback record data when insert index "
                          "entries failed. table name=%s, rc=%d:%s",
                          name(), rc2, strrc(rc2));
            }
            return rc;
        }
    }

    rc = insert_entry_of_indexes(record->data, record->rid);
    if (rc != ResultCode::SUCCESS) {
        ResultCode rc2 = delete_entry_of_indexes(record->data, record->rid, true);
        if (rc2 != ResultCode::SUCCESS) {
            LOG_ERROR("Failed to rollback index data when insert index entries "
                      "failed. table name=%s, rc=%d:%s",
                      name(), rc2, strrc(rc2));
        }
        rc2 = record_handler_->delete_record(&record->rid);
        if (rc2 != ResultCode::SUCCESS) {
            LOG_PANIC("Failed to rollback record data when insert index "
                      "entries failed. table name=%s, rc=%d:%s",
                      name(), rc2, strrc(rc2));
        }
        return rc;
    }
    return rc;
}
ResultCode Table::insert_record(Transaction* transaction, int value_num, const Value* values) {
    if (value_num <= 0 || nullptr == values) {
        LOG_ERROR("Invalid argument. table name: %s, value num=%d, values=%p",
                  name(), value_num, values);
        return ResultCode::INVALID_ARGUMENT;
    }

    char* record_data;
    ResultCode    rc = make_record(value_num, values, record_data);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to create a record. rc=%d:%s", rc, strrc(rc));
        return rc;
    }

    Record record;
    record.data = record_data;
    // record.valid = true;
    rc = insert_record(transaction, &record);
    delete[] record_data;
    return rc;
}

const char*      Table::name() const { return table_meta_.name(); }

const TableMeta& Table::table_meta() const { return table_meta_; }

ResultCode Table::make_record(int value_num, const Value* values, char*& record_out) {
    // 检查字段类型是否一致
    if (value_num + table_meta_.sys_field_num() != table_meta_.field_num()) {
        LOG_WARN("Input values don't match the table's schema, table name:%s",
                 table_meta_.name());
        return ResultCode::SCHEMA_FIELD_MISSING;
    }

    const int normal_field_start_index = table_meta_.sys_field_num();
    for (int i = 0; i < value_num; i++) {
        const FieldMeta* field =
            table_meta_.field(i + normal_field_start_index);
        const Value& value = values[i];
        if (field->type() != value.type) {
            LOG_ERROR("Invalid value type. table name =%s, field name=%s, "
                      "type=%d, but given=%d",
                      table_meta_.name(), field->name(), field->type(),
                      value.type);
            return ResultCode::SCHEMA_FIELD_TYPE_MISMATCH;
        }
    }

    // 复制所有字段的值
    int   record_size = table_meta_.record_size();
    char* record      = new char[record_size];

    for (int i = 0; i < value_num; i++) {
        const FieldMeta* field =
            table_meta_.field(i + normal_field_start_index);
        const Value& value = values[i];
        memcpy(record + field->offset(), value.data, field->len());
    }

    record_out = record;
    return ResultCode::SUCCESS;
}

ResultCode Table::init_record_handler(const char* base_dir) {
    std::string data_file = table_data_file(base_dir, table_meta_.name());
    if (nullptr == data_buffer_pool_) {
        data_buffer_pool_ = theGlobalDiskBufferPool();
    }

    int data_buffer_pool_file_id;
    ResultCode  rc = data_buffer_pool_->open_file(data_file.c_str(),
                                          &data_buffer_pool_file_id);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s",
                  data_file.c_str(), rc, strrc(rc));
        return rc;
    }

    record_handler_ = new RecordFileHandler();
    rc = record_handler_->init(data_buffer_pool_, data_buffer_pool_file_id);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to init record handler. rc=%d:%s", rc, strrc(rc));
        data_buffer_pool_->close_file(data_buffer_pool_file_id);
        delete record_handler_;
        record_handler_ = nullptr;
        return rc;
    }

    file_id_ = data_buffer_pool_file_id;
    return rc;
}

/**
 * 为了不把Record暴露出去，封装一下
 */
class RecordReaderScanAdapter {
    public:
    explicit RecordReaderScanAdapter(void (*record_reader)(const char* data,
                                                           void*       context),
                                     void* context)
        : record_reader_(record_reader), context_(context) {}

    void consume(const Record* record) {
        record_reader_(record->data, context_);
    }

    private:
    void (*record_reader_)(const char*, void*);
    void* context_;
};

static ResultCode scan_record_reader_adapter(Record* record, void* context) {
    RecordReaderScanAdapter& adapter = *(RecordReaderScanAdapter*)context;
    adapter.consume(record);
    return ResultCode::SUCCESS;
}

ResultCode Table::scan_record(Transaction* transaction, ConditionFilter* filter, int limit,
                      void* context,
                      void (*record_reader)(const char* data, void* context)) {
    RecordReaderScanAdapter adapter(record_reader, context);
    return scan_record(transaction, filter, limit, (void*)&adapter,
                       scan_record_reader_adapter);
}

ResultCode Table::scan_record(Transaction* transaction, ConditionFilter* filter, int limit,
                      void* context,
                      ResultCode (*record_reader)(Record* record, void* context)) {
    if (nullptr == record_reader) {
        return ResultCode::INVALID_ARGUMENT;
    }

    if (0 == limit) {
        return ResultCode::SUCCESS;
    }

    if (limit < 0) {
        limit = INT_MAX;
    }

    IndexScanner* index_scanner = find_index_for_scan(filter);
    if (index_scanner != nullptr) {
        return scan_record_by_index(transaction, index_scanner, filter, limit, context,
                                    record_reader);
    }

    ResultCode                rc = ResultCode::SUCCESS;
    RecordFileScanner scanner;
    rc = scanner.open_scan(*data_buffer_pool_, file_id_, filter);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("failed to open scanner. file id=%d. rc=%d:%s", file_id_, rc,
                  strrc(rc));
        return rc;
    }

    int    record_count = 0;
    Record record;
    rc = scanner.get_first_record(&record);
    for (; ResultCode::SUCCESS == rc && record_count < limit;
         rc = scanner.get_next_record(&record)) {
        if (transaction == nullptr || transaction->is_visible(this, &record)) {
            rc = record_reader(&record, context);
            if (rc != ResultCode::SUCCESS) {
                break;
            }
            record_count++;
        }
    }

    if (ResultCode::RECORD_EOF == rc) {
        rc = ResultCode::SUCCESS;
    } else {
        LOG_ERROR("failed to scan record. file id=%d, rc=%d:%s", file_id_, rc,
                  strrc(rc));
    }
    scanner.close_scan();
    return rc;
}

ResultCode Table::scan_record_by_index(Transaction* transaction, IndexScanner* scanner,
                               ConditionFilter* filter, int limit,
                               void* context,
                               ResultCode (*record_reader)(Record*, void*)) {
    ResultCode     rc = ResultCode::SUCCESS;
    RID    rid;
    Record record;
    int    record_count = 0;
    while (record_count < limit) {
        rc = scanner->next_entry(&rid);
        if (rc != ResultCode::SUCCESS) {
            if (ResultCode::RECORD_EOF == rc) {
                rc = ResultCode::SUCCESS;
                break;
            }
            LOG_ERROR("Failed to scan table by index. rc=%d:%s", rc, strrc(rc));
            break;
        }

        rc = record_handler_->get_record(&rid, &record);
        if (rc != ResultCode::SUCCESS) {
            LOG_ERROR("Failed to fetch record of rid=%d:%d, rc=%d:%s",
                      rid.page_num, rid.slot_num, rc, strrc(rc));
            break;
        }

        if ((transaction == nullptr || transaction->is_visible(this, &record)) &&
            (filter == nullptr || filter->filter(record))) {
            rc = record_reader(&record, context);
            if (rc != ResultCode::SUCCESS) {
                LOG_TRACE("Record reader break the table scanning. rc=%d:%s",
                          rc, strrc(rc));
                break;
            }
        }

        record_count++;
    }

    scanner->destroy();
    return rc;
}

class IndexInserter {
    public:
    explicit IndexInserter(Index* index) : index_(index) {}

    ResultCode insert_index(const Record* record) {
        return index_->insert_entry(record->data, &record->rid);
    }

    private:
    Index* index_;
};

static ResultCode insert_index_record_reader_adapter(Record* record, void* context) {
    IndexInserter& inserter = *(IndexInserter*)context;
    return inserter.insert_index(record);
}

ResultCode Table::create_index(Transaction* transaction, const char* index_name,
                       const char* attribute_name) {
    if (common::is_blank(index_name) || common::is_blank(attribute_name)) {
        LOG_INFO("Invalid input arguments, table name is %s, index_name is "
                 "blank or attribute_name is blank",
                 name());
        return ResultCode::INVALID_ARGUMENT;
    }
    if (table_meta_.index(index_name) != nullptr ||
        table_meta_.find_index_by_field((attribute_name))) {
        LOG_INFO("Invalid input arguments, table name is %s, index %s exist or "
                 "attribute %s exist index",
                 name(), index_name, attribute_name);
        return ResultCode::SCHEMA_INDEX_EXIST;
    }

    const FieldMeta* field_meta = table_meta_.field(attribute_name);
    if (!field_meta) {
        LOG_INFO(
            "Invalid input arguments, there is no field of %s in table:%s.",
            attribute_name, name());
        return ResultCode::SCHEMA_FIELD_MISSING;
    }

    IndexMeta new_index_meta;
    ResultCode        rc = new_index_meta.init(index_name, *field_meta);
    if (rc != ResultCode::SUCCESS) {
        LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s, "
                 "field_name:%s",
                 name(), index_name, attribute_name);
        return rc;
    }

    // 创建索引相关数据
    BplusTreeIndex* index = new BplusTreeIndex();
    std::string     index_file =
        table_index_file(base_dir_.c_str(), name(), index_name);
    rc = index->create(index_file.c_str(), new_index_meta, *field_meta);
    if (rc != ResultCode::SUCCESS) {
        delete index;
        LOG_ERROR("Failed to create bplus tree index. file name=%s, rc=%d:%s",
                  index_file.c_str(), rc, strrc(rc));
        return rc;
    }

    // 遍历当前的所有数据，插入这个索引
    IndexInserter index_inserter(index);
    rc = scan_record(transaction, nullptr, -1, &index_inserter,
                     insert_index_record_reader_adapter);
    if (rc != ResultCode::SUCCESS) {
        // rollback
        delete index;
        LOG_ERROR("Failed to insert index to all records. table=%s, rc=%d:%s",
                  name(), rc, strrc(rc));
        return rc;
    }
    indexes_.push_back(index);

    TableMeta new_table_meta(table_meta_);
    rc = new_table_meta.add_index(new_index_meta);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s",
                  index_name, name(), rc, strrc(rc));
        return rc;
    }
    // 创建元数据临时文件
    std::string  tmp_file = table_meta_file(base_dir_.c_str(), name()) + ".tmp";
    std::fstream fs;
    fs.open(tmp_file,
            std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    if (!fs.is_open()) {
        LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s",
                  tmp_file.c_str(), strerror(errno));
        return ResultCode::IOERR; // 创建索引中途出错，要做还原操作
    }
    if (new_table_meta.serialize(fs) < 0) {
        LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s",
                  tmp_file.c_str(), errno, strerror(errno));
        return ResultCode::IOERR;
    }
    fs.close();

    // 覆盖原始元数据文件
    std::string meta_file = table_meta_file(base_dir_.c_str(), name());
    int         ret       = rename(tmp_file.c_str(), meta_file.c_str());
    if (ret != 0) {
        LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file "
                  "(%s) while creating index (%s) on table (%s). "
                  "system error=%d:%s",
                  tmp_file.c_str(), meta_file.c_str(), index_name, name(),
                  errno, strerror(errno));
        return ResultCode::IOERR;
    }

    table_meta_.swap(new_table_meta);

    LOG_INFO("Successfully added a new index (%s) on the table (%s)",
             index_name, name());

    return rc;
}

ResultCode Table::update_record(Transaction* transaction, const char* attribute_name,
                        const Value* value, int condition_num,
                        const Condition conditions[], int* updated_count) {
    return ResultCode::GENERIC_ERROR;
}

class RecordDeleter {
    public:
    RecordDeleter(Table& table, Transaction* transaction) : table_(table), transaction_(transaction) {}

    ResultCode delete_record(Record* record) {
        ResultCode rc = ResultCode::SUCCESS;
        rc    = table_.delete_record(transaction_, record);
        if (rc == ResultCode::SUCCESS) {
            deleted_count_++;
        }
        return rc;
    }

    int deleted_count() const { return deleted_count_; }

    private:
    Table& table_;
    Transaction*   transaction_;
    int    deleted_count_ = 0;
};

static ResultCode record_reader_delete_adapter(Record* record, void* context) {
    RecordDeleter& record_deleter = *(RecordDeleter*)context;
    return record_deleter.delete_record(record);
}

ResultCode Table::delete_record(Transaction* transaction, ConditionFilter* filter, int* deleted_count) {
    RecordDeleter deleter(*this, transaction);
    ResultCode            rc =
        scan_record(transaction, filter, -1, &deleter, record_reader_delete_adapter);
    if (deleted_count != nullptr) {
        *deleted_count = deleter.deleted_count();
    }
    return rc;
}

ResultCode Table::delete_record(Transaction* transaction, Record* record) {
    ResultCode rc = ResultCode::SUCCESS;
    if (transaction != nullptr) {
        rc = transaction->delete_record(this, record);
    } else {
        rc = delete_entry_of_indexes(record->data, record->rid,
                                     false); // 重复代码 refer to commit_delete
        if (rc != ResultCode::SUCCESS) {
            LOG_ERROR(
                "Failed to delete indexes of record (rid=%d.%d). rc=%d:%s",
                record->rid.page_num, record->rid.slot_num, rc, strrc(rc));
        } else {
            rc = record_handler_->delete_record(&record->rid);
        }
    }
    return rc;
}

ResultCode Table::commit_delete(Transaction* transaction, const RID& rid) {
    ResultCode     rc = ResultCode::SUCCESS;
    Record record;
    rc = record_handler_->get_record(&rid, &record);
    if (rc != ResultCode::SUCCESS) {
        return rc;
    }
    rc = delete_entry_of_indexes(record.data, record.rid, false);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to delete indexes of record(rid=%d.%d). rc=%d:%s",
                  rid.page_num, rid.slot_num, rc,
                  strrc(rc)); // panic?
    }

    rc = record_handler_->delete_record(&rid);
    if (rc != ResultCode::SUCCESS) {
        return rc;
    }

    return rc;
}

ResultCode Table::rollback_delete(Transaction* transaction, const RID& rid) {
    ResultCode     rc = ResultCode::SUCCESS;
    Record record;
    rc = record_handler_->get_record(&rid, &record);
    if (rc != ResultCode::SUCCESS) {
        return rc;
    }

    return transaction->rollback_delete(this, record); // update record in place
}

ResultCode Table::insert_entry_of_indexes(const char* record, const RID& rid) {
    ResultCode rc = ResultCode::SUCCESS;
    for (Index* index : indexes_) {
        rc = index->insert_entry(record, &rid);
        if (rc != ResultCode::SUCCESS) {
            break;
        }
    }
    return rc;
}

ResultCode Table::delete_entry_of_indexes(const char* record, const RID& rid,
                                  bool error_on_not_exists) {
    ResultCode rc = ResultCode::SUCCESS;
    for (Index* index : indexes_) {
        rc = index->delete_entry(record, &rid);
        if (rc != ResultCode::SUCCESS) {
            if (rc != ResultCode::RECORD_INVALID_KEY || !error_on_not_exists) {
                break;
            }
        }
    }
    return rc;
}

Index* Table::find_index(const char* index_name) const {
    for (Index* index : indexes_) {
        if (0 == strcmp(index->index_meta().name(), index_name)) {
            return index;
        }
    }
    return nullptr;
}

IndexScanner* Table::find_index_for_scan(const DefaultConditionFilter& filter) {
    const ConDesc* field_cond_desc = nullptr;
    const ConDesc* value_cond_desc = nullptr;
    if (filter.left().is_attr && !filter.right().is_attr) {
        field_cond_desc = &filter.left();
        value_cond_desc = &filter.right();
    } else if (filter.right().is_attr && !filter.left().is_attr) {
        field_cond_desc = &filter.right();
        value_cond_desc = &filter.left();
    }
    if (field_cond_desc == nullptr || value_cond_desc == nullptr) {
        return nullptr;
    }

    const FieldMeta* field_meta =
        table_meta_.find_field_by_offset(field_cond_desc->attr_offset);
    if (nullptr == field_meta) {
        LOG_PANIC("Cannot find field by offset %d. table=%s",
                  field_cond_desc->attr_offset, name());
        return nullptr;
    }

    const IndexMeta* index_meta =
        table_meta_.find_index_by_field(field_meta->name());
    if (nullptr == index_meta) {
        return nullptr;
    }

    Index* index = find_index(index_meta->name());
    if (nullptr == index) {
        return nullptr;
    }

    return index->create_scanner(filter.comp_op(),
                                 (const char*)value_cond_desc->value);
}

IndexScanner* Table::find_index_for_scan(const ConditionFilter* filter) {
    if (nullptr == filter) {
        return nullptr;
    }

    // remove dynamic_cast
    const DefaultConditionFilter* default_condition_filter =
        dynamic_cast<const DefaultConditionFilter*>(filter);
    if (default_condition_filter != nullptr) {
        return find_index_for_scan(*default_condition_filter);
    }

    const CompositeConditionFilter* composite_condition_filter =
        dynamic_cast<const CompositeConditionFilter*>(filter);
    if (composite_condition_filter != nullptr) {
        int filter_num = composite_condition_filter->filter_num();
        for (int i = 0; i < filter_num; i++) {
            IndexScanner* scanner =
                find_index_for_scan(&composite_condition_filter->filter(i));
            if (scanner != nullptr) {
                return scanner; // 可以找到一个最优的，比如比较符号是=
            }
        }
    }
    return nullptr;
}

ResultCode Table::sync() {
    ResultCode rc = data_buffer_pool_->purge_all_pages(file_id_);
    if (rc != ResultCode::SUCCESS) {
        LOG_ERROR("Failed to flush table's data pages. table=%s, rc=%d:%s",
                  name(), rc, strrc(rc));
        return rc;
    }

    for (Index* index : indexes_) {
        rc = index->sync();
        if (rc != ResultCode::SUCCESS) {
            LOG_ERROR(
                "Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
                name(), index->index_meta().name(), rc, strrc(rc));
            return rc;
        }
    }
    LOG_INFO("Sync table over. table=%s", name());
    return rc;
}
