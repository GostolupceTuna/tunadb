#include "duckdb/planner/tableref/bound_match_recognize_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_match_recognize.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

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

// Return true if the parsed expression tree contains any PREV() call.
static bool HasPrevCall(ParsedExpression &expr) {
    if (expr.type == ExpressionType::FUNCTION) {
        auto &func = expr.Cast<FunctionExpression>();
        if (StringUtil::Lower(func.function_name) == "prev") {
            return true;
        }
    }
    bool found = false;
    ParsedExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<ParsedExpression> &child) {
        if (!found) {
            found = HasPrevCall(*child);
        }
    });
    return found;
}

// Walk a parsed expression tree and replace every PREV(col) node with a virtual
// column reference __prev__col__ that resolves against the virtual prev table.
// Must be called AFTER ValidateAndRewriteVarCol, so col refs are single-part.
static void RewritePrevToVirtualPrevColRef(unique_ptr<ParsedExpression> &expr) {
    // recurse first so children are processed before the parent check
    ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
        RewritePrevToVirtualPrevColRef(child);
    });

    if (expr->type == ExpressionType::FUNCTION) {
        auto &func = expr->Cast<FunctionExpression>();
        if (StringUtil::Lower(func.function_name) == "prev") {
            D_ASSERT(func.children.size() == 1);
            D_ASSERT(func.children[0]->type == ExpressionType::COLUMN_REF);
            auto &col_ref = func.children[0]->Cast<ColumnRefExpression>();
            string col_name = col_ref.column_names.back();
            expr = make_uniq<ColumnRefExpression>("__prev__" + col_name + "__");
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

    // bind WITHIN (if it exists, is optional)
    if (ref.within) {
        unique_ptr<Expression> bound_within;
        try {
            bound_within = expr_binder.Bind(ref.within);
        } catch (const Exception &ex) {
            throw BinderException("Invalid WITHIN expression: %s", ex.what());
        }
        if (bound_within->return_type != LogicalType::INTERVAL) {
            throw BinderException("WITHIN must be an INTERVAL expression");
        }
        // within only makes sense if we find matches in time-sorted data
        if (result.bound_mr.order_by.empty()) {
            throw BinderException("WITHIN requires ORDER BY");
        }
        // we assume first ORDER BY is time related
        auto &order_type = result.bound_mr.order_by[0].expression->return_type;
        auto order_id = order_type.id();
        if (!(order_id == LogicalTypeId::DATE || order_id == LogicalTypeId::TIME ||
            order_id == LogicalTypeId::TIME_TZ || order_id == LogicalTypeId::TIMESTAMP ||
            order_id == LogicalTypeId::TIMESTAMP_TZ)) {
            throw BinderException("WITHIN requires ORDER BY on a time or date column");
        }
        result.bound_mr.within = std::move(bound_within);
    }

    // extract all variables from PATTERN
    unordered_set<string> pattern_vars = ExtractPatternVars(ref.pattern);

    // Set up a virtual prev table so PREV(col) can be bound to a separate column index.
    // We register vritual column names __prev__col__ in the child binder's context.
    // These are used only when building prev_exec_condition and are never visible to optimizers.
    idx_t N = result.child.types.size();
    idx_t prev_table_idx = result.child_binder->GenerateTableIndex();
    result.bound_mr.prev_table_idx = prev_table_idx;
    result.bound_mr.num_source_cols = N;

    vector<string> prev_col_names;
    vector<LogicalType> prev_col_types;
    for (idx_t i = 0; i < N; i++) {
        prev_col_names.push_back("__prev__" + result.child.names[i] + "__");
        prev_col_types.push_back(result.child.types[i]);
    }
    result.child_binder->bind_context.AddGenericBinding(prev_table_idx, "__prev_table__",
                                                        prev_col_names, prev_col_types);

    // bind DEFINE
    for (auto &define : ref.define) {
        // Validate DEFINE: for each 'DEFINE X AS …', check if X is contained in PATTERN
        const auto &var = define.variable_name;
        if (!pattern_vars.count(define.variable_name)) {
            throw BinderException("DEFINE variable '%s' does not appear in PATTERN", var);
        }

        for ( auto c : pattern_vars) {
            bool isFound = false;
            for (auto &define : ref.define) {
                const auto &var = define.variable_name;
                if (c == var) {
                    isFound = true;
                    break;
                }
            }
            if (!isFound) {
                throw BinderException("PATTERN variable '%s' does not appear in DEFINE", c);
            }
        }

        // validate and rewrite references like 'var.col' (e.g. A.val → val) before binding
        ValidateAndRewriteVarCol(define.condition, pattern_vars);

        BoundDefine bound_define;
        bound_define.variable_name = define.variable_name;

        // If the condition contains PREV(), build a separate prev_exec_condition where
        // PREV(col) references are already resolved to BoundReferenceExpression(N+k).
        if (HasPrevCall(*define.condition)) {
            bound_define.uses_prev = true;
            auto prev_copy = define.condition->Copy();
            RewritePrevToVirtualPrevColRef(prev_copy);
            unique_ptr<Expression> prev_exec_bound;
            try {
                prev_exec_bound = expr_binder.Bind(prev_copy);
            } catch (const Exception &ex) {
                throw BinderException("Invalid PREV expression in DEFINE '%s': %s",
                                      define.variable_name.c_str(), ex.what());
            }
            // Leave as BoundColumnRefExpression; plan_match_recognize.cpp will resolve
            // these to BoundReferenceExpression using the post-optimization column bindings
            // so that column pruning by RemoveUnusedColumns is accounted for.
            bound_define.prev_exec_condition = std::move(prev_exec_bound);
        }

        // validate and rewrite PREV(col) → col before binding
        ValidateAndRewritePrev(define.condition);

        // NULL in DEFINE counts as false: coalesce(cond, false)
        vector<unique_ptr<ParsedExpression>> coalesce_children;
        coalesce_children.push_back(std::move(define.condition));
        coalesce_children.push_back(make_uniq_base<ParsedExpression, ConstantExpression>(Value::BOOLEAN(false)));
        define.condition = make_uniq_base<ParsedExpression, OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_children));

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

        bound_define.condition = std::move(bound_expr);
        result.bound_mr.defines.push_back(std::move(bound_define));
    }

    // bind MEASURES: each must be var.col or FUNC(var.col) AS alias
    const unordered_set<string> allowed_functions = {
        "FIRST", "LAST", "COUNT", "COUNT_STAR", "MIN", "MAX", "SUM", "AVG"
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
            if (func_name == "COUNT_STAR") {
                result.bound_mr.measures.emplace_back(func_name, "", "*", LogicalType::BIGINT, measure.alias,
                                                      LogicalType::BIGINT, 0);
                continue;
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

        // COUNT(var.*): count rows bound to var, no column binding needed
        if (func_name == "COUNT" && col_name == "*") {
            result.bound_mr.measures.emplace_back(func_name, var_name, col_name, LogicalType::BIGINT, measure.alias, LogicalType::BIGINT, 0);
            continue;
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
