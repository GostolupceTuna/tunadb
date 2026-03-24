//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/match_recognize/physical_match_recognize.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/tableref/bound_match_recognize_ref.hpp"

namespace duckdb {

//! PhysicalMatchRecognize implements the physical MATCH_RECOGNIZE operation
class PhysicalMatchRecognize : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::MATCH_RECOGNIZE;

public:
	PhysicalMatchRecognize(PhysicalPlan &physical_plan, BoundMatchRecognizeInfo bound_mr, idx_t estimated_cardinality);

	BoundMatchRecognizeInfo bound_mr;

public:
	// Source Interface
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	//! Output order is not guaranteed across partitions
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                             const ProgressData source_progress) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	//! Single-threaded sink: row order within a partition must be deterministic
	bool ParallelSink() const override {
		return false;
	}
	//! Input must arrive in ORDER BY order so partitions are contiguous
	bool SinkOrderDependent() const override {
		return true;
	}

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
