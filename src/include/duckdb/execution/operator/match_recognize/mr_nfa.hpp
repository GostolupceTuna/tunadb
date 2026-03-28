//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/match_recognize/mr_nfa.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "duckdb/common/types.hpp"

namespace duckdb {

//! Variable -> list of row indices that were bound to it (per partition) 
using MRMatchAssignment = unordered_map<string, vector<idx_t>>;

struct MRMatchResult {
	MRMatchAssignment assignment;
    //! first row of the match
    idx_t match_start; 
    //! first row after the match
	idx_t match_end; 
    idx_t match_length; 
};

//! Run NFA-based MATCH_RECOGNIZE pattern matching over one partition
vector<MRMatchResult> MRRunPatternMatching(
    //! The PATTERN string (e.g. "A B+")
    const string &pattern, 
    //! Number of rows in this partition
    idx_t num_rows, 
    //! Per-variable boolean masks
    const unordered_map<string, vector<bool>> &var_masks, 
    //! true = SKIP TO NEXT ROW, false = SKIP PAST LAST ROW
    bool skip_to_next_row);

} // namespace duckdb
