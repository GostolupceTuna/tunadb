#include "duckdb/execution/operator/match_recognize/physical_match_recognize.hpp"
#include "duckdb/execution/operator/match_recognize/mr_nfa.hpp"

#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Sink States
//===--------------------------------------------------------------------===//
// Global Sink State
class MatchRecognizeGlobalSinkState : public GlobalSinkState {
public:
	MatchRecognizeGlobalSinkState(const PhysicalMatchRecognize &op, ClientContext &context)
	    : op(op), client(context), count(0) {
		D_ASSERT(!op.children.empty());
		
		vector<unique_ptr<BaseStatistics>> no_stats; // we do not use any stats

		// fuer sort_strategy partition_keys & order_keys benoetigt 
		// -> full scan damit innerhalb partitionen und alles sortiert ist
		auto &partition_keys = op.bound_mr.partition_by; 
		auto &order_keys = op.bound_mr.order_by; 
		
		if (order_keys.empty()) {
			throw InvalidInputException("MATCH RECOGNIZE requires ORDER BY");
		}
		sort_strategy = SortStrategy::Factory(client, partition_keys, order_keys, op.children[0].get().GetTypes(), no_stats, op.estimated_cardinality);
		strategy_sink = sort_strategy->GetGlobalSinkState(client);
	}

	const PhysicalMatchRecognize &op;

	//! Client context
	ClientContext &client;
	//! The partitioned sunk data
	unique_ptr<SortStrategy> sort_strategy;
	//! The partitioned sunk data
	unique_ptr<GlobalSinkState> strategy_sink;
	//! The number of sunk rows (for progress)
	atomic<idx_t> count;
};

//	Per-thread sink state
class MatchRecognizeLocalSinkState : public LocalSinkState {
public:
	MatchRecognizeLocalSinkState(ExecutionContext &context, const MatchRecognizeGlobalSinkState &gstate)
	    : local_group(gstate.sort_strategy->GetLocalSinkState(context)) {
	}
	unique_ptr<LocalSinkState> local_group;
};

//===--------------------------------------------------------------------===//
// Constructor: this implements a sorted match recognize functions variant
//===--------------------------------------------------------------------===//
PhysicalMatchRecognize::PhysicalMatchRecognize(PhysicalPlan &physical_plan, BoundMatchRecognizeInfo bound_mr,
                                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::MATCH_RECOGNIZE, bound_mr.types, estimated_cardinality),
      bound_mr(std::move(bound_mr)) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalMatchRecognize::Sink(ExecutionContext &context, DataChunk &input, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<MatchRecognizeGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<MatchRecognizeLocalSinkState>();
	gstate.count += input.size();

	OperatorSinkInput child_sink {
		*gstate.strategy_sink, 
		*lstate.local_group, 
		sink.interrupt_state
	};
	return gstate.sort_strategy->Sink(context, input, child_sink);
}

// Combine 
SinkCombineResultType PhysicalMatchRecognize::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<MatchRecognizeGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<MatchRecognizeLocalSinkState>();

	OperatorSinkCombineInput child_input {
		*gstate.strategy_sink, 
		*lstate.local_group, 
		combine.interrupt_state
	};
	return gstate.sort_strategy->Combine(context, child_input);
}

// getLocalSinkState und getGlobalSinkState (Hilfsfunktionen)
unique_ptr<LocalSinkState> PhysicalMatchRecognize::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<MatchRecognizeGlobalSinkState>();
	return make_uniq<MatchRecognizeLocalSinkState>(context, gstate);
}

unique_ptr<GlobalSinkState> PhysicalMatchRecognize::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<MatchRecognizeGlobalSinkState>(*this, client);
}

// Finalize
SinkFinalizeType PhysicalMatchRecognize::Finalize(Pipeline &pipeline, Event &event, ClientContext &context, OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MatchRecognizeGlobalSinkState>();

	OperatorSinkFinalizeInput child_finalize {*gstate.strategy_sink, input.interrupt_state};

	return gstate.sort_strategy->Finalize(context, child_finalize);
}

