#include "duckdb/execution/operator/match_recognize/physical_match_recognize.hpp"

#include "duckdb/common/sorting/sort_strategy.hpp"

namespace duckdb {

//	Global sink state
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

// this implements a sorted match recognize functions variant
PhysicalMatchRecognize::PhysicalMatchRecognize(PhysicalPlan &physical_plan, BoundMatchRecognizeInfo bound_mr,
                                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::MATCH_RECOGNIZE, bound_mr.types, estimated_cardinality),
      bound_mr(std::move(bound_mr)) {
}

// Sink
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
	// TODO: PHY1 tie detection — check for ORDER BY ties within each partition
	// and throw InvalidInputException("MATCH_RECOGNIZE ORDER BY must be a total order (no ties)")
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
class MatchRecognizeGlobalSourceState : public GlobalSourceState {
public:
	MatchRecognizeGlobalSourceState(ClientContext &client, MatchRecognizeGlobalSinkState &gsink) {
		auto &sort_strategy = *gsink.sort_strategy;
		hashed_source = sort_strategy.GetGlobalSourceState(client, *gsink.strategy_sink);
	}

	//! Cursor over the sorted partitions produced by the sort strategy
	unique_ptr<GlobalSourceState> hashed_source;
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

//! Produce the next output chunk.
//! Iterates over sorted partitions, runs NFA pattern matching per partition,
//! computes MEASURES, and emits rows according to ONE ROW / ALL ROWS PER MATCH.
SourceResultType PhysicalMatchRecognize::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	// TODO: PHY1 pattern matching execution per partition:
	// 1. Iterate hash groups (partitions) via hashed_source
	// 2. For each partition get SortedRun and scan rows
	// 3. Run NFA matching using bound_mr.pattern and bound_mr.defines
	// 4. Compute bound_mr.measures (FIRST/LAST/COUNT/SUM/AVG/MIN/MAX)
	// 5. Emit rows according to bound_mr.one_row_per_match / ALL ROWS PER MATCH
	// 6. Apply skip logic (bound_mr.skip_to_next_row)
	return SourceResultType::FINISHED;
}

InsertionOrderPreservingMap<string> PhysicalMatchRecognize::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["pattern"] = bound_mr.pattern;
	result["one_row_per_match"] = bound_mr.one_row_per_match ? "true" : "false";
	result["skip_to_next_row"] = bound_mr.skip_to_next_row ? "true" : "false";
	result["partitions"] = to_string(bound_mr.partition_by.size());
	result["orders"] = to_string(bound_mr.order_by.size());
	result["defines"] = to_string(bound_mr.defines.size());
	result["measures"] = to_string(bound_mr.measures.size());
	return result;
}

} // namespace duckdb
