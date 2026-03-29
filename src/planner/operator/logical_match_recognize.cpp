#include "duckdb/planner/operator/logical_match_recognize.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

LogicalMatchRecognize::LogicalMatchRecognize() : LogicalOperator(LogicalOperatorType::LOGICAL_MATCH_RECOGNIZE) {
}

LogicalMatchRecognize::LogicalMatchRecognize(idx_t bind_idx, unique_ptr<LogicalOperator> plan, BoundMatchRecognizeInfo info_p)
    : LogicalOperator(LogicalOperatorType::LOGICAL_MATCH_RECOGNIZE), bind_index(bind_idx), bound_mr(std::move(info_p)) {
	D_ASSERT(plan);
	children.push_back(std::move(plan));
}

LogicalMatchRecognize::LogicalMatchRecognize(idx_t bind_index, const BoundMatchRecognizeInfo &info)
    : LogicalOperator(LogicalOperatorType::LOGICAL_MATCH_RECOGNIZE), bind_index(bind_index), bound_mr(info.Copy()) {
}

vector<ColumnBinding> LogicalMatchRecognize::GetColumnBindings() {
	vector<ColumnBinding> result;
	for (idx_t i = 0; i < bound_mr.types.size(); i++) {
		result.emplace_back(bind_index, i);
	}
	return result;
}

vector<idx_t> LogicalMatchRecognize::GetTableIndex() const {
	return vector<idx_t> {bind_index};
}

void LogicalMatchRecognize::ResolveTypes() {
	this->types = bound_mr.types;
}

string LogicalMatchRecognize::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", bind_index);
	}
#endif
	return LogicalOperator::GetName();
}

unique_ptr<LogicalOperator> LogicalMatchRecognize::Copy(ClientContext &context) const {
	auto copy_info = bound_mr.Copy();
	auto result = make_uniq<LogicalMatchRecognize>(bind_index, std::move(copy_info));
	for (auto &child : children) {
		result->children.push_back(child->Copy(context));
	}
	result->types = types; 

	return std::move(result);
}

// Add details for EXPLAIN output
InsertionOrderPreservingMap<string> LogicalMatchRecognize::ParamsToString() const {
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