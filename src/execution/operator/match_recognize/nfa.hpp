#ifndef NFA_HPP
#define NFA_HPP

#include "parser.hpp"
#include <vector>
#include <functional>
#include <set>

// Global toggle for NFA debug output (default: off)
extern bool nfa_output_enabled;

struct matchedVar {
    char var;
    int row_idx;
};

enum class TransitionType {
    NONE,   
    VAR,    
    EPSILON   
};

using GuardFn = std::function<bool(const std::vector<matchedVar>&, int)>;

struct Transition {
    TransitionType type;
    int to;
    char var;
    GuardFn guard;

    Transition();
    Transition(TransitionType type, int to, char var = 0, GuardFn guard = GuardFn());
};

struct State {
    int id;

    Transition out1;
    Transition out2;

    State(int id = 0);
};

struct NFA {
    int start;
    int accept;
    
    int next_state_id; 
    std::vector<State> states;

    NFA();
    int new_state();
    void add_transition(int from, const Transition &trans);

    NFA build_eps_NFA();
    NFA build_var_NFA(char var, GuardFn guard = GuardFn());

    NFA build_star_NFA(NFA nfa);
    NFA build_plus_NFA(NFA nfa);
    NFA build_opt_NFA(NFA nfa);

    NFA build_union_NFA(NFA nfa1, NFA nfa2);
    NFA build_concat_NFA(NFA nfa1, NFA nfa2);

    void print() const; 
};

NFA build_from_AST(Node* ast);

struct Run {
    int state;
    std::vector<matchedVar> bindings;

    Run(int state = 0);
};

struct Simulation {
    const NFA &nfa;
    std::vector<Run> currentRuns;
    std::vector<Run> accRuns;

    Simulation(const NFA &nfa);

    void epsilon_closure(std::vector<Run> &currentRuns);
    void print_run(const Run &run);
    void print_results(bool match); 
    bool run(int start_row, int num_rows);
    const Run* find_matches(Simulation &sim, int &start_pos, int num_rows, bool after_match_skip_to_next_row);
};

#endif