ProgressData PhysicalMatchRecognize::GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
                                                     const ProgressData source_progress) const {
	auto &gsink = gstate.Cast<MatchRecognizeGlobalSinkState>();
	return gsink.sort_strategy->GetSinkProgress(context, *gsink.strategy_sink, source_progress);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
struct MatchRecognizeHashGroup {
	MatchRecognizeHashGroup(idx_t hash_bin, idx_t count)
		: hash_bin(hash_bin), count(count), ties_checked(false) {
	}

	//! Index into the sort strategy's hash groups
	idx_t hash_bin;
	//! Number of rows in this partition
	idx_t count;
	//! track if tie detection done
	bool ties_checked;
};

class MatchRecognizeGlobalSourceState : public GlobalSourceState {
public:
	MatchRecognizeGlobalSourceState(ClientContext &client, MatchRecognizeGlobalSinkState &gsink)
		: next_group(0), initialized(false), within_initialized(false) {
		auto &sort_strategy = *gsink.sort_strategy;
		hashed_source = sort_strategy.GetGlobalSourceState(client, *gsink.strategy_sink);
		auto &hash_groups = sort_strategy.GetHashGroups(*hashed_source);
		match_recognize_hash_groups.resize(hash_groups.size());

		for (idx_t group_idx = 0; group_idx < hash_groups.size(); ++group_idx) {
			const auto &group = hash_groups[group_idx];
			if (!group.count) {
				continue;
			}
			auto match_recognize_hash_group = make_uniq<MatchRecognizeHashGroup>(group_idx, group.count);

			match_recognize_hash_groups[group_idx] = std::move(match_recognize_hash_group);
		}
	}

	//! The hashed sort global source state for delayed sorting
    unique_ptr<GlobalSourceState> hashed_source;
    //! The sorted hash groups
    vector<unique_ptr<MatchRecognizeHashGroup>> match_recognize_hash_groups;
    //! Index of the next partition to process
	atomic<idx_t> next_group;
	//! Whether all partitions have been processed into results
	bool initialized;
	//! Cached WITHIN interval value (if present)
	bool within_initialized;
	Value within_value;
	//! Pre-computed output rows
	unique_ptr<ColumnDataCollection> results;
	//! Scan cursor for results
	ColumnDataScanState scan_state;
};

// Per-thread scan state
class MatchRecognizeLocalSourceState : public LocalSourceState {
public:
	MatchRecognizeLocalSourceState(ExecutionContext &context, MatchRecognizeGlobalSourceState &gsource)
	    : local_source(nullptr) {
	}

	//! Thread-local scan cursor (populated during GetDataInternal)
	unique_ptr<LocalSourceState> local_source;
};

unique_ptr<LocalSourceState> PhysicalMatchRecognize::GetLocalSourceState(ExecutionContext &context,
                                                                         GlobalSourceState &gsource_p) const {
	auto &gsource = gsource_p.Cast<MatchRecognizeGlobalSourceState>();
	return make_uniq<MatchRecognizeLocalSourceState>(context, gsource);
}

unique_ptr<GlobalSourceState> PhysicalMatchRecognize::GetGlobalSourceState(ClientContext &client) const {
	auto &gsink = sink_state->Cast<MatchRecognizeGlobalSinkState>();
	return make_uniq<MatchRecognizeGlobalSourceState>(client, gsink);
}

//===--------------------------------------------------------------------===//
// Tie detection 
//===--------------------------------------------------------------------===//
// inspo: BoundIndex::ApplyBufferedReplays at src/execution/index/bound_index.cpp
static void DetectTies(const vector<BoundOrderByNode> &order_by,
                       ColumnDataCollection &partition, ClientContext &client) {
	// A single row (or empty partition) can never have ties
	if (partition.Count() <= 1) {
		return;
	}

	// Collect the ORDER BY expressions and their return types so we can
	// evaluate them on each scanned chunk with an ExpressionExecutor
	vector<unique_ptr<Expression>> order_exprs;
	vector<LogicalType> key_types;
	for (auto &order : order_by) {
		order_exprs.push_back(order.expression->Copy());
		key_types.push_back(order.expression->return_type);
	}

	// Build an executor that knows how to compute the ORDER BY expressions
	ExpressionExecutor executor(client, order_exprs);

	// We will store the evaluated ORDER BY key values for every row so we
	// can compare adjacent rows afterwards
	vector<vector<Value>> key_values;
	key_values.reserve(partition.Count());

	// Prepare a scan over the ColumnDataCollection (one chunk at a time)
	DataChunk scan_chunk;
	partition.InitializeScanChunk(scan_chunk);
	ColumnDataScanState scan_state;
	partition.InitializeScan(scan_state);

	// Scan the partition chunk-by-chunk, evaluate the ORDER BY expressions
	// on each chunk, and extract the resulting key values row by row
	while (partition.Scan(scan_state, scan_chunk)) {
		// Allocate a chunk to hold the evaluated ORDER BY key columns
		DataChunk key_chunk;
		key_chunk.Initialize(Allocator::DefaultAllocator(), key_types);

		// Run the ORDER BY expressions on the scanned input chunk
		executor.Execute(scan_chunk, key_chunk);

		// Extract each row's key values into our materialized vector
		for (idx_t r = 0; r < scan_chunk.size(); r++) {
			vector<Value> row;
			for (idx_t c = 0; c < key_chunk.ColumnCount(); c++) {
				row.push_back(key_chunk.GetValue(c, r));
			}
			key_values.push_back(std::move(row));
		}
	}

	// Walk through adjacent rows and check if any two consecutive rows
	// have identical ORDER BY key values (= a tie).
	for (idx_t i = 1; i < key_values.size(); i++) {
		bool all_equal = true;
		for (idx_t c = 0; c < key_values[i].size(); c++) {
			if (!Value::NotDistinctFrom(key_values[i - 1][c], key_values[i][c])) {
				all_equal = false;
				break;
			}
		}
		if (all_equal) {
			throw InvalidInputException("MATCH_RECOGNIZE ORDER BY must be a total order (no ties)");
		}
	}
}

//===--------------------------------------------------------------------===//
// Evaluate WITHIN clause
//===--------------------------------------------------------------------===//
// Create a lookup table for WITHIN: 
// Evaluate the first ORDER BY expression for every row in the partition and
// return a row-aligned vector of ORDER BY values (index i -> order key for row i)
static vector<Value> CreateLookupForWithin(const BoundOrderByNode &order_by, ColumnDataCollection &partition, ClientContext &client) {
	// build a single-expression executor for the first ORDER BY key
	vector<unique_ptr<Expression>> order_exprs;
	vector<LogicalType> key_types;
	order_exprs.push_back(order_by.expression->Copy());
	key_types.push_back(order_by.expression->return_type);

	ExpressionExecutor executor(client, order_exprs);

	// reserve memory
	vector<Value> values;
	values.reserve(partition.Count());

	// prepare chunk-wise scan over the partition
	DataChunk scan_chunk;
	partition.InitializeScanChunk(scan_chunk);
	ColumnDataScanState scan_state;
	partition.InitializeScan(scan_state);

	while (partition.Scan(scan_state, scan_chunk)) {
		// execute aka compute the ORDER BY expression on the current input chunk
		DataChunk key_chunk;
		key_chunk.Initialize(Allocator::DefaultAllocator(), key_types);
		executor.Execute(scan_chunk, key_chunk);

		// append the computed key value for each row
		for (idx_t r = 0; r < scan_chunk.size(); r++) {
			values.push_back(key_chunk.GetValue(0, r));
		}
	}
	return values;
}

// checks if match exceeds given time interval in WITHIN clause
static bool ExceedsWithin(const Value &start, const Value &end, const Value &within, LogicalTypeId order_type) {
	// null endpoints or null WITHIN behave like "not within"
	if (start.IsNull() || end.IsNull()) {
		return true;
	}
	if (within.IsNull()) {
		return true;
	}

	interval_t delta;
	switch (order_type) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		// timestamp difference in microseconds
		auto timestamp_start = start.GetValue<timestamp_t>();
		auto timestamp_end = end.GetValue<timestamp_t>();
		delta = Interval::GetDifference(timestamp_end, timestamp_start);
		break;
	}
	case LogicalTypeId::DATE: {
		// date difference via epoch micros
		auto date_start = start.GetValue<date_t>();
		auto date_end = end.GetValue<date_t>();
		auto micro_start = Date::EpochMicroseconds(date_start);
		auto micro_end = Date::EpochMicroseconds(date_end);
		delta = Interval::FromMicro(micro_end - micro_start);
		break;
	}
	case LogicalTypeId::TIME: {
		// time values are already stored as micros since midnight
		auto time_start = start.GetValue<dtime_t>();
		auto time_end = end.GetValue<dtime_t>();
		delta = Interval::FromMicro(time_end.micros - time_start.micros);
		break;
	}
	case LogicalTypeId::TIME_TZ: {
		// normalize TIME_TZ to TIME (for different time zones)
		auto time_start = Time::NormalizeTimeTZ(start.GetValue<dtime_tz_t>());
		auto time_end = Time::NormalizeTimeTZ(end.GetValue<dtime_tz_t>());
		delta = Interval::FromMicro(time_end.micros - time_start.micros);
		break;
	}
	default:
		throw InvalidInputException("WITHIN requires ORDER BY on a time or date column");
	}

	// compare absolute time distance against WITHIN interval
	int64_t delta_in_microsecs = Interval::GetMicro(delta);
	if (delta_in_microsecs < 0) {
		delta_in_microsecs = - delta_in_microsecs;
	}
	auto within_interval = within.GetValue<interval_t>();
	int64_t within_in_microsecs = Interval::GetMicro(within_interval);
	if (within_in_microsecs < 0) {
		throw InvalidInputException("WITHIN interval must be non-negative");
	}
	return delta_in_microsecs > within_in_microsecs;
}

