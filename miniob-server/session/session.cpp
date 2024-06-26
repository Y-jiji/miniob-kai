/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its
affiliates. All rights reserved. miniob is licensed under Mulan PSL v2. You can
use this software according to the terms and conditions of the Mulan PSL v2. You
may obtain a copy of Mulan PSL v2 at: http://license.coscl.org.cn/MulanPSL2 THIS
SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2021/5/12.
//

#include <session/session.h>
#include <storage/transaction/transaction.h>

Session& Session::default_session() {
    static Session session;
    return session;
}

Session::Session(const Session& other) : current_db_(other.current_db_) {}

Session::~Session() {
    delete transaction_;
    transaction_ = nullptr;
}

const std::string& Session::get_current_db() const { return current_db_; }
void               Session::set_current_db(const std::string& dbname) {
    current_db_ = dbname;
}

void Session::set_transaction_multi_operation_mode(bool multi_operation_mode) {
    transaction_multi_operation_mode_ = multi_operation_mode;
}

bool Session::is_transaction_multi_operation_mode() const {
    return transaction_multi_operation_mode_;
}

Transaction* Session::current_transaction() {
    if (transaction_ == nullptr) {
        transaction_ = new Transaction;
    }
    return transaction_;
}
