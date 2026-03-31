#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

//! Transforms a PGMatchRecognize parse node into a MatchRecognizeRef AST node.
//! Handles all MATCH_RECOGNIZE subclauses: source, PARTITION BY, ORDER BY,
//! MEASURES, DEFINE, PATTERN, ROWS PER MATCH, and AFTER MATCH SKIP.
unique_ptr<TableRef> Transformer::TransformMatchRecognize(duckdb_libpgquery::PGMatchRecognize &root) {
	auto result = make_uniq<MatchRecognizeRef>();

	// Transform the source table reference (the table MATCH_RECOGNIZE operates on)
	if (root.source) {
		result->source = TransformTableRefNode(*root.source);
	}

	// Transform PARTITION BY clause (optional) — splits input into independent partitions
	if (root.partition) {
		TransformExpressionList(*root.partition, result->partition_by);
	}

	// Transform ORDER BY clause — defines the row ordering within each partition
	TransformOrderBy(root.order, result->order_by);

	// Transform MEASURES clause — defines output columns computed over matched rows
	// Each measure is a ResTarget node with an expression and an optional alias
	if (root.measures) {
		for (auto node = root.measures->head; node != nullptr; node = node->next) {
			auto raw_node = PGPointerCast<duckdb_libpgquery::PGNode>(node->data.ptr_value);
			if (raw_node->type != duckdb_libpgquery::T_PGResTarget) {
				throw ParserException("MATCH_RECOGNIZE MEASURES: expected ResTarget node");
			}
			auto &target = PGCast<duckdb_libpgquery::PGResTarget>(*raw_node);
			MeasureDefinition entry;
			entry.expression = TransformExpression(PGPointerCast<duckdb_libpgquery::PGNode>(target.val));
			if (target.name) {
				entry.alias = target.name;
			}
			result->measures.push_back(std::move(entry));
		}
	}

	// Transform WITHIN clause
	if (root.within) {
		result->within = TransformExpression(root.within);
	}
	
	// Transform DEFINE clause — assigns boolean conditions to pattern variables
	// Each definition is a ResTarget: name = variable name, val = condition expression
	if (root.define) {
		for (auto node = root.define->head; node != nullptr; node = node->next) {
			auto raw_node = PGPointerCast<duckdb_libpgquery::PGNode>(node->data.ptr_value);
			if (raw_node->type != duckdb_libpgquery::T_PGResTarget) {
				throw ParserException("MATCH_RECOGNIZE DEFINE: expected ResTarget node");
			}
			auto &target = PGCast<duckdb_libpgquery::PGResTarget>(*raw_node);
			DefineDefinition entry;
			if (target.name) {
				entry.variable_name = target.name;
			}
			entry.condition = TransformExpression(PGPointerCast<duckdb_libpgquery::PGNode>(target.val));
			result->define.push_back(std::move(entry));
		}
	}

	// ROWS PER MATCH — ONE ROW PER MATCH produces one summary row per match,
	// ALL ROWS PER MATCH produces one row for each row in the match
	result->rows_per_match =
	    root.one_row_per_match ? RowsPerMatchType::ONE_ROW_PER_MATCH : RowsPerMatchType::ALL_ROWS_PER_MATCH;

	// AFTER MATCH SKIP — controls where pattern matching resumes after a match
	result->after_match_skip = root.skip_to_next_row ? AfterMatchSkipType::SKIP_TO_NEXT_ROW
	                                                  : AfterMatchSkipType::SKIP_PAST_LAST_ROW;

	// PATTERN — stored as a raw string, parsed later during NFA construction
	if (root.pattern) {
		result->pattern = root.pattern;
	}

	// set alias (if used)
	if (root.alias) {
		result->alias = TransformAlias(root.alias, result->column_name_alias);
	}

	return std::move(result);
}

} // namespace duckdb
