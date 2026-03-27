#include "duckdb/planner/tableref/bound_match_recognize_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_match_recognize.hpp"

namespace duckdb {

// Look up the type and index of a source column by name
LogicalType LookupSourceColumnType(const string &col_name,
                                   const vector<string> &source_columns,
                                   const vector<LogicalType> &source_types,
                                   idx_t &out_index) {
    for (idx_t i = 0; i < source_columns.size(); i++) {
        if (StringUtil::CIEquals(source_columns[i], col_name)) {
            out_index = i;
            return source_types[i];
        }
    }
    throw BinderException("Column '%s' does not exist in source table", col_name);
}

// Determine the output type for a MEASURES aggregate function (sum, count etc.)
LogicalType GetMeasureOutputType(const string &func_name, const LogicalType &input_type) {
    if (func_name == "COUNT") {
        return LogicalType::BIGINT;
    } else if (func_name == "SUM" || func_name == "AVG") {
        return LogicalType::DOUBLE;
    } else {
        // FIRST, LAST, MIN, MAX -> same type as input column
        return input_type;
    }
}

unordered_set<string> ExtractPatternVars(string pattern) {
    unordered_set<string> pattern_vars;
    string var;  
    for (auto c : pattern) {
        // restrict pattern variables to alphabetic characters
        if (isalpha(c)) {
            var += c;
        } 
        else if (c == '*' || c == '+' || c == '?' || c == '|' || c == '(' || c == ')' || c == ' ' ) {
            if (!var.empty()) {
                pattern_vars.insert(var);
                var.clear();
            }
        } 
        else {
            throw SyntaxException("Invalid character '%c' in PATTERN", c);
        }
    }
    if (!var.empty()) {
        pattern_vars.insert(var);
    }

    return pattern_vars;
}

// Recursively walk a parsed expression tree.
// If a node is a two-part column reference like A.val where A is a pattern
// variable and val is a source column, strip the variable prefix so it becomes
// just val (which the binder can resolve against the source table).
void ValidateAndRewriteVarCol(unique_ptr<ParsedExpression> &expr, const unordered_set<string> &pattern_vars) {
    // first recurse into all children
    ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
        ValidateAndRewriteVarCol(child, pattern_vars);
    });

    // then look for var.col expressions
    if (expr->type == ExpressionType::COLUMN_REF) {
        auto &col_ref = expr->Cast<ColumnRefExpression>();
        // column_names.size() == 2 if reference looks like 'var.col'
        if (col_ref.column_names.size() == 2) {
            const auto &var_name = col_ref.column_names[0];
            const auto &col_name = col_ref.column_names[1];

            // Validate that var_name is valid PATTERN variable 
            if (!pattern_vars.count(var_name)) {
                throw BinderException("Unknown pattern variable '%s' in expression", var_name);
            }

            // Replace A.val with just val
            expr = make_uniq<ColumnRefExpression>(col_name);
        }
    }
}

// Recursively walk a parsed expression tree.
// If a node is PREV(col), validate that the argument is a column reference,
// then replace the PREV(col) node with just col.
void ValidateAndRewritePrev(unique_ptr<ParsedExpression> &expr) {
    // first recurse into all children
    ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
        ValidateAndRewritePrev(child);
    });

    // then check if this node itself is PREV(...)
    if (expr->type == ExpressionType::FUNCTION) {
        auto &func = expr->Cast<FunctionExpression>(); 
        if (StringUtil::Lower(func.function_name) == "prev") {
            if (func.children.size() != 1) {
                throw BinderException("PREV() requires exactly one argument");
            }
            if (func.children[0]->type != ExpressionType::COLUMN_REF) {
                throw BinderException("PREV() argument must be a column reference");
            }
            expr = std::move(func.children[0]);
        }
    }
}

