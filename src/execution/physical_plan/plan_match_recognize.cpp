#include "duckdb/execution/operator/match_recognize/physical_match_recognize.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_match_recognize.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalMatchRecognize &op) {
	D_ASSERT(op.children.size() == 1);

	auto &child = CreatePlan(*op.children[0]);
	auto &mr = Make<PhysicalMatchRecognize>(op.bound_mr.Copy(), op.estimated_cardinality);
	mr.children.push_back(child);
	return mr;
}

} // namespace duckdb
