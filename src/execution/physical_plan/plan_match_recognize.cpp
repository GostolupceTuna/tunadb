#include "duckdb/execution/operator/match_recognize/physical_match_recognize.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_match_recognize.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

// Walk prev_exec_condition (which still has BoundColumnRefExpression nodes from the binder)
// and convert each one to a BoundReferenceExpression 
//
// So the index assigned here is:
//   current-row ref  (table_index != prev_table_idx) → physical_idx
//   previous-row ref (table_index == prev_table_idx) → N + physical_idx
//
// physical_idx is found by scanning child_bindings for the entry whose
// column_index matches the BoundColumnRefExpression's column_index.
static void ResolvePrevRef(unique_ptr<Expression> &expr,
                           const vector<ColumnBinding> &child_bindings,
                           idx_t prev_table_idx,
                           idx_t N) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &col_ref = expr->Cast<BoundColumnRefExpression>();
		idx_t col_idx = col_ref.binding.column_index;
		LogicalType type = col_ref.return_type;

		bool is_prev = (col_ref.binding.table_index == prev_table_idx);

		// Find the physical position of this column in the child's output DataChunk
		idx_t physical_idx = DConstants::INVALID_INDEX;
		for (idx_t p = 0; p < child_bindings.size(); p++) {
			if (child_bindings[p].column_index == col_idx) {
				physical_idx = p;
				break;
			}
		}
		D_ASSERT(physical_idx != DConstants::INVALID_INDEX);

		// convert
		expr = make_uniq<BoundReferenceExpression>(type, is_prev ? N + physical_idx : physical_idx);
		return;
	}

	// Not a leaf — recurse into child expressions
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		ResolvePrevRef(child, child_bindings, prev_table_idx, N);
	});
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalMatchRecognize &op) {
	D_ASSERT(op.children.size() == 1);

	auto child_bindings = op.children[0]->GetColumnBindings();
	idx_t N = child_bindings.size();
	idx_t prev_table_idx = op.bound_mr.prev_table_idx;

	auto bound_mr_copy = op.bound_mr.Copy();

	// Resolve prev_exec_condition: BoundColumnRefExpression → BoundReferenceExpression
	// using the final column layout so PREV slot indices are correct
	for (auto &define : bound_mr_copy.defines) {
		if (define.uses_prev && define.prev_exec_condition) {
			ResolvePrevRef(define.prev_exec_condition, child_bindings, prev_table_idx, N);
		}
	}

	auto &child = CreatePlan(*op.children[0]);
	auto &mr = Make<PhysicalMatchRecognize>(std::move(bound_mr_copy), op.estimated_cardinality);
	mr.children.push_back(child);
	return mr;
}

} // namespace duckdb
