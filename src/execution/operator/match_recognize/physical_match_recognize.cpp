#include "duckdb/execution/operator/match_recognize/physical_match_recognize.hpp"

#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/common/types/row/block_iterator.hpp"

namespace duckdb {

// inspo: TemplatedScan() in physical_asof_join.cpp
template <SortKeyType SORT_KEY_TYPE>
static void DetectTiesTemplated(const SortedRun &sorted_run) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<ExternalBlockIteratorState, SORT_KEY>;
	ExternalBlockIteratorState block_state(*sorted_run.key_data, sorted_run.payload_data.get());
	BLOCK_ITERATOR itr(block_state, 0, 0);

	auto prev = itr[0];
	for (idx_t i = 1; i < sorted_run.Count(); i++) {
		auto cur = itr[i];
		if (!(prev < cur) && !(cur < prev)) {
			throw InvalidInputException("MATCH_RECOGNIZE ORDER BY must be a total order (no ties)");
		}
		prev = cur;
	}
}

// inspo: SortedRunScanState::Scan in sorted_run.cpp
static void DetectTies(const SortedRun &sorted_run) {
    // we need at least 2 rows to have someting to compare
    if (sorted_run.Count() <= 1) {
        return;
    }

    const auto sort_key_type = sorted_run.key_data->GetLayout().GetSortKeyType();

    switch (sort_key_type) {
    case SortKeyType::NO_PAYLOAD_FIXED_8:
        return DetectTiesTemplated<SortKeyType::NO_PAYLOAD_FIXED_8>(sorted_run);
    case SortKeyType::NO_PAYLOAD_FIXED_16:
        return DetectTiesTemplated<SortKeyType::NO_PAYLOAD_FIXED_16>(sorted_run);
    case SortKeyType::NO_PAYLOAD_FIXED_24:
        return DetectTiesTemplated<SortKeyType::NO_PAYLOAD_FIXED_24>(sorted_run);
    case SortKeyType::NO_PAYLOAD_FIXED_32:
        return DetectTiesTemplated<SortKeyType::NO_PAYLOAD_FIXED_32>(sorted_run);
    case SortKeyType::NO_PAYLOAD_VARIABLE_32:
        return DetectTiesTemplated<SortKeyType::NO_PAYLOAD_VARIABLE_32>(sorted_run);
    case SortKeyType::PAYLOAD_FIXED_16:
        return DetectTiesTemplated<SortKeyType::PAYLOAD_FIXED_16>(sorted_run);
    case SortKeyType::PAYLOAD_FIXED_24:
        return DetectTiesTemplated<SortKeyType::PAYLOAD_FIXED_24>(sorted_run);
    case SortKeyType::PAYLOAD_FIXED_32:
        return DetectTiesTemplated<SortKeyType::PAYLOAD_FIXED_32>(sorted_run);
    case SortKeyType::PAYLOAD_VARIABLE_32:
        return DetectTiesTemplated<SortKeyType::PAYLOAD_VARIABLE_32>(sorted_run);
    default:
        throw NotImplementedException("CheckForTies for %s", 
                                      EnumUtil::ToString(sort_key_type));
    }
}

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
		: next_group(0) {
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
                                                         OperatorSourceInput &source) const {
    auto &gsource = source.global_state.Cast<MatchRecognizeGlobalSourceState>();
	auto &gsink = sink_state->Cast<MatchRecognizeGlobalSinkState>();
    auto &sort_strategy = *gsink.sort_strategy;

    // Iterate over partitions
	while (gsource.next_group < gsource.match_recognize_hash_groups.size()) {
		auto &mr_group = gsource.match_recognize_hash_groups[gsource.next_group];
        if (!mr_group) {
            ++gsource.next_group;
            continue;
        }
        const idx_t group_idx = mr_group->hash_bin;

        // Materialize the sorted run for this partition
        auto unused = make_uniq<LocalSourceState>();
        OperatorSourceInput hsource {*gsource.hashed_source, *unused, source.interrupt_state};

        sort_strategy.MaterializeSortedRun(context, group_idx, hsource);

        // Get the sorted run
		auto sorted_run = sort_strategy.GetSortedRun(context.client, group_idx, hsource);

        // Tie detection for ORDER BY
		if (sorted_run && sorted_run->Count() > 1 && !mr_group->ties_checked) {
			DetectTies(*sorted_run);
			mr_group->ties_checked = true;
		}

		// TODO: Run NFA matching
		// TODO: Compute bound_mr.measures
		// TODO: Emit rows according to bound_mr.one_row_per_match
		// TODO: Apply skip logic

        ++gsource.next_group;
    }
	
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
