/*
 * Copyright (C) 2023 The Android Open Source Project
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
 */
#ifndef SRC_TRACE_PROCESSOR_DB_QUERY_EXECUTOR_H_
#define SRC_TRACE_PROCESSOR_DB_QUERY_EXECUTOR_H_

#include <vector>

#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/column.h"

namespace perfetto {
namespace trace_processor {

// Responsible for executing filtering/sorting operations on a single Table.
// TODO(b/283763282): Introduce sorting.
class QueryExecutor {
 public:
  static constexpr uint32_t kMaxOverlayCount = 8;

  // |row_count| is the size of the last overlay.
  QueryExecutor(const std::vector<column::Column*>& columns, uint32_t row_count)
      : columns_(columns), row_count_(row_count) {}

  // Apply all the constraints on the data and return the filtered RowMap.
  RowMap Filter(const std::vector<Constraint>& cs) {
    RowMap rm(0, row_count_);
    for (const auto& c : cs) {
      FilterColumn(c, *columns_[c.col_idx], &rm);
    }
    return rm;
  }

  // Sorts using vector of Order.
  // TODO(b/283763282): Implement.
  RowMap Sort(const std::vector<Order>&) { PERFETTO_FATAL("Not implemented."); }

  // Enables QueryExecutor::Filter on Table columns.
  static RowMap FilterLegacy(const Table*, const std::vector<Constraint>&);

  // Enables QueryExecutor::Sort on Table columns.
  // TODO(b/283763282): Implement.
  static RowMap SortLegacy(const Table*, const std::vector<Order>&) {
    PERFETTO_FATAL("Not implemented.");
  }

  // Used only in unittests. Exposes private function.
  static void BoundedColumnFilterForTesting(const Constraint&,
                                            const column::Column&,
                                            RowMap*);

  // Used only in unittests. Exposes private function.
  static void IndexedColumnFilterForTesting(const Constraint&,
                                            const column::Column&,
                                            RowMap*);

 private:
  // Updates RowMap with result of filtering single column using the Constraint.
  static void FilterColumn(const Constraint&, const column::Column&, RowMap*);

  // Filters the column using Range algorithm - tries to find the smallest Range
  // to filter the storage with.
  static void LinearSearch(const Constraint&, const column::Column&, RowMap*);

  // Filters the column using Index algorithm - finds the indices to filter the
  // storage with.
  static void IndexSearch(const Constraint&, const column::Column&, RowMap*);

  std::vector<column::Column*> columns_;

  // Number of rows in the outmost overlay.
  uint32_t row_count_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_QUERY_EXECUTOR_H_
