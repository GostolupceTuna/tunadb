//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/match_recognize_ref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

enum class RowsPerMatchType : uint8_t {
	ONE_ROW_PER_MATCH = 0,
	ALL_ROWS_PER_MATCH = 1
};

enum class AfterMatchSkipType : uint8_t {
	SKIP_PAST_LAST_ROW = 0,
	SKIP_TO_NEXT_ROW = 1
};

//! Represents a single MEASURES entry: expr AS alias
struct MeasureDefinition {
	unique_ptr<ParsedExpression> expression;
	string alias;

	MeasureDefinition() = default;
	MeasureDefinition(unique_ptr<ParsedExpression> expr, string alias);

	MeasureDefinition Copy() const;
	bool Equals(const MeasureDefinition &other) const;
	
	void Serialize(Serializer &serializer) const;
	static MeasureDefinition Deserialize(Deserializer &source);
};

//! Represents a variable aka a boolean condition that a row must satisfy to be part of a pattern match
struct VariableDefinition {
	string variable_name;
	unique_ptr<ParsedExpression> condition;

	VariableDefinition Copy() const;
	bool Equals(const VariableDefinition &other) const;
};

//! Represents a single DEFINE entry: variable_name AS condition
struct DefineDefinition {
	string variable_name;
	unique_ptr<ParsedExpression> condition;

	DefineDefinition() = default;
	DefineDefinition(string variable_name, unique_ptr<ParsedExpression> condition);

	bool Equals(const DefineDefinition &other) const;
	DefineDefinition Copy() const;

	void Serialize(Serializer &serializer) const;
	static DefineDefinition Deserialize(Deserializer &source);
};

//! Represents a MATCH_RECOGNIZE clause attached to a table source
class MatchRecognizeRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::MATCH_RECOGNIZE;

public:
	MatchRecognizeRef()
	    : TableRef(TableReferenceType::MATCH_RECOGNIZE), rows_per_match(RowsPerMatchType::ONE_ROW_PER_MATCH), after_match_skip(AfterMatchSkipType::SKIP_PAST_LAST_ROW) {
	}

	//! Source table/subquery the clause is attached to
	unique_ptr<TableRef> source;
	//! PARTITION BY expressions
	vector<unique_ptr<ParsedExpression>> partition_by;
	//! ORDER BY expressions
	vector<OrderByNode> order_by;
	//! MEASURES definitions
	vector<MeasureDefinition> measures;
	//! ROWS PER MATCH behavior
	RowsPerMatchType rows_per_match = RowsPerMatchType::ONE_ROW_PER_MATCH;
	//! AFTER MATCH SKIP behavior
	AfterMatchSkipType after_match_skip = AfterMatchSkipType::SKIP_PAST_LAST_ROW;
	//! WITHIN time window
	unique_ptr<ParsedExpression> within;
	//! PATTERN text
	string pattern;
	//! DEFINE definitions
	vector<DefineDefinition> define;

public:
	string ToString() const override;
	bool Equals(const TableRef &other_p) const override;
	unique_ptr<TableRef> Copy() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb