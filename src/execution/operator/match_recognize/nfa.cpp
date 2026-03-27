#include <iostream>
#include <queue>
#include "nfa.hpp"
#include <string>

#define SHINY_RED "\033[1;38;2;255;0;0m"
#define SHINY_GREEN "\033[1;38;2;0;255;0m"
#define RESET_COLOR "\033[0m"


Transition::Transition()
    : type(TransitionType::NONE), to(-1), var(0), guard(GuardFn()) {}

Transition::Transition(TransitionType type, int to, char var, GuardFn guard)
    : type(type), to(to), var(var), guard(guard) {}

State::State(int id)
    : id(id), out1(), out2() {}

NFA::NFA()
    : start(-1), accept(-1), next_state_id(0) {}

int NFA::new_state() {
    states.emplace_back(next_state_id);
    return next_state_id++;
}

void NFA::add_transition(int from, const Transition &trans) {
    if (from < 0 || from >= (int)states.size()) {
        throw std::runtime_error("Invalid from-state: " + std::to_string(from)); 
    }
    State &state = states[static_cast<size_t>(from)];    

    if (state.out1.type == TransitionType::NONE) {
        state.out1 = trans; 
    } else if (state.out2.type == TransitionType::NONE) {
        state.out2 = trans; 
    } else {
        throw std::runtime_error("State " + std::to_string(from) + " already has 2 outgoing transitions");
    }     
}

NFA NFA::build_eps_NFA() {
    NFA resultNFA;
    resultNFA.new_state();
    resultNFA.new_state();
    
    resultNFA.add_transition(0, Transition(TransitionType::EPSILON, 1));

    resultNFA.start = 0;
    resultNFA.accept = 1;

    return resultNFA;
}

NFA NFA::build_var_NFA(char var, GuardFn guard) {
    NFA resultNFA;
    resultNFA.new_state();
    resultNFA.new_state();

    resultNFA.add_transition(0, Transition(TransitionType::VAR, 1, var, guard));

    resultNFA.start = 0;
    resultNFA.accept = 1;

    return resultNFA;
}

