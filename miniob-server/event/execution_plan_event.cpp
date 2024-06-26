/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its
affiliates. All rights reserved. miniob is licensed under Mulan PSL v2. You can
use this software according to the terms and conditions of the Mulan PSL v2. You
may obtain a copy of Mulan PSL v2 at: http://license.coscl.org.cn/MulanPSL2 THIS
SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2021/5/11.
//

#include <event/execution_plan_event.h>
#include <event/sql_event.h>

ExecutionPlanEvent::ExecutionPlanEvent(SQLStageEvent* sql_event, Query* sqls)
    : sql_event_(sql_event), sqls_(sqls) {}
ExecutionPlanEvent::~ExecutionPlanEvent() {
    sql_event_ = nullptr;
    // if (sql_event_) {
    //   sql_event_->doneImmediate();
    // }

    query_destroy(sqls_);
    sqls_ = nullptr;
}
