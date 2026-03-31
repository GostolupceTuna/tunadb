#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/common/serializer/serializer.hpp" 
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// ===== MeasureDefinition =====

MeasureDefinition::MeasureDefinition(unique_ptr<ParsedExpression> expression, string alias)
    : expression(std::move(expression)), alias(std::move(alias)) {
}

bool MeasureDefinition::Equals(const MeasureDefinition &other) const {
	return alias == other.alias && ParsedExpression::Equals(expression, other.expression);
}

MeasureDefinition MeasureDefinition::Copy() const {
	MeasureDefinition result;
	result.expression = expression ? expression->Copy() : nullptr;
	result.alias = alias;
	return result;
}

void MeasureDefinition::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(1, "expression", expression);
	serializer.WritePropertyWithDefault<string>(2, "alias", alias);
}

MeasureDefinition MeasureDefinition::Deserialize(Deserializer &deserializer) {
	MeasureDefinition result;
	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(1, "expression", result.expression);
	deserializer.ReadPropertyWithDefault<string>(2, "alias", result.alias);
	return result;
}

// ===== VariableDefinition =====

VariableDefinition VariableDefinition::Copy() const {
	VariableDefinition result;
	result.variable_name = variable_name;
	result.condition = condition ? condition->Copy() : nullptr;
	return result;
}

bool VariableDefinition::Equals(const VariableDefinition &other) const {
	return variable_name == other.variable_name && ParsedExpression::Equals(condition, other.condition);
}

// ===== DefineDefinition =====

DefineDefinition::DefineDefinition(string variable_name, unique_ptr<ParsedExpression> condition)
    : variable_name(std::move(variable_name)), condition(std::move(condition)) {
}

bool DefineDefinition::Equals(const DefineDefinition &other) const {
	if (variable_name != other.variable_name) {
		return false;
	}
	if (!ParsedExpression::Equals(condition, other.condition)) {
		return false;
	}
	return true;
}

DefineDefinition DefineDefinition::Copy() const {
	DefineDefinition result;
	result.variable_name = variable_name;
	result.condition = condition ? condition->Copy() : nullptr;
	return result;
}

void DefineDefinition::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<string>(1, "variable_name", variable_name);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(2, "condition", condition);
}

DefineDefinition DefineDefinition::Deserialize(Deserializer &source) {
	DefineDefinition result;
	source.ReadPropertyWithDefault<string>(1, "variable_name", result.variable_name);
	source.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(2, "condition", result.condition);
	return result;
}

// ===== MatchRecognizeRef =====

static string ToStringOrderBy(const vector<OrderByNode> &orders) {
	string result;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += orders[i].ToString();
	}
	return result;
}

static string ToStringExpressionList(const vector<unique_ptr<ParsedExpression>> &expressions) {
	string result;
	for (idx_t i = 0; i < expressions.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		auto &expr = expressions[i];
		result += expr->ToString();
		if (!expr->GetAlias().empty()) {
			result += " AS " + KeywordHelper::WriteOptionallyQuoted(expr->GetAlias());
		}
	}
	return result;
}

static string ToStringMeasures(const vector<MeasureDefinition> &measures) {
	string result;
	for (idx_t i = 0; i < measures.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += measures[i].expression->ToString();
		if (!measures[i].alias.empty()) {
			result += " AS " + KeywordHelper::WriteOptionallyQuoted(measures[i].alias);
		}
	}
	return result;
}

static string ToStringDefine(const vector<DefineDefinition> &definitions) { // Typ geändert!
    string result;
    for (idx_t i = 0; i < definitions.size(); i++) {
        if (i > 0) {
            result += ", ";
        }
        result += KeywordHelper::WriteOptionallyQuoted(definitions[i].variable_name);
        result += " AS ";
        result += definitions[i].condition->ToString();
    }
    return result;
}

static void AppendClause(string &result, bool &has_clause, const string &clause) {
	if (has_clause) {
		result += " ";
	}
	result += clause;
	has_clause = true;
}

string MatchRecognizeRef::ToString() const {
	string result = source ? source->ToString() : "<missing source>";
	result += " MATCH_RECOGNIZE(";

	bool has_clause = false;

	if (!partition_by.empty()) {
		AppendClause(result, has_clause, "PARTITION BY " + ToStringExpressionList(partition_by));
	}
	if (!order_by.empty()) {
		AppendClause(result, has_clause, "ORDER BY " + ToStringOrderBy(order_by));
	}
	if (!measures.empty()) {
		AppendClause(result, has_clause, "MEASURES " + ToStringMeasures(measures));
	}
	AppendClause(result, has_clause,
	             rows_per_match == RowsPerMatchType::ONE_ROW_PER_MATCH ? "ONE ROW PER MATCH" : "ALL ROWS PER MATCH");
	AppendClause(result, has_clause, after_match_skip == AfterMatchSkipType::SKIP_TO_NEXT_ROW ? "AFTER MATCH SKIP TO NEXT ROW"
	                                                                                           : "AFTER MATCH SKIP PAST LAST ROW");
	if (within) {
		AppendClause(result, has_clause, "WITHIN " + within->ToString());
	}
	if (!pattern.empty()) {
		AppendClause(result, has_clause, "PATTERN ('" + StringUtil::Replace(pattern, "'", "''") + "')");
	}
	if (!define.empty()) {
		AppendClause(result, has_clause, "DEFINE " + ToStringDefine(define));
	}
	
	result += ")";
	return BaseToString(result);
}

bool MatchRecognizeRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<MatchRecognizeRef>();
	if (!source->Equals(*other.source)) {
		return false;
	}
	if (!ParsedExpression::ListEquals(partition_by, other.partition_by)) {
		return false;
	}
	if (!TableRef::Equals(source, other.source)) {
		return false;
	}
	if (order_by.size() != other.order_by.size()) {
		return false;
	}
	for (idx_t i = 0; i < order_by.size(); i++) {
		if (order_by[i].type != other.order_by[i].type || order_by[i].null_order != other.order_by[i].null_order) {
			return false;
		}
		if (!ParsedExpression::Equals(order_by[i].expression, other.order_by[i].expression)) {
			return false;
		}
	}
	if (measures.size() != other.measures.size()) {
		return false;
	}
	for (idx_t i = 0; i < measures.size(); i++) {
		if (!measures[i].Equals(other.measures[i])) {
			return false;
		}
	}
	if (define.size() != other.define.size()) {
		return false;
	}
	for (idx_t i = 0; i < define.size(); i++) {
		if (!define[i].Equals(other.define[i])) {
			return false;
		}
	}
	if (!ParsedExpression::Equals(within, other.within)) {
		return false;
	}

	return rows_per_match == other.rows_per_match && after_match_skip == other.after_match_skip && pattern == other.pattern;
}

unique_ptr<TableRef> MatchRecognizeRef::Copy() {
	auto copy = make_uniq<MatchRecognizeRef>();
	copy->source = source ? source->Copy() : nullptr;
	for (auto &expr : partition_by) {
		copy->partition_by.push_back(expr->Copy());
	}
	for (auto &order : order_by) {
		copy->order_by.emplace_back(order.type, order.null_order, order.expression->Copy());
	}
	for (auto &measure : measures) {
		copy->measures.push_back(measure.Copy());
	}
	copy->rows_per_match = rows_per_match;  
	copy->after_match_skip = after_match_skip;
	copy->within = within ? within->Copy() : nullptr;
	copy->pattern = pattern;
	for (auto &entry : define) {
		copy->define.push_back(entry.Copy());
	}
	copy->alias = alias;
	CopyProperties(*copy);
	return std::move(copy);
}

} // namespace duckdb