NFA NFA::build_star_NFA(NFA nfa) {
    NFA resultNFA;
    resultNFA.new_state();

    // copy nfa to the resultNFA, push all transition id's by one
    for(auto state: nfa.states) {
        int id = resultNFA.new_state();
        resultNFA.states[static_cast<size_t>(id)] = state;
        if(resultNFA.states[static_cast<size_t>(id)].out1.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out1.to++;
        if(resultNFA.states[static_cast<size_t>(id)].out2.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out2.to++;
        resultNFA.states[static_cast<size_t>(id)].id = id;
    }

    // add new end state
    int acceptID = resultNFA.new_state();

    // add e-transitions
    resultNFA.add_transition(0, Transition(TransitionType::EPSILON, 1));
    resultNFA.add_transition(0, Transition(TransitionType::EPSILON, acceptID));
    resultNFA.add_transition(acceptID - 1, Transition(TransitionType::EPSILON, 1));
    resultNFA.add_transition(acceptID - 1, Transition(TransitionType::EPSILON, acceptID));

    resultNFA.start = 0;
    resultNFA.accept = acceptID;

    return resultNFA;
}
     
NFA NFA::build_union_NFA(NFA nfa1, NFA nfa2) {
    NFA resultNFA;
    resultNFA.new_state();

    // copy nfa1 to the resultNFA, push all transition id's by one
    for(auto state: nfa1.states) {
        int id = resultNFA.new_state();
        resultNFA.states[static_cast<size_t>(id)] = state;
        if(resultNFA.states[static_cast<size_t>(id)].out1.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out1.to++;
        if(resultNFA.states[static_cast<size_t>(id)].out2.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out2.to++;
        resultNFA.states[static_cast<size_t>(id)].id = id;
    }

    int offset = static_cast<int>(nfa1.states.size()) + 1;    // +1 because of the start state at the beginning
    
    for(auto state: nfa2.states) {
        int id = resultNFA.new_state();
        resultNFA.states[static_cast<size_t>(id)] = state;
        if(resultNFA.states[static_cast<size_t>(id)].out1.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out1.to += offset;
        if(resultNFA.states[static_cast<size_t>(id)].out2.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out2.to += offset;
        resultNFA.states[static_cast<size_t>(id)].id = id;
    }

    // the e-transitions from the start state
    resultNFA.add_transition(0, Transition(TransitionType::EPSILON, 1));
    resultNFA.add_transition(0, Transition(TransitionType::EPSILON, offset));
    
    // the e-transitions that lead to the end state
    int acceptID = resultNFA.new_state();
    resultNFA.add_transition(offset-1, Transition(TransitionType::EPSILON, acceptID));
    resultNFA.add_transition(acceptID-1, Transition(TransitionType::EPSILON, acceptID));

    resultNFA.start = 0;
    resultNFA.accept = acceptID;

    return resultNFA;
}

// the first states from nfa1 and the following states are from nfa2
// start state is the 1st state of nfa1 and accept state is the last state of nfa2
NFA NFA::build_concat_NFA(NFA nfa1, NFA nfa2) {
    NFA resultNFA;
    int offset = static_cast<int>(nfa1.states.size());

    // copy nfa1 to resultNFA
    for(auto state : nfa1.states) {
        int id = resultNFA.new_state();
        resultNFA.states[static_cast<size_t>(id)] = state;
        resultNFA.states[static_cast<size_t>(id)].id = id;
    }

    // at this point of code, id points to the last state of nfa1, which should be the first state of nfa2
    int id = offset - 1;

    // copy nfa2 to resultNFA
    for(auto state: nfa2.states) {
        resultNFA.states[static_cast<size_t>(id)] = state;
        if(resultNFA.states[static_cast<size_t>(id)].out1.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out1.to += offset - 1;
        if(resultNFA.states[static_cast<size_t>(id)].out2.type != TransitionType::NONE) resultNFA.states[static_cast<size_t>(id)].out2.to += offset - 1;
        resultNFA.states[static_cast<size_t>(id)].id = id;

        id = resultNFA.new_state();     // this loop adds the new state at the end, because we use the last defined state from the previous loop
    }

    resultNFA.states.pop_back();    // because we add the new state at the end of the loop, the last added state is unused

    resultNFA.start = 0;
    resultNFA.accept = id - 1;

    return resultNFA;
}

NFA NFA::build_plus_NFA(NFA nfa) {
    return build_concat_NFA(nfa, build_star_NFA(nfa));
}

NFA NFA::build_opt_NFA(NFA nfa) {
    return build_union_NFA(build_eps_NFA(), nfa);
}

NFA build_from_AST(Node* node) {
    if (!node) {
        throw std::runtime_error("Null AST node");
    }
        
    NFA builder;

    switch (node->type) {
        case NodeType::VAR: {
            return builder.build_var_NFA(node->value);
        } 
        case NodeType::CONCAT: {
            NFA left = build_from_AST(node->left);
            NFA right = build_from_AST(node->right);
            return builder.build_concat_NFA(left, right);
        } 
        case NodeType::ALT: {
            NFA left = build_from_AST(node->left);
            NFA right = build_from_AST(node->right);
            return builder.build_union_NFA(left, right);
        } 
        case NodeType::STAR: {
            NFA atom = build_from_AST(node->left);
            return builder.build_star_NFA(atom);
        } 
        case NodeType::PLUS: {
            NFA atom = build_from_AST(node->left);
            return builder.build_plus_NFA(atom);
        } 
        case NodeType::OPTIONAL: {
            NFA atom = build_from_AST(node->left);
            return builder.build_opt_NFA(atom);
        } 
        default: {
            throw std::runtime_error("Unknown AST node type");
        }   
    }
}

void print_transition(const Transition &trans) {
    switch (trans.type) {
        case TransitionType::NONE:
            std::cout << "NONE";
            break;
        case TransitionType::EPSILON:
            std::cout << "EPSILON -> " << trans.to;
            break;
        case TransitionType::VAR:
            std::cout << "'" << trans.var << "' -> " << trans.to;
            break;
    }
}

void NFA::print() const {
    std::cout << "Start state: " << start << "\n";
    std::cout << "Accept state: " << accept << "\n";

    for (const auto &state : states) {
        if (state.out1.type != TransitionType::NONE || state.out2.type != TransitionType::NONE) {
            std::cout << "State " << state.id << ": ";
            
            if (state.out1.type != TransitionType::NONE) {
                print_transition(state.out1);
            }

            if (state.out2.type != TransitionType::NONE) {
                std::cout << ", ";
                print_transition(state.out2);
            }
            std::cout << "\n";
        } 
    }
}

Run::Run(int state)
    : state(state), bindings() {}

Simulation::Simulation(const NFA &nfa)
    : nfa(nfa) {
        Run Run(nfa.start);
        currentRuns.push_back(Run);
        epsilon_closure(currentRuns);
    }

bool run_exists(const Run &run, const std::vector<Run> &currentRuns) {
    for (const Run &r : currentRuns) {
        if (r.state != run.state) {
            continue;
        }
        if (r.bindings.size() != run.bindings.size()) {
            continue;
        }

        bool exists = true;
        for (size_t i = 0; i < run.bindings.size(); ++i) {
            if (r.bindings[i].var != run.bindings[i].var || r.bindings[i].row_idx != run.bindings[i].row_idx) {
                exists = false;
                break;
            }
        }
        if (exists) {
            return true; 
        }
    }
    return false;
}

void Simulation::epsilon_closure(std::vector<Run> &currentRuns) {
    std::queue<Run> q;

    for (Run run : currentRuns) {
        q.push(run);
    }
    
    currentRuns.clear();

    while (!q.empty()) {
        Run run = q.front();
        q.pop();

        int id = run.state;
        const State &state = nfa.states[static_cast<size_t>(id)];

        if (run.state == nfa.accept) {
            accRuns.push_back(run);
        } if (state.out1.type == TransitionType::VAR) {
            if (!run_exists(run, currentRuns)) {
                currentRuns.push_back(run);
            }
        } if (state.out1.type == TransitionType::EPSILON) {  
            Run r = run;
            r.state = state.out1.to;
            q.push(r);
        } if (state.out2.type == TransitionType::EPSILON) {
            Run r = run;
            r.state = state.out2.to;
            q.push(r);
        }
    }
}

void Simulation::print_run(const Run &run) {
    if (run.bindings.size()  == 0) {
        std::cout << "Run: state=" << run.state << ", bindings=[]\n";
    } else {
        std::cout << "Run: state=" << run.state << ", bindings=[";

        for (size_t i = 0; i < run.bindings.size() - 1; ++i) {
            std::cout << run.bindings[i].var  << ":" << run.bindings[i].row_idx << " ";
        }

        size_t last = run.bindings.size() - 1;
        std::cout << run.bindings[last].var << ":" << run.bindings[last].row_idx << "]\n";
    }
}

void Simulation::print_results(bool match) {
    if (!match) {
        std::cout << "\n" << SHINY_RED << "=== EMPTY ===" << RESET_COLOR <<"\n\n";
    } else {
        const Run *longest_match = &accRuns[0];
        for (const auto &run : accRuns) {
            if (run.bindings.size() > longest_match->bindings.size()) {
                longest_match = &run;
            }
        }
        std::cout << "\n" << SHINY_GREEN << "=============== RESULT ===============" << RESET_COLOR << "\n\n";
        for (const matchedVar &matchedVar : longest_match->bindings) {
            std::cout << matchedVar.var << " -> Row " << matchedVar.row_idx << "\n";
        }
        std::cout << "\n";
    }
}

bool Simulation::run(int start_row, int num_rows){
    for (int row_idx = start_row; row_idx < num_rows; row_idx++) {
        std::cout << "\nROW " << row_idx << ":\n";

        std::vector<Run> nextRuns;

        for (Run run : currentRuns) {
            print_run(run);

            int id = run.state;
            const State &state = nfa.states[static_cast<size_t>(id)];

            if (run.state == nfa.accept) {
                accRuns.push_back(run);  
                continue;                 
            }

            if (state.out1.type == TransitionType::VAR) {
                if (state.out1.guard(run.bindings, row_idx)) {
                    std::cout << state.out1.var << " -> " << state.out1.to << " accepted\n";

                    run.state = state.out1.to;

                    matchedVar matchedVar;
                    matchedVar.var = state.out1.var;
                    matchedVar.row_idx = row_idx; 
                    run.bindings.push_back(matchedVar);

                    nextRuns.push_back(run); 
                } else {
                    std::cout << state.out1.var << " -> " << state.out1.to << " rejected\n";
                }
            } 
        }
        epsilon_closure(nextRuns);
        currentRuns = std::move(nextRuns);
        if (currentRuns.empty()) {
            break;
        }
    }
    
    bool match = !accRuns.empty();
    return match;
}

const Run* Simulation::find_matches(Simulation &sim, int &start_pos, int num_rows, bool after_match_skip_to_next_row) {
    bool match = sim.run(start_pos, num_rows);
    sim.print_results(match);

    // no match -> start at next row
    if (sim.accRuns.empty()) {
        start_pos++;
        return nullptr;
    }

    // greedy: pick longest match
    const Run *longest_match = &sim.accRuns[0];
    for (const auto &run : sim.accRuns) {
        if (run.bindings.size() > longest_match->bindings.size()) {
            longest_match = &run;
        }
    }

    // skip logic
    if (after_match_skip_to_next_row) {
        start_pos++;
    } else {
        int match_length = static_cast<int>(longest_match->bindings.size());
        start_pos += match_length;
    }

    return longest_match;
}