//===--------------------------------------------------------------------===//
// Evaluate DEFINE conditions -> per-variable boolean masks
//===--------------------------------------------------------------------===//

// Evaluates every DEFINE condition on the partition and returns a map from
// pattern variable name to a boolean mask (one bool per row). The NFA guard
// functions later check these masks to decide whether a variable matches at
// a given row position.
static unordered_map<string, vector<bool>> EvaluateDefines(const vector<BoundDefine> &defines,
                                                            ColumnDataCollection &partition, ClientContext &client) {
	unordered_map<string, vector<bool>> var_masks;
	idx_t num_rows = partition.Count();

	// Process each DEFINE independently: build an executor, scan the
	// partition, evaluate the boolean condition, and store the result mask
	for (auto &define : defines) {
		// Start with all rows set to false (no match)
		vector<bool> mask(num_rows, false);

		// Wrap the DEFINE condition in an ExpressionExecutor
		vector<unique_ptr<Expression>> exprs;
		exprs.push_back(define.condition->Copy());
		ExpressionExecutor executor(client, exprs);

		// Prepare a fresh scan over the partition
		DataChunk scan_chunk;
		partition.InitializeScanChunk(scan_chunk);
		ColumnDataScanState scan_state;
		partition.InitializeScan(scan_state);

		idx_t row = 0;
		while (partition.Scan(scan_state, scan_chunk)) {
			// Allocate a boolean result chunk and execute the condition
			DataChunk result_chunk;
			result_chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BOOLEAN});
			executor.Execute(scan_chunk, result_chunk);

			// Flatten converts any dictionary/constant vectors to flat format
			// so we can safely read the raw bool* array and validity mask
			result_chunk.Flatten();

			auto result_data = FlatVector::GetData<bool>(result_chunk.data[0]);
			auto &validity = FlatVector::Validity(result_chunk.data[0]);

			// A row matches the DEFINE only if the value is valid (not NULL)
			// and the boolean value is true
			for (idx_t r = 0; r < scan_chunk.size(); r++) {
				mask[row + r] = validity.RowIsValid(r) && result_data[r];
			}
			row += scan_chunk.size();
		}

		var_masks[define.variable_name] = std::move(mask);
	}

	return var_masks;
}

