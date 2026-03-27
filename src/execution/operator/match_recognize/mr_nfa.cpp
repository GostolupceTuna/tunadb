#include "duckdb/execution/operator/match_recognize/mr_nfa.hpp"

#include "nfa.hpp"

#include <iostream>

#define SHINY_CYAN "\033[1;38;2;0;255;255m"
#define RESET_COLOR "\033[0m"

namespace duckdb {

vector<MRMatchResult> MRRunPatternMatching(const string &pattern, idx_t num_rows, const unordered_map<string, vector<bool>> &var_masks, bool skip_to_next_row) {
	Lexer lexer(pattern);
    Parser parser(lexer);
    Node* ast = parser.parse_pattern();
    NFA nfa = build_from_AST(ast);
    delete(ast);

	// attach guard functions
	for (State &state : nfa.states) {
		auto attach_guard = [&](Transition &trans) {
			// no need to look at empty transitions
			if (trans.type != TransitionType::VAR) {
				return;
			}
			std::string var_name(1, trans.var);
			auto iterator = var_masks.find(var_name);
			// we assume that wildcards are explicitly defined 
			if (iterator == var_masks.end()) {
				throw InternalException("MATCH_RECOGNIZE: pattern variable '%s' has no DEFINE entry", var_name);
			}
			const std::vector<bool> *mask = &iterator->second;
			trans.guard = [mask](const std::vector<matchedVar> &, int row_idx) {
				return 
					row_idx >= 0 &&	static_cast<size_t>(row_idx) < mask->size() &&	// valid index?
					(*mask)[static_cast<size_t>(row_idx)];							// value == true?
			};
		};

		attach_guard(state.out1);
		attach_guard(state.out2);
	}

	// run matching loop
	vector<MRMatchResult> results;
	int start_pos = 0;
	int end_pos = static_cast<int>(num_rows);

	while (start_pos < end_pos) {
		std::cout << SHINY_CYAN << "Starting from ROW " << start_pos << RESET_COLOR << "\n";
		Simulation sim(nfa);
		const Run *longest_match = sim.find_matches(sim, start_pos, end_pos, skip_to_next_row);
		if (!longest_match) {
			continue;
		}

		// convert Run bindings to MRMatchResult
		MRMatchResult match_result;
		for (const auto &binding : longest_match->bindings) {
			match_result.assignment[std::string(1, binding.var)].push_back(static_cast<idx_t>(binding.row_idx));
		}
		
		match_result.match_end = static_cast<idx_t>(longest_match->bindings.back().row_idx + 1);	// last binding holds the last row
		results.push_back(std::move(match_result));
	}

	return results;
}

} // namespace duckdb
