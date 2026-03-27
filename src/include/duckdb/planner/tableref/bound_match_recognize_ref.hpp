//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/tableref/bound_match_recognize_ref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

struct BoundDefine {
	string variable_name;
	unique_ptr<Expression> condition;

	BoundDefine() = default;
	BoundDefine(string variable_name, unique_ptr<Expression> condition)
		: variable_name(std::move(variable_name)), condition(std::move(condition)) {
	}

	// Deep-copy constructor: BoundDefine owns a unique_ptr<Expression>, so we must clone the expression tree to avoid copying unique_ptrs.
	BoundDefine(const BoundDefine &other)
	    : variable_name(other.variable_name),
	      condition(other.condition ? other.condition->Copy() : nullptr) {
	}

	// Deep-copy assignment: same reason as above; replace current owned expression with a clone.
	BoundDefine &operator=(const BoundDefine &other) {
		if (this == &other) return *this;
		variable_name = other.variable_name;
		condition = other.condition ? other.condition->Copy() : nullptr;
		return *this;
	}

	BoundDefine Copy() const {
		return BoundDefine(variable_name, condition ? condition->Copy() : nullptr);
	}
	void Serialize(Serializer &serializer) const;
	static BoundDefine Deserialize(Deserializer &deserializer);
};

struct BoundMeasure {
	//! Aggregate function name: FIRST, LAST, COUNT, MIN, MAX, SUM, AVG
	string function_name;
	//! The referenced PATTERN variable (e.g. "A", "B")
	string pattern_variable;
	//! The referenced source column name (e.g. "id", "price")
	string input_column;
	//! The type of the source column
	LogicalType input_type;
	//! The output column name (the AS alias)
	string output_name;
	//! The output type (COUNT→BIGINT, SUM/AVG→DOUBLE, FIRST/LAST/MIN/MAX→input type)
	LogicalType output_type;
	//! The physical column index in the input partition
	idx_t input_column_index = 0;

	BoundMeasure() = default;
	BoundMeasure(string function_name, string pattern_variable, string input_column,
	             LogicalType input_type, string output_name, LogicalType output_type, idx_t input_column_index = 0)
	    : function_name(std::move(function_name)), pattern_variable(std::move(pattern_variable)),
	      input_column(std::move(input_column)), input_type(std::move(input_type)),
	      output_name(std::move(output_name)), output_type(std::move(output_type)),
		  input_column_index(input_column_index) {
	}

	// default copy is fine
	BoundMeasure(const BoundMeasure &) = default;
	BoundMeasure &operator=(const BoundMeasure &) = default;

	BoundMeasure Copy() const {
		return *this;
	}
	void Serialize(Serializer &serializer) const;
	static BoundMeasure Deserialize(Deserializer &deserializer);
};

struct BoundMatchRecognizeInfo {
	//! The bound PARTITION BY keys
	vector<unique_ptr<Expression>> partition_by;
	//! The bound ORDER BY keys
	vector<BoundOrderByNode> order_by;
	//! The bound DEFINE entries
	vector<BoundDefine> defines;
	//! The output schema for matches: ONE ROW PER MATCH or ALL ROWS PER MATCH
	bool one_row_per_match = true;
	//! The skip logic: AFTER MATCH SKIP PAST LAST ROW or TO NEXT ROW
	bool skip_to_next_row = false;
	//! The pattern string
	string pattern;
	//! The bound MEASURES entries
	vector<BoundMeasure> measures;
	//! The schema currently exported by the relation
	vector<string> names;
	vector<LogicalType> types;

	BoundMatchRecognizeInfo() = default;

	// Deep-copy constructor: required because this struct owns unique_ptr<Expression>.
	// We clone the expression trees so each instance has independent ownership.
	BoundMatchRecognizeInfo(const BoundMatchRecognizeInfo &other) {
		for (auto &p : other.partition_by) {
			partition_by.push_back(p ? p->Copy() : nullptr);
		}
		for (auto &o : other.order_by) {
			order_by.push_back(o.Copy());
		}
		for (auto &d : other.defines) {
			defines.push_back(d);
		}
		measures = other.measures;
		one_row_per_match = other.one_row_per_match;
		skip_to_next_row = other.skip_to_next_row;
		pattern = other.pattern;
		names = other.names;
		types = other.types;
	}

	// Deep-copy assignment: same reason as above; replaces owned data with cloned copies to avoid sharing or copying unique_ptrs.
	BoundMatchRecognizeInfo &operator=(const BoundMatchRecognizeInfo &other) {
		if (this == &other) return *this;
		partition_by.clear();
		order_by.clear();
		defines.clear();

		for (auto &p : other.partition_by) {
			partition_by.push_back(p ? p->Copy() : nullptr);
		}
		for (auto &o : other.order_by) {
			order_by.push_back(o.Copy());
		}
		for (auto &d : other.defines) {
			defines.push_back(d);
		}
		measures = other.measures;
		one_row_per_match = other.one_row_per_match;
		skip_to_next_row = other.skip_to_next_row;
		pattern = other.pattern;
		names = other.names;
		types = other.types;
		return *this;
	}

	BoundMatchRecognizeInfo Copy() const {
		BoundMatchRecognizeInfo copy;
		for (auto &p : partition_by) {
			copy.partition_by.push_back(p ? p->Copy() : nullptr);
		}
		for (auto &o : order_by) {
			copy.order_by.push_back(o.Copy());
		}
		for (auto &d : defines) {
			copy.defines.push_back(d.Copy());
		}
		for (auto &m : measures) {
			copy.measures.push_back(m.Copy());
		}
		copy.one_row_per_match = one_row_per_match;
		copy.skip_to_next_row = skip_to_next_row;
		copy.pattern = pattern;
		copy.names = names;
		copy.types = types;
		return copy;
	}
	void Serialize(Serializer &serializer) const;
	static BoundMatchRecognizeInfo Deserialize(Deserializer &deserializer);
};

class BoundMatchRecognizeRef {
public:
	//! The bind index exported by the bound source relation
	idx_t bind_index;
	//! The binder used to bind the input relation and match-recognize keys
	shared_ptr<Binder> child_binder;
	//! The bound source relation
	BoundStatement child;
	//! The bound match recognize info
	BoundMatchRecognizeInfo bound_mr;
};

} // namespace duckdb