//===--------------------------------------------------------------------===//
// Materialise all row values for MEASURES lookup
//===--------------------------------------------------------------------===//
// inspo: ColumnDataRowCollection::ColumnDataRowCollection in column_data_collection.cpp
// Converts a ColumnDataCollection (columnar, chunk-based) into a row-indexed
// vector<vector<Value>> so that ComputeMeasure can access by row index.
static vector<vector<Value>> MaterializePartitionRows(ColumnDataCollection &partition) {
	vector<vector<Value>> rows;
	rows.reserve(partition.Count());

	// Prepare a scan over the ColumnDataCollection (one chunk at a time)
	DataChunk scan_chunk;
	partition.InitializeScanChunk(scan_chunk);
	ColumnDataScanState scan_state;
	partition.InitializeScan(scan_state);

	// Scan chunk-by-chunk; for each chunk extract every row's column values
	while (partition.Scan(scan_state, scan_chunk)) {
		for (idx_t r = 0; r < scan_chunk.size(); r++) {
			vector<Value> row;
			row.reserve(scan_chunk.ColumnCount());
			for (idx_t c = 0; c < scan_chunk.ColumnCount(); c++) {
				// GetValue extracts a single cell as a Value object
				row.push_back(scan_chunk.GetValue(c, r));
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

//===--------------------------------------------------------------------===//
// Compute a single MEASURES value from a match
//===--------------------------------------------------------------------===//
// Computes a single MEASURES value for one match. Looks up which rows the NFA
// bound to the measure's pattern variable, then applies the requested aggregate
// function (FIRST, LAST, COUNT, MIN, MAX, SUM, AVG) over those rows' column values.
// Returns a typed NULL if no rows were bound to the variable.
static Value ComputeMeasure(const BoundMeasure &measure, const MRMatchAssignment &assignment,
                            const vector<vector<Value>> &partition_rows) {
	// Look up the row indices the NFA assigned to this measure's pattern variable
	auto it = assignment.find(measure.pattern_variable);
	vector<idx_t> rows;
	if (it != assignment.end()) {
		rows = it->second;
	}

	const auto &func = measure.function_name;
	// The column in the partition that this measure reads from
	idx_t col_idx = measure.input_column_index;

	// LAST (or bare column reference with no function): return the last matched row's value
	if (func.empty() || func == "LAST") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		return partition_rows[rows.back()][col_idx].DefaultCastAs(measure.output_type);
	}
	// FIRST: return the first matched row's value
	if (func == "FIRST") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		return partition_rows[rows.front()][col_idx].DefaultCastAs(measure.output_type);
	}
	// FIRST: return the first matched row's value
	if (func == "FIRST") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		return partition_rows[rows.front()][col_idx].DefaultCastAs(measure.output_type);
	}
	// COUNT: number of matched rows (including NULLs)
	if (func == "COUNT") {
		return Value::BIGINT(NumericCast<int64_t>(rows.size()));
	}
	// MIN: smallest non-NULL value among matched rows
	if (func == "MIN") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		Value result = partition_rows[rows[0]][col_idx];
		for (idx_t i = 1; i < rows.size(); i++) {
			const auto &v = partition_rows[rows[i]][col_idx];
			if (!v.IsNull() && (result.IsNull() || v < result)) {
				result = v;
			}
		}
		return result.DefaultCastAs(measure.output_type);
	}
	// MAX: largest non-NULL value among matched rows
	if (func == "MAX") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		Value result = partition_rows[rows[0]][col_idx];
		for (idx_t i = 1; i < rows.size(); i++) {
			const auto &v = partition_rows[rows[i]][col_idx];
			if (!v.IsNull() && (result.IsNull() || v > result)) {
				result = v;
			}
		}
		return result.DefaultCastAs(measure.output_type);
	}
	// SUM: sum of all non-NULL values, cast through DOUBLE for arithmetic
	if (func == "SUM") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		double sum = 0;
		bool has_value = false;
		for (auto row_idx : rows) {
			const auto &v = partition_rows[row_idx][col_idx];
			if (!v.IsNull()) {
				sum += v.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
				has_value = true;
			}
		}
		if (!has_value) {
			return Value(measure.output_type);
		}
		return Value::DOUBLE(sum).DefaultCastAs(measure.output_type);
	}
	// AVG: mean of all non-NULL values
	if (func == "AVG") {
		if (rows.empty()) {
			return Value(measure.output_type);
		}
		double sum = 0;
		idx_t count = 0;
		for (auto row_idx : rows) {
			const auto &v = partition_rows[row_idx][col_idx];
			if (!v.IsNull()) {
				sum += v.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
				count++;
			}
		}
		if (count == 0) {
			return Value(measure.output_type);
		}
		return Value::DOUBLE(sum / static_cast<double>(count)).DefaultCastAs(measure.output_type);
	}

	throw InternalException("MATCH_RECOGNIZE: unknown MEASURES function '%s'", func);
}