BoundStatement Binder::Bind(MatchRecognizeRef &ref) {
    // bind the child table in a child binder
	BoundMatchRecognizeRef result;
	result.bind_index = GenerateTableIndex();
	result.child_binder = Binder::CreateBinder(context, this);
	result.child = result.child_binder->Bind(*ref.source);

    // bind rows per match, skip logic, pattern
    result.bound_mr.one_row_per_match = (ref.rows_per_match == RowsPerMatchType::ONE_ROW_PER_MATCH);
    result.bound_mr.skip_to_next_row = (ref.after_match_skip == AfterMatchSkipType::SKIP_TO_NEXT_ROW);
	result.bound_mr.pattern = ref.pattern;

    // bind PARTITION BY
    ExpressionBinder expr_binder(*result.child_binder, context);

    for (auto &expr : ref.partition_by) {
        unique_ptr<Expression> bound_expr;
        try {
            bound_expr = expr_binder.Bind(expr);
        } catch (const Exception &ex) {
            throw BinderException("Unknown column %s in PARTITION BY: %s", expr->ToString(), ex.what());
        }
        result.bound_mr.partition_by.push_back(std::move(bound_expr));
    }
   
    // bind ORDER BY
    for (auto &order : ref.order_by) {
        unique_ptr<Expression> bound_expr;
        try {
            bound_expr = expr_binder.Bind(order.expression);
        } catch (const Exception &ex) {
            throw BinderException("Unknown column %s in ORDER BY: %s", order.expression->ToString(), ex.what());
        }
        result.bound_mr.order_by.emplace_back(order.type, order.null_order, std::move(bound_expr));
    }

    // extract all variables from PATTERN
    unordered_set<string> pattern_vars = ExtractPatternVars(ref.pattern);

    // bind DEFINE
    for (auto &define : ref.define) {
        // Validate DEFINE: for each 'DEFINE X AS …', check if X is contained in PATTERN
        const auto &var = define.variable_name;
        if (!pattern_vars.count(define.variable_name)) {
            throw BinderException("DEFINE variable '%s' does not appear in PATTERN", var);
        }

        // validate and rewrite references like 'var.col' (e.g. A.val → val)  before binding
        ValidateAndRewriteVarCol(define.condition, pattern_vars);

        // validate and rewrite PREV(col) → col before binding
        ValidateAndRewritePrev(define.condition);

        unique_ptr<Expression> bound_expr;
        try {
            bound_expr = expr_binder.Bind(define.condition);
        } catch (const Exception &ex) {
            throw BinderException("Invalid DEFINE condition '%s' for variable '%s': %s", define.condition->ToString(), define.variable_name.c_str(), ex.what());
        }

        // type check: each DEFINE condition must be a boolean expression
        if (bound_expr->return_type != LogicalType::BOOLEAN) {
            throw BinderException("DEFINE condition for '%s' must return BOOLEAN, got %s", define.variable_name.c_str(), bound_expr->return_type.ToString());
        }

        result.bound_mr.defines.emplace_back(define.variable_name, std::move(bound_expr));
    }

    // bind MEASURES: each must be var.col or FUNC(var.col) AS alias
    const unordered_set<string> allowed_functions = {
        "FIRST", "LAST", "COUNT", "MIN", "MAX", "SUM", "AVG"
    };

    for (auto &measure : ref.measures) {
        auto &expr = measure.expression;
        string func_name;
        string var_name;
        string col_name;

        if (expr->type == ExpressionType::COLUMN_REF) {
            // bare var.col form
            auto &col_ref = expr->Cast<ColumnRefExpression>();
            if (col_ref.column_names.size() != 2) {
                throw BinderException("MEASURES expression '%s' must be var.col or FUNC(var.col)",
                                      expr->ToString());
            }
            var_name = col_ref.column_names[0];
            col_name = col_ref.column_names[1];

        } else if (expr->type == ExpressionType::FUNCTION) {
            // FUNC(var.col) form (e.g. LAST(B.totalprice) AS bottom_price)
            auto &func = expr->Cast<FunctionExpression>();
            func_name = StringUtil::Upper(func.function_name);

            if (!allowed_functions.count(func_name)) {
                throw BinderException("Unknown MEASURES function '%s'. Allowed: FIRST, LAST, COUNT, MIN, MAX, SUM, AVG",
                                      func.function_name);
            }
            if (func.children.size() != 1) {
                throw BinderException("MEASURES function '%s' requires exactly one argument", func.function_name);
            }
            if (func.children[0]->type != ExpressionType::COLUMN_REF) {
                throw BinderException("MEASURES function '%s' argument must be a column reference (var.col)",
                                      func.function_name);
            }

            auto &arg = func.children[0]->Cast<ColumnRefExpression>();
            if (arg.column_names.size() != 2) {
                throw BinderException("MEASURES function '%s' argument must be var.col (e.g. A.price)",
                                      func.function_name);
            }
            var_name = arg.column_names[0];
            col_name = arg.column_names[1];

        } else {
            throw BinderException("MEASURES expression '%s' must be var.col or FUNC(var.col)",
                                  expr->ToString());
        }

        // validate pattern variable
        if (!pattern_vars.count(var_name)) {
            throw BinderException("Unknown pattern variable '%s' in MEASURES expression '%s'",
                                  var_name, expr->ToString());
        }

        // resolve column type and index
        unique_ptr<ParsedExpression> col_ref_parsed = make_uniq<ColumnRefExpression>(col_name);
        unique_ptr<Expression> bound_col;
        try {
            bound_col = expr_binder.Bind(col_ref_parsed);
        } catch (const Exception &ex) {
            throw BinderException("MEASURES: column '%s' does not exist in source table: %s", col_name, ex.what());
        }
        auto &bound_ref = bound_col->Cast<BoundColumnRefExpression>();
        idx_t col_idx = bound_ref.binding.column_index;
        LogicalType input_type = bound_ref.return_type;

        // determine output type
        LogicalType output_type = func_name.empty() ? input_type : GetMeasureOutputType(func_name, input_type);

        result.bound_mr.measures.emplace_back(func_name, var_name, col_name,
                                     input_type, measure.alias, output_type, col_idx);
    }

    // build output schema from MEASURES columns
    vector<string> names;
    vector<LogicalType> types;
    for (auto &m : result.bound_mr.measures) {
        names.push_back(m.output_name);
        types.push_back(m.output_type);
    }

    result.bound_mr.names = names;
    result.bound_mr.types = types;

    auto alias = ref.alias.empty() ? "__unnamed_match_recognize" : ref.alias;
    bind_context.AddGenericBinding(result.bind_index, alias, names, types);
    MoveCorrelatedExpressions(*result.child_binder);

    BoundStatement result_statement;
    result_statement.names = std::move(names);
    result_statement.types = std::move(types);

    auto logical_mr = make_uniq<LogicalMatchRecognize>(result.bind_index, std::move(result.bound_mr));
    logical_mr->children.push_back(std::move(result.child.plan));
    result_statement.plan = std::move(logical_mr);

    return result_statement;
}

} // namespace duckdb