// Per-row variant for ALL ROWS PER MATCH. For bare column references (no
// aggregate function), returns the current row's value only if that row is
// labeled with the measure's pattern variable; otherwise returns NULL.
// Aggregate measures (FIRST, LAST, COUNT, ...) are the same for every row
// in the match, so they delegate to ComputeMeasure.
static Value ComputeMeasureForRow(const BoundMeasure &measure, const MRMatchAssignment &assignment,
                                  const vector<vector<Value>> &partition_rows, idx_t current_row) {
	// Bare column reference: per-row semantics
	if (measure.function_name.empty()) {
		auto it = assignment.find(measure.pattern_variable);
		if (it == assignment.end()) {
			return Value(measure.output_type);
		}
		// Check if the current row is assigned to this variable
		for (auto idx : it->second) {
			if (idx == current_row) {
				return partition_rows[current_row][measure.input_column_index].DefaultCastAs(measure.output_type);
			}
		}
		// Current row is not labeled with this variable
		return Value(measure.output_type);
	}
	// Aggregate functions are match-level, not row-level
	return ComputeMeasure(measure, assignment, partition_rows);
}

//===--------------------------------------------------------------------===//
// GetDataInternal
//===--------------------------------------------------------------------===//
// Produce the next output chunk.
// Iterates over sorted partitions, runs NFA pattern matching per partition,
// computes MEASURES, and emits rows according to ONE ROW / ALL ROWS PER MATCH.
SourceResultType PhysicalMatchRecognize::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &source) const {
	// get state objects
    auto &gsource = source.global_state.Cast<MatchRecognizeGlobalSourceState>();
	auto &gsink = sink_state->Cast<MatchRecognizeGlobalSinkState>();
    auto &sort_strategy = *gsink.sort_strategy;
	auto &client = gsink.client;
	auto &bound_mr = this->bound_mr;
	
	auto &lsource = source.local_state.Cast<MatchRecognizeLocalSourceState>();
	if (!lsource.local_source) {
		lsource.local_source = sort_strategy.GetLocalSourceState(context, *gsource.hashed_source);
	}

	// turn WITHIN expression to a value we can calculate with
	const bool use_within = bound_mr.within != nullptr;
	if (use_within && !gsource.within_initialized) {
		// check if it provides a fixed duration (no placeholders or row dependence)
		if (bound_mr.within->HasParameter() || !bound_mr.within->IsFoldable()) {
			throw InvalidInputException("WITHIN must be a constant INTERVAL expression");
		}
		gsource.within_value = ExpressionExecutor::EvaluateScalar(client, *bound_mr.within);
		if (gsource.within_value.IsNull()) {
			throw InvalidInputException("WITHIN interval must not be NULL");
		}
		gsource.within_initialized = true;
	}

	// Process all partitions on the first call and buffer results
	if (!gsource.initialized) {
		// we need a ColumnDataCollection as a buffer to store all match results
		// column-oriented 
		gsource.results = make_uniq<ColumnDataCollection>(client, bound_mr.types);
		// Iterate over partitions
		while (gsource.next_group < gsource.match_recognize_hash_groups.size()) {
			auto &mr_group = gsource.match_recognize_hash_groups[gsource.next_group];
			// skip empty groups
			if (!mr_group) {
				++gsource.next_group;
				continue;
			}
			const idx_t group_idx = mr_group->hash_bin;

			// sort partition data by PARTITION BY + ORDER BY keys
			OperatorSinkFinalizeInput finalize_input {*gsink.strategy_sink, source.interrupt_state};
			sort_strategy.SortColumnData(context, group_idx, finalize_input);

			// materialize
			OperatorSourceInput hsource {*gsource.hashed_source, *lsource.local_source, source.interrupt_state};
			sort_strategy.MaterializeColumnData(context, group_idx, hsource);
			auto partition_data = sort_strategy.GetColumnData(group_idx, hsource);

			// skip empty partitions
			if (!partition_data || partition_data->Count() == 0) {
				++gsource.next_group;
				continue;
			}

			// Tie detection for ORDER BY
			if (!mr_group->ties_checked) {
				DetectTies(bound_mr.order_by, *partition_data, client);
				mr_group->ties_checked = true;
			}

			idx_t num_rows = partition_data->Count();

			// convert column-oriented partition into row-indexed access for the variables in MEASURES
			auto partition_rows = MaterializePartitionRows(*partition_data);

			// turn DEFINEs into boolean masks (one for each variable)
			auto var_masks = EvaluateDefines(bound_mr.defines, *partition_data, client);

			// cache values of the leading ORDER BY column to calculate match durations for WITHIN
			vector<Value> order_values;
			LogicalTypeId order_type = LogicalTypeId::INVALID;
			if (use_within) {
				order_values = CreateLookupForWithin(bound_mr.order_by[0], *partition_data, client);
				order_type = bound_mr.order_by[0].expression->return_type.id();
			}
	
			// Run NFA matching
			auto matches = MRRunPatternMatching(bound_mr.pattern, num_rows, var_masks, bound_mr.skip_to_next_row);

			// compute MEASURES and emit output rows
			for (auto &match : matches) {
				if (use_within) {
					auto start_idx = match.match_start;
					auto end_idx = match.match_end > 0 ? match.match_end - 1 : match.match_start;
					if (start_idx >= order_values.size() || end_idx >= order_values.size()) {
						continue;
					}
					// skip match if not within time interval
					if (ExceedsWithin(order_values[start_idx], order_values[end_idx], gsource.within_value, order_type)) {
						continue;
					}
				}
				// ONE ROW PER MATCH
				if (bound_mr.one_row_per_match) {

					// Allocate a single-row output chunk
					DataChunk output_chunk;
					output_chunk.Initialize(Allocator::DefaultAllocator(), bound_mr.types);
					output_chunk.SetCardinality(1);

					// Fill each MEASURES column for this row
					for (idx_t m = 0; m < bound_mr.measures.size(); m++) {
						auto val = ComputeMeasure(bound_mr.measures[m], match.assignment, partition_rows);
						output_chunk.SetValue(m, 0, std::move(val));
					}
					// append output row to results buffer
					gsource.results->Append(output_chunk);
				} else {
					// The same thing, with an additional loop	
					for (idx_t i = 0; i < match.match_length; i++) {
						idx_t current_row = match.match_start + i;

						// Allocate a single-row output chunk
						DataChunk output_chunk;
						output_chunk.Initialize(Allocator::DefaultAllocator(), bound_mr.types);
						output_chunk.SetCardinality(1);

						// Fill each MEASURES column for this row
						for (idx_t m = 0; m < bound_mr.measures.size(); m++) {
							auto val = ComputeMeasureForRow(bound_mr.measures[m], match.assignment,
							                                partition_rows, current_row);
							output_chunk.SetValue(m, 0, std::move(val));
						}

						// Append the row to the buffered results
						gsource.results->Append(output_chunk);
					}
				}
			}

			++gsource.next_group;
		}

		// all partitions done —> prepare to scan buffered results
		gsource.results->InitializeScan(gsource.scan_state);
		gsource.initialized = true;
	}

	if (!gsource.results || gsource.results->Count() == 0) {
		return SourceResultType::FINISHED;
	}

	// get next batch from result buffer if there is more
	if (gsource.results->Scan(gsource.scan_state, chunk)) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// ParamsToString
//===--------------------------------------------------------------------===//
InsertionOrderPreservingMap<string> PhysicalMatchRecognize::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["pattern"] = bound_mr.pattern;
	result["one_row_per_match"] = bound_mr.one_row_per_match ? "true" : "false";
	result["skip_to_next_row"] = bound_mr.skip_to_next_row ? "true" : "false";
	result["partitions"] = to_string(bound_mr.partition_by.size());
	result["orders"] = to_string(bound_mr.order_by.size());
	result["defines"] = to_string(bound_mr.defines.size());
	result["measures"] = to_string(bound_mr.measures.size());
	if (bound_mr.within) {
		result["within"] = bound_mr.within->ToString();
	}
	return result;
}

} // namespace duckdb
