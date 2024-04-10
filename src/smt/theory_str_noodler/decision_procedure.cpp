#include <queue>
#include <utility>
#include <algorithm>
#include <functional>

#include <mata/nfa/strings.hh>
#include "util.h"
#include "aut_assignment.h"
#include "decision_procedure.h"

namespace smt::noodler {

    void SolvingState::substitute_vars(std::unordered_map<BasicTerm, std::vector<BasicTerm>> &substitution_map) {
        // substitutes variables in a vector using substitution_map
        auto substitute_vector = [&substitution_map](const std::vector<BasicTerm> &vector) {
            std::vector<BasicTerm> result;
            for (const BasicTerm &var : vector) {
                if (substitution_map.count(var) == 0) {
                    result.push_back(var);
                } else {
                    const auto &to_this = substitution_map.at(var);
                    result.insert(result.end(), to_this.begin(), to_this.end());
                }
            }
            return result;
        };

        // substitutes variables in both sides of inclusion using substitution_map
        auto substitute_inclusion = [&substitute_vector](const Predicate &inclusion) {
            std::vector<BasicTerm> new_left_side = substitute_vector(inclusion.get_left_side());
            std::vector<BasicTerm> new_right_side = substitute_vector(inclusion.get_right_side());
            return Predicate{inclusion.get_type(), { new_left_side, new_right_side }};
        };

        // returns true if the inclusion has the same thing on both sides
        auto inclusion_has_same_sides = [](const Predicate &inclusion) { return inclusion.get_left_side() == inclusion.get_right_side(); };

        // substitutes variables of inclusions in a vector using substitute_map, but does not keep the ones that have the same sides after substitution
        auto substitute_set = [&substitute_inclusion, &inclusion_has_same_sides](const std::set<Predicate> inclusions) {
            std::set<Predicate> new_inclusions;
            for (const auto &old_inclusion : inclusions) {
                auto new_inclusion = substitute_inclusion(old_inclusion);
                if (!inclusion_has_same_sides(new_inclusion)) {
                    new_inclusions.insert(new_inclusion);
                }
            }
            return new_inclusions;
        };

        inclusions = substitute_set(inclusions);
        inclusions_not_on_cycle = substitute_set(inclusions_not_on_cycle);

        // substituting inclusions to process is bit harder, it is possible that two inclusions that were supposed to
        // be processed become same after substituting, so we do not want to keep both in inclusions to process
        std::set<Predicate> substituted_inclusions_to_process;
        std::deque<Predicate> new_inclusions_to_process;
        while (!inclusions_to_process.empty()) {
            Predicate substituted_inclusion = substitute_inclusion(inclusions_to_process.front());
            inclusions_to_process.pop_front();
            
            if (!inclusion_has_same_sides(substituted_inclusion) // we do not want to add inclusion that is already in inclusions_to_process
                && substituted_inclusions_to_process.count(substituted_inclusion) == 0) {
                new_inclusions_to_process.push_back(substituted_inclusion);
            }
        }
        inclusions_to_process = new_inclusions_to_process;
    }

    LenNode SolvingState::get_lengths(const BasicTerm& var) const {
        if (aut_ass.count(var) > 0) {
            // if var is not substituted, get length constraint from its automaton
            return aut_ass.get_lengths(var);
        } else if (substitution_map.count(var) > 0) {
            // if var is substituted, i.e. state.substitution_map[var] = x_1 x_2 ... x_n, then we have to create length equation
            //      |var| = |x_1| + |x_2| + ... + |x_n|
            std::vector<LenNode> plus_operands;
            for (const auto& subst_var : substitution_map.at(var)) {
                plus_operands.emplace_back(subst_var);
            }
            LenNode result(LenFormulaType::EQ, {var, LenNode(LenFormulaType::PLUS, plus_operands)});
            // to be safe, we add |var| >= 0 (for the aut_ass case, it is done in aut_ass.get_lengths)
            return LenNode(LenFormulaType::AND, {result, LenNode(LenFormulaType::LEQ, {0, var})});
        } else {
            util::throw_error("Variable was neither in automata assignment nor was substituted");
            return LenNode(BasicTerm(BasicTermType::Literal)); // return something to get rid of warnings
        }
    }

    void SolvingState::flatten_substition_map() {
        std::unordered_map<BasicTerm, std::vector<BasicTerm>> new_substitution_map;
        std::function<std::vector<BasicTerm>(const BasicTerm&)> flatten_var;

        flatten_var = [&new_substitution_map, &flatten_var, this](const BasicTerm &var) -> std::vector<BasicTerm> {
            if (new_substitution_map.count(var) == 0) {
                std::vector<BasicTerm> flattened_mapping;
                for (const auto &subst_var : this->substitution_map.at(var)) {
                    if (aut_ass.count(subst_var) > 0) {
                        // subst_var is not substituted, keep it
                        flattened_mapping.push_back(subst_var);
                    } else {
                        // subst_var has a substitution, flatten it and insert it to the end of flattened_mapping
                        std::vector<BasicTerm> flattened_mapping_of_subst_var = flatten_var(subst_var);
                        flattened_mapping.insert(flattened_mapping.end(),
                                                 flattened_mapping_of_subst_var.begin(),
                                                 flattened_mapping_of_subst_var.end());
                    }
                }
                new_substitution_map[var] = flattened_mapping;
                return flattened_mapping;
            } else {
                return new_substitution_map[var];
            }
        };

        for (const auto &subst_map_pair : substitution_map) {
            flatten_var(subst_map_pair.first);
        }

        STRACE("str-nfa",
            tout << "Flattened substitution map:" << std::endl;
            for (const auto &var_map : new_substitution_map) {
                tout << "    " << var_map.first.get_name() << " ->";
                for (const auto &subst_var : var_map.second) {
                    tout << " " << subst_var;
                }
                tout << std::endl;
            });

        substitution_map = new_substitution_map;
    }

    lbool DecisionProcedure::compute_next_solution() {

        // if we have a not contains, we give unknown
        if(this->not_contains.get_predicates().size() > 0) {
            return l_undef;
        }

        // iteratively select next state of solving that can lead to solution and
        // process one of the unprocessed nodes (or possibly find solution)
        STRACE("str", tout << "------------------------"
                           << "Getting another solution"
                           << "------------------------" << std::endl;);

        // start with formula for disequations
        std::vector<LenNode> conjuncts = disequations_len_formula_conjuncts;
        // add length formula from preprocessing
        conjuncts.push_back(preprocessing_len_formula);

        while (!worklist.empty()) {
            SolvingState element_to_process = std::move(worklist.front());
            worklist.pop_front();

            auto [noodler_lengths, precision] = get_node_lengths(element_to_process, conjuncts);
            len_node_to_z3_formula(noodler_lengths);
            //auto lengths = len_node_to_z3_formula(noodler_lengths);
            //lbool is_lengths_sat = int_solver->check_sat(lengths);
            //printf("is_lengths_sat: %d\n", is_lengths_sat);

            if (element_to_process.inclusions_to_process.empty()) {
                // we found another solution, element_to_process contain the automata
                // assignment and variable substition that satisfy the original
                // inclusion graph
                solution = std::move(element_to_process);
                STRACE("str",
                    tout << "Found solution:" << std::endl;
                    for (const auto &var_substitution : solution.substitution_map) {
                        tout << "    " << var_substitution.first << " ->";
                        for (const auto& subst_var : var_substitution.second) {
                            tout << " " << subst_var;
                        }
                        tout << std::endl;
                    }
                    for (const auto& var_aut : solution.aut_ass) {
                        tout << "    " << var_aut.first << " -> NFA" << std::endl;
                        if (is_trace_enabled("str-nfa")) {
                            var_aut.second->print_to_mata(tout);
                        }
                    }
                );
                return l_true;
            }

            // we will now process one inclusion from the inclusion graph which is at front
            // i.e. we will update automata assignments and substitutions so that this inclusion is fulfilled
            Predicate inclusion_to_process = element_to_process.inclusions_to_process.front();
            element_to_process.inclusions_to_process.pop_front();

            // this will decide whether we will continue in our search by DFS or by BFS
            bool is_inclusion_to_process_on_cycle = element_to_process.is_inclusion_on_cycle(inclusion_to_process);

            STRACE("str", tout << "Processing node with inclusion " << inclusion_to_process << " which is" << (is_inclusion_to_process_on_cycle ? " " : " not ") << "on the cycle" << std::endl;);
            STRACE("str",
                tout << "Length variables are:";
                for(auto const &var : inclusion_to_process.get_vars()) {
                    if (element_to_process.length_sensitive_vars.count(var)) {
                        tout << " " << var.to_string();
                    }
                }
                tout << std::endl;
            );

            const auto &left_side_vars = inclusion_to_process.get_left_side();
            const auto &right_side_vars = inclusion_to_process.get_right_side();

            /********************************************************************************************************/
            /****************************************** One side is empty *******************************************/
            /********************************************************************************************************/
            // As kinda optimization step, we do "noodlification" for empty sides separately (i.e. sides that
            // represent empty string). This is because it is simpler, we would get only one noodle so we just need to
            // check that the non-empty side actually contains empty string and replace the vars on that side by epsilon.
            if (right_side_vars.empty() || left_side_vars.empty()) {
                std::unordered_map<BasicTerm, std::vector<BasicTerm>> substitution_map;
                auto const non_empty_side_vars = right_side_vars.empty() ? 
                                                        inclusion_to_process.get_left_set()
                                                      : inclusion_to_process.get_right_set();
                bool non_empty_side_contains_empty_word = true;
                for (const auto &var : non_empty_side_vars) {
                    if (element_to_process.aut_ass.contains_epsilon(var)) {
                        // var contains empty word, we substitute it with only empty word, but only if...
                        if (right_side_vars.empty() // ...non-empty side is the left side (var is from left) or...
                               || element_to_process.length_sensitive_vars.count(var) > 0 // ...var is length-aware
                         ) {
                            assert(substitution_map.count(var) == 0 && element_to_process.aut_ass.count(var) > 0);
                            // we prepare substitution for all vars on the left or only the length vars on the right
                            // (as non-length vars are probably not needed? TODO: would it make sense to update non-length vars too?)
                            substitution_map[var] = {};
                            element_to_process.aut_ass.erase(var);
                        }
                    } else {
                        // var does not contain empty word => whole non-empty side cannot contain empty word
                        non_empty_side_contains_empty_word = false;
                        break;
                    }
                }
                if (!non_empty_side_contains_empty_word) {
                    // in the case that the non_empty side does not contain empty word
                    // the inclusion cannot hold (noodlification would not create anything)
                    continue;
                }

                // TODO: all this following shit is done also during normal noodlification, I need to split it to some better defined functions

                element_to_process.remove_inclusion(inclusion_to_process);

                // We might be updating left side, in that case we need to process all nodes that contain the variables from the left,
                // i.e. those nodes to which inclusion_to_process goes to. In the case we are updating right side, there will be no edges
                // coming from inclusion_to_process, so this for loop will do nothing.
                for (const auto &dependent_inclusion : element_to_process.get_dependent_inclusions(inclusion_to_process)) {
                    // we push only those nodes which are not already in inclusions_to_process
                    // if the inclusion_to_process is on cycle, we need to do BFS
                    // if it is not on cycle, we can do DFS
                    // TODO: can we really do DFS??
                    element_to_process.push_unique(dependent_inclusion, is_inclusion_to_process_on_cycle);
                }

                // do substitution in the inclusion graph
                element_to_process.substitute_vars(substitution_map);
                // update the substitution_map of new_element by the new substitutions
                element_to_process.substitution_map.merge(substitution_map);

                // TODO: should we really push to front when not on cycle?
                // TODO: maybe for this case of one side being empty, we should just push to front?
                if (!is_inclusion_to_process_on_cycle) {
                    worklist.push_front(element_to_process);
                } else {
                    worklist.push_back(element_to_process);
                }
                continue;
            }
            /********************************************************************************************************/
            /*************************************** End of one side is empty ***************************************/
            /********************************************************************************************************/



            /********************************************************************************************************/
            /****************************************** Process left side *******************************************/
            /********************************************************************************************************/
            std::vector<std::shared_ptr<mata::nfa::Nfa>> left_side_automata;
            STRACE("str-nfa", tout << "Left automata:" << std::endl);
            for (const auto &l_var : left_side_vars) {
                left_side_automata.push_back(element_to_process.aut_ass.at(l_var));
                STRACE("str-nfa",
                    tout << "Automaton for left var " << l_var.get_name() << ":" << std::endl;
                    left_side_automata.back()->print_to_DOT(tout);
                );
            }
            /********************************************************************************************************/
            /************************************** End of left side processing *************************************/
            /********************************************************************************************************/




            /********************************************************************************************************/
            /***************************************** Process right side *******************************************/
            /********************************************************************************************************/
            // We combine the right side into automata where we concatenate non-length-aware vars next to each other.
            // Each right side automaton corresponds to either concatenation of non-length-aware vars (vector of
            // basic terms) or one lenght-aware var (vector of one basic term). Division then contains for each right
            // side automaton the variables whose concatenation it represents.
            std::vector<std::shared_ptr<mata::nfa::Nfa>> right_side_automata;
            std::vector<std::vector<BasicTerm>> right_side_division;

            assert(!right_side_vars.empty()); // empty case was processed at the beginning
            auto right_var_it = right_side_vars.begin();
            auto right_side_end = right_side_vars.end();

            std::shared_ptr<mata::nfa::Nfa> next_aut = element_to_process.aut_ass[*right_var_it];
            std::vector<BasicTerm> next_division{ *right_var_it };
            bool last_was_length = (element_to_process.length_sensitive_vars.count(*right_var_it) > 0);
            bool is_there_length_on_right = last_was_length;
            ++right_var_it;

            STRACE("str-nfa", tout << "Right automata:" << std::endl);
            for (; right_var_it != right_side_end; ++right_var_it) {
                std::shared_ptr<mata::nfa::Nfa> right_var_aut = element_to_process.aut_ass.at(*right_var_it);
                if (element_to_process.length_sensitive_vars.count(*right_var_it) > 0) {
                    // current right_var is length-aware
                    right_side_automata.push_back(next_aut);
                    right_side_division.push_back(next_division);
                    STRACE("str-nfa",
                        tout << "Automaton for right var(s)";
                        for (const auto &r_var : next_division) {
                            tout << " " << r_var.get_name();
                        }
                        tout << ":" << std::endl;
                        next_aut->print_to_DOT(tout);
                    );
                    next_aut = right_var_aut;
                    next_division = std::vector<BasicTerm>{ *right_var_it };
                    last_was_length = true;
                    is_there_length_on_right = true;
                } else {
                    // current right_var is not length-aware
                    if (last_was_length) {
                        // if last var was length-aware, we need to add automaton for it into right_side_automata
                        right_side_automata.push_back(next_aut);
                        right_side_division.push_back(next_division);
                        STRACE("str-nfa",
                            tout << "Automaton for right var(s)";
                            for (const auto &r_var : next_division) {
                                tout << " " << r_var.get_name();
                            }
                            tout << ":" << std::endl;
                            next_aut->print_to_DOT(tout);
                        );
                        next_aut = right_var_aut;
                        next_division = std::vector<BasicTerm>{ *right_var_it };
                    } else {
                        // if last var was not length-aware, we combine it (and possibly the non-length-aware vars before)
                        // with the current one
                        next_aut = std::make_shared<mata::nfa::Nfa>(mata::nfa::concatenate(*next_aut, *right_var_aut));
                        next_division.push_back(*right_var_it);
                        // TODO should we reduce size here?
                    }
                    last_was_length = false;
                }
            }
            right_side_automata.push_back(next_aut);
            right_side_division.push_back(next_division);
            STRACE("str-nfa",
                tout << "Automaton for right var(s)";
                for (const auto &r_var : next_division) {
                    tout << " " << r_var.get_name();
                }
                tout << ":" << std::endl;
                next_aut->print_to_DOT(tout);
            );
            /********************************************************************************************************/
            /************************************* End of right side processing *************************************/
            /********************************************************************************************************/


            /********************************************************************************************************/
            /****************************************** Inclusion test **********************************************/
            /********************************************************************************************************/
            if (!is_there_length_on_right) {
                // we have no length-aware variables on the right hand side => we need to check if inclusion holds
                assert(right_side_automata.size() == 1); // there should be exactly one element in right_side_automata as we do not have length variables
                // TODO probably we should try shortest words, it might work correctly
                if (is_inclusion_to_process_on_cycle // we do not test inclusion if we have node that is not on cycle, because we will not go back to it (TODO: should we really not test it?)
                    && mata::nfa::is_included(element_to_process.aut_ass.get_automaton_concat(left_side_vars), *right_side_automata[0])) {
                    // TODO can I push to front? I think I can, and I probably want to, so I can immediately test if it is not sat (if element_to_process.inclusions_to_process is empty), or just to get to sat faster
                    worklist.push_front(element_to_process);
                    // we continue as there is no need for noodlification, inclusion already holds
                    continue;
                }
            }
            /********************************************************************************************************/
            /*************************************** End of inclusion test ******************************************/
            /********************************************************************************************************/

            element_to_process.remove_inclusion(inclusion_to_process);

            // We are going to change the automata on the left side (potentially also split some on the right side, but that should not have impact)
            // so we need to add all nodes whose variable assignments are going to change on the right side (i.e. we follow inclusion graph) for processing.
            // Warning: Self-loops are not in inclusion graph, but we might still want to add this node again to inclusions_to_process, however, this node will be
            // split during noodlification, so we will only add parts whose right sides actually change (see below in noodlification)
            for (const auto &node : element_to_process.get_dependent_inclusions(inclusion_to_process)) {
                // we push only those nodes which are not already in inclusions_to_process
                // if the inclusion_to_process is on cycle, we need to do BFS
                // if it is not on cycle, we can do DFS
                // TODO: can we really do DFS??
                element_to_process.push_unique(node, is_inclusion_to_process_on_cycle);
            }
            // We will need the set of left vars, so we can sort the 'non-existing self-loop' in noodlification (see previous warning)
            const auto left_vars_set = inclusion_to_process.get_left_set();


            /* TODO check here if we have empty elements_to_process, if we do, then every noodle we get should finish and return sat
             * right now if we test sat at the beginning it should work, but it is probably better to immediatly return sat if we have
             * empty elements_to_process, however, we need to remmeber the state of the algorithm, we would need to return back to noodles
             * and process them if z3 realizes that the result is actually not sat (because of lengths)
             */

            

            /********************************************************************************************************/
            /******************************************* Noodlification *********************************************/
            /********************************************************************************************************/
            /**
             * We get noodles where each noodle consists of automata connected with a vector of numbers.
             * So for example if we have some noodle and automaton noodle[i].first, then noodle[i].second is a vector,
             * where first element i_l = noodle[i].second[0] tells us that automaton noodle[i].first belongs to the
             * i_l-th left var (i.e. left_side_vars[i_l]) and the second element i_r = noodle[i].second[1] tell us that
             * it belongs to the i_r-th division of the right side (i.e. right_side_division[i_r])
             **/
            auto noodles = mata::strings::seg_nfa::noodlify_for_equation(left_side_automata, 
                                                                        right_side_automata,
                                                                        false, 
                                                                        {{"reduce", "forward"}});

            //---------------------------------------------------------------------------------------------------

            // STRACE("str", tout << "noodlecount: " << noodles.size() << std::endl;);
            // STRACE("str", tout << "noodles:" << std::endl;);
            // for (const auto &noodle : noodles) {
            //     STRACE("str", tout << "noodle:" << std::endl;);
            //     for (const auto &noodle_pair : noodle) {
            //         for (const auto &noodle_pair_pair : noodle_pair.second) {
            //             STRACE("str", tout << noodle_pair_pair << std::endl;);
            //         }
            //         STRACE("str", tout << noodle_pair.first.get()->print_to_mata() << std::endl;);
            //     }
            // }

            // /**
            //  * The following code focuses on reducing the case split, created by the noodlification, as much as possible.
            //  * The algorithm is based on inclusion checking between all produced noodles. The inclusions are processed
            //  * on the level of separate bubbles that make the noodles up. First, we compare the right sides of the bubbles (the alignments),
            //  * and if they get past, we check inclusions of their respective left sides (automata). The final product 
            //  * is a distinct set of noodles, contained within the noodles vector.
            //  */
            // for (size_t stopper = 0; stopper < noodles.size(); stopper++){
            //     //printf("outer: %d\n", stopper);
            //     STRACE("str", tout << "uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu" << std::endl;);
            //     // load the i-th noodle into variable udon
            //     auto udon = noodles[stopper];
            //     // loop through all variables inside the noodle
            //     for (size_t j = 0; j < stopper; j++){
            //         STRACE("str", tout << "sssssssssssssssssssssssssssssssssssssssssssssss" << std::endl;);
            //         //printf("inner: %d\n", j);
            //         auto soba = noodles[j];
            //         size_t l = 0;
            //         // Now the sizes of the udon.first and soba.first do not have to be the same so how do we compare them?
            //         size_t bubble_count = udon.size() < soba.size() ? udon.size() : soba.size();
            //         bool soba_larger = udon.size() < soba.size() ? true : false;
            //         mata::strings::seg_nfa::NoodleWithEpsilonsCounter *smaller_nood = udon.size() < soba.size() ? &udon : &soba;
            //         mata::strings::seg_nfa::NoodleWithEpsilonsCounter *bigger_nood = udon.size() >= soba.size() ? &udon : &soba;
            //         bool skipped_noodle = false;
            //         bool deletion_time_udon = false;
            //         bool deletion_time_soba = false;
            //         // based on these two flags, we later decide about the deletion of the noodle
            //         bool udon_is_smaller = true;
            //         bool soba_is_smaller = true;
            //         for (size_t k = 0; k < bubble_count; k++){
            //             STRACE("str", tout << "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv" << std::endl;);
            //             //printf("inner inner: %d\n", k);
            //             if (l == (*bigger_nood).size()){
            //                     //printf("l == size\n");
            //                     break;
            //             }

            //             // In case of alignment mismatches, we try to catch up with the smaller noodle 
            //             // if there are epsilons language automata. If there are none, or the bigger noodle overtakes 
            //             // the smaller one, we can skip checking inclusions on the noodle and push it.
            //             while ((*smaller_nood)[k].second != (*bigger_nood)[l].second){
            //                 //printf("whilebing\n");
            //                 if (!((*bigger_nood)[l].first->final == ((*bigger_nood)[l].first->initial))) {
            //                     STRACE("str", tout << "skipping inclusion" << std::endl;);
            //                     skipped_noodle = true;
            //                     break;
            //                 }
                            
            //                 //printf("mezi\n");
            //                 l++;
            //                 if (l == (*bigger_nood).size()){
            //                     //printf("l == size\n");
            //                     skipped_noodle = true;
            //                     break;
            //                 }

            //                 if  (((*bigger_nood)[l].second[0] > (*smaller_nood)[k].second[0]) ||
            //                      ((*bigger_nood)[l].second[1] > (*smaller_nood)[k].second[1])) {
            //                     STRACE("str", tout << "skipping inclusion" << std::endl;);
            //                     skipped_noodle = true;
            //                     break;
            //                 }
            //                 //printf("l: %d, k: %d\n", l, k);
            //             }

            //             if (skipped_noodle){
            //                 //printf("skipped\n");
            //                 break;
            //             }

            //             // helper pointers to the right and left variable sides
            //             std::vector<unsigned int> *soba_right_side;
            //             std::vector<unsigned int> *udon_right_side;
            //             std::shared_ptr<mata::nfa::Nfa> *soba_first;
            //             std::shared_ptr<mata::nfa::Nfa> *udon_first;

            //             if (soba_larger){
            //                 soba_right_side = &soba[l].second;
            //                 udon_right_side = &udon[k].second;
            //                 soba_first = &soba[l].first;
            //                 udon_first = &udon[k].first;
            //             } else {
            //                 soba_right_side = &soba[k].second;
            //                 udon_right_side = &udon[l].second;
            //                 soba_first = &soba[k].first;
            //                 udon_first = &udon[l].first;
            //             }


            //             // if alignments match, we can compare the automata
            //             if (*udon_right_side == *soba_right_side){
            //                 STRACE("str", tout << "antichain start" << std::endl;);
            //                 //printf("antichain start\n");
            //                 // the right sides are the same, we can call is_included
            //                 bool udon_in_soba = mata::nfa::is_included(*(*udon_first), *(*soba_first));
            //                 bool soba_in_udon = mata::nfa::is_included(*(*soba_first), *(*udon_first));
            //                 STRACE("str", tout << "antichain end" << std::endl;);
            //                 //printf("antichain end\n");
            //                 if (udon_in_soba && soba_in_udon){
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                     STRACE("str", tout << "soba variable is the same as udon" << std::endl;);
            //                     //printf("soba variable is the same as udon\n");
            //                     STRACE("str", tout << udon_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << soba_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                 }
            //                 else if (udon_in_soba){
            //                     // udon is smaller, soba is eaten
            //                     soba_is_smaller = false;
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                     STRACE("str", tout << "udon variable is smaller than soba" << std::endl;);
            //                     //printf("udon variable is smaller than soba\n");
            //                     STRACE("str", tout << udon_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << soba_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                 } else if (soba_in_udon){
            //                     // soba is smaller, udon is eaten
            //                     udon_is_smaller = false;
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                     STRACE("str", tout << "soba variable is smaller than udon" << std::endl;);
            //                     //printf("soba variable is smaller than udon\n");
            //                     STRACE("str", tout << udon_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << soba_first->get()->print_to_mata() << std::endl;);
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                 } else {
            //                     // they are not the same, we can push it to newNoodles
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                     STRACE("str", tout << "soba and udon are not the same" << std::endl;);
            //                     STRACE("str", tout << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" << std::endl;);
            //                     //printf("soba and udon are not the same\n");

            //                     break;
            //                 }
            //             }

            //             if (k == bubble_count - 1) {
            //                 //printf("k == bubble_count - 1\n");
            //                 while (l != (*bigger_nood).size() - 1 && (*bigger_nood)[l].first->final == (*bigger_nood)[l].first->initial)
            //                     l++;

            //                 if (l != (*bigger_nood).size() - 1) {
            //                     STRACE("str", tout << "magicky skip" << std::endl;);
            //                     //printf("magicky skip\n");
            //                 } else if (udon_is_smaller && soba_is_smaller){
            //                     // both noodles are completely the same, we can delete one
            //                     if (soba_larger) {
            //                         deletion_time_udon = true;
            //                     } else {
            //                         deletion_time_soba = true;
            //                     }
            //                     STRACE("str", tout << "soba and udon are the same -> eaten" << std::endl;);
            //                     //printf("soba and udon are the same -> eaten\n");
            //                 } else if (udon_is_smaller){
            //                     // udon is smaller, soba is eaten
            //                     deletion_time_udon = true;
            //                     STRACE("str", tout << "udon is smaller -> eaten" << std::endl;);
            //                     //printf("udon is smaller -> eaten\n");
            //                 } else if (soba_is_smaller){
            //                     // soba is smaller, udon is eaten
            //                     deletion_time_soba = true;
            //                     STRACE("str", tout << "soba is smaller -> eaten" << std::endl;);
            //                     //printf("soba is smaller -> eaten\n");
            //                 }
            //             }

            //             l++;
            //         }

            //         if (deletion_time_udon){
            //             noodles.erase(noodles.begin() + stopper);
            //             stopper--;
            //             break;
            //         }

            //         if (deletion_time_soba){
            //             noodles.erase(noodles.begin() + j);
            //             j--;
            //             stopper--;
            //         }
            //         //printf("porovnavam s dalsim\n");
            //     }
            //     //printf("beru novou nudli\n");
            // }
            // //printf("je to over\n");
            // STRACE("str", tout << "noodlecount: " << noodles.size() << std::endl;);
            // STRACE("str", tout << "noodles:" << std::endl;);
            // for (const auto &noodle : noodles) {
            //     STRACE("str", tout << "noodle:" << std::endl;);
            //     for (const auto &noodle_pair : noodle) {
            //         for (const auto &noodle_pair_pair : noodle_pair.second) {
            //             STRACE("str", tout << noodle_pair_pair << std::endl;);
            //         }
            //         STRACE("str", tout << noodle_pair.first.get()->print_to_mata() << std::endl;);
            //     }
            // }

            for (const auto &noodle : noodles) {
                STRACE("str", tout << "Processing noodle" << (is_trace_enabled("str-nfa") ? " with automata:" : "") << std::endl;);
                SolvingState new_element = element_to_process;

                /* Explanation of the next code on an example:
                 * Left side has variables x_1, x_2, x_3, x_2 while the right side has variables x_4, x_1, x_5, x_6, where x_1
                 * and x_4 are length-aware (i.e. there is one automaton for concatenation of x_5 and x_6 on the right side).
                 * Assume that noodle represents the case where it was split like this:
                 *              | x_1 |    x_2    | x_3 |       x_2       |
                 *              | t_1 | t_2 | t_3 | t_4 | t_5 |    t_6    |
                 *              |    x_4    |       x_1       | x_5 | x_6 |
                 * In the following for loop, we create the vars t1, t2, ..., t6 and prepare two vectors left_side_vars_to_new_vars
                 * and right_side_divisions_to_new_vars which map left vars and right divisions into the concatenation of the new
                 * vars. So for example left_side_vars_to_new_vars[1] = t_2 t_3, because second left var is x_2 and we map it to t_2 t_3,
                 * while right_side_divisions_to_new_vars[2] = t_6, because the third division on the right represents the automaton for
                 * concatenation of x_5 and x_6 and we map it to t_6.
                 */
                std::vector<std::vector<BasicTerm>> left_side_vars_to_new_vars(left_side_vars.size());
                std::vector<std::vector<BasicTerm>> right_side_divisions_to_new_vars(right_side_division.size());
                for (unsigned i = 0; i < noodle.size(); ++i) {
                    // TODO do not make a new_var if we can replace it with one left or right var (i.e. new_var is exactly left or right var)
                    // TODO also if we can substitute with epsilon, we should do that first? or generally process epsilon substitutions better, in some sort of 'preprocessing'
                    BasicTerm new_var = util::mk_noodler_var_fresh(std::string("align_") + std::to_string(noodlification_no));
                    left_side_vars_to_new_vars[noodle[i].second[0]].push_back(new_var);
                    right_side_divisions_to_new_vars[noodle[i].second[1]].push_back(new_var);
                    new_element.aut_ass[new_var] = noodle[i].first; // we assign the automaton to new_var
                    STRACE("str-nfa", tout << new_var << std::endl << *noodle[i].first;);
                }

                // Each variable that occurs in the left side or is length-aware needs to be substituted, we use this map for that 
                std::unordered_map<BasicTerm, std::vector<BasicTerm>> substitution_map;

                /* Following the example from before, the following loop will create these inclusions from the right side divisions:
                 *         t_1 t_2 ⊆ x_4
                 *     t_3 t_4 t_5 ⊆ x_1
                 *             t_6 ⊆ x_5 x_6
                 * However, we do not add the first two inclusions into the inclusion graph but use them for substitution, i.e.
                 *        substitution_map[x_4] = t_1 t_2
                 *        substitution_map[x_1] = t_3 t_4 t_5
                 * because they are length-aware vars.
                 */
                for (unsigned i = 0; i < right_side_division.size(); ++i) {
                    const auto &division = right_side_division[i];
                    if (division.size() == 1 && element_to_process.length_sensitive_vars.count(division[0]) != 0) {
                        // right side is length-aware variable y => we are either substituting or adding new inclusion "new_vars ⊆ y"
                        const BasicTerm &right_var = division[0];
                        if (substitution_map.count(right_var)) {
                            // right_var is already substituted, therefore we add 'new_vars ⊆ right_var' to the inclusion graph
                            // TODO: how to decide if sometihng is on cycle? by previous node being on cycle, or when we recompute inclusion graph edges?
                            const auto &new_inclusion = new_element.add_inclusion(right_side_divisions_to_new_vars[i], division, is_inclusion_to_process_on_cycle);
                            // we also add this inclusion to the worklist, as it represents unification
                            // we push it to the front if we are processing node that is not on the cycle, because it should not get stuck in the cycle then
                            // TODO: is this correct? can we push to the front?
                            // TODO: can't we push to front even if it is on cycle??
                            new_element.push_unique(new_inclusion, is_inclusion_to_process_on_cycle);
                            STRACE("str", tout << "added new inclusion from the right side because it could not be substituted: " << new_inclusion << std::endl; );
                        } else {
                            // right_var is not substitued by anything yet, we will substitute it
                            substitution_map[right_var] = right_side_divisions_to_new_vars[i];
                            STRACE("str", tout << "right side var " << right_var.get_name() << " replaced with:"; for (auto const &var : right_side_divisions_to_new_vars[i]) { tout << " " << var.get_name(); } tout << std::endl; );
                            // as right_var wil be substituted in the inclusion graph, we do not need to remember the automaton assignment for it
                            new_element.aut_ass.erase(right_var);
                            // update the length variables
                            for (const BasicTerm &new_var : right_side_divisions_to_new_vars[i]) {
                                new_element.length_sensitive_vars.insert(new_var);
                            }
                        }

                    } else {
                        // right side is non-length concatenation "y_1...y_n" => we are adding new inclusion "new_vars ⊆ y1...y_n"
                        // TODO: how to decide if sometihng is on cycle? by previous node being on cycle, or when we recompute inclusion graph edges?
                        // TODO: do we need to add inclusion if previous node was not on cycle? because I think it is not possible to get to this new node anyway
                        const auto &new_inclusion = new_element.add_inclusion(right_side_divisions_to_new_vars[i], division, is_inclusion_to_process_on_cycle);
                        // we add this inclusion to the worklist only if the right side contains something that was on the left (i.e. it was possibly changed)
                        if (SolvingState::is_dependent(left_vars_set, new_inclusion.get_right_set())) {
                            // TODO: again, push to front? back? where the fuck to push??
                            new_element.push_unique(new_inclusion, is_inclusion_to_process_on_cycle);
                        }
                        STRACE("str", tout << "added new inclusion from the right side (non-length): " << new_inclusion << std::endl; );
                    }
                }

                /* Following the example from before, the following loop will create these inclusions from the left side:
                 *           x_1 ⊆ t_1
                 *           x_2 ⊆ t_2 t_3
                 *           x_3 ⊆ t_4
                 *           x_2 ⊆ t_5 t_6
                 * Again, we want to use the inclusions for substitutions, but we replace only those variables which were
                 * not substituted yet, so the first inclusion stays (x_1 was substituted from the right side) and the
                 * fourth inclusion stays (as we substitute x_2 using the second inclusion). So from the second and third
                 * inclusion we get:
                 *        substitution_map[x_2] = t_2 t_3
                 *        substitution_map[x_3] = t_4
                 */
                for (unsigned i = 0; i < left_side_vars.size(); ++i) {
                    // TODO maybe if !is_there_length_on_right, we should just do intersection and not create new inclusions
                    const BasicTerm &left_var = left_side_vars[i];
                    if (left_var.is_literal()) {
                        // we skip literals, we do not want to substitute them
                        continue;
                    }
                    if (substitution_map.count(left_var)) {
                        // left_var is already substituted, therefore we add 'left_var ⊆ left_side_vars_to_new_vars[i]' to the inclusion graph
                        std::vector<BasicTerm> new_inclusion_left_side{ left_var };
                        // TODO: how to decide if sometihng is on cycle? by previous node being on cycle, or when we recompute inclusion graph edges?
                        const auto &new_inclusion = new_element.add_inclusion(new_inclusion_left_side, left_side_vars_to_new_vars[i], is_inclusion_to_process_on_cycle);
                        // we also add this inclusion to the worklist, as it represents unification
                        // we push it to the front if we are processing node that is not on the cycle, because it should not get stuck in the cycle then
                        // TODO: is this correct? can we push to the front?
                        // TODO: can't we push to front even if it is on cycle??
                        new_element.push_unique(new_inclusion, is_inclusion_to_process_on_cycle);
                        STRACE("str", tout << "added new inclusion from the left side because it could not be substituted: " << new_inclusion << std::endl; );
                    } else {
                        // TODO make this function or something, we do the same thing here as for the right side when substituting
                        // left_var is not substitued by anything yet, we will substitute it
                        substitution_map[left_var] = left_side_vars_to_new_vars[i];
                        STRACE("str", tout << "left side var " << left_var.get_name() << " replaced with:"; for (auto const &var : left_side_vars_to_new_vars[i]) { tout << " " << var.get_name(); } tout << std::endl; );
                        // as left_var wil be substituted in the inclusion graph, we do not need to remember the automaton assignment for it
                        new_element.aut_ass.erase(left_var);
                        // update the length variables
                        if (new_element.length_sensitive_vars.count(left_var) > 0) { // if left_var is length-aware => substituted vars should become length-aware
                            for (const BasicTerm &new_var : left_side_vars_to_new_vars[i]) {
                                new_element.length_sensitive_vars.insert(new_var);
                            }
                        }
                    }
                }

                // do substitution in the inclusion graph
                new_element.substitute_vars(substitution_map);

                // update the substitution_map of new_element by the new substitutions
                new_element.substitution_map.merge(substitution_map);

                // TODO should we really push to front when not on cycle?
                if (!is_inclusion_to_process_on_cycle) {
                    worklist.push_front(new_element);
                } else {
                    worklist.push_back(new_element);
                }

            }

            ++noodlification_no; // TODO: when to do this increment?? maybe noodlification_no should be part of SolvingState?
            /********************************************************************************************************/
            /*************************************** End of noodlification ******************************************/
            /********************************************************************************************************/

        }

        // there are no solving states left, which means nothing led to solution -> it must be unsatisfiable
        return l_false;
    }

    LenNode DecisionProcedure::get_initial_lengths() {
        if (init_length_sensitive_vars.empty()) {
            // there are no length sensitive vars, so we can immediately say true
            return LenNode(LenFormulaType::TRUE);
        }

        // start from length formula from preprocessing
        std::vector<LenNode> conjuncts = {preprocessing_len_formula};

        // for each initial length variable get the lengths of all its possible words for automaton in init_aut_ass
        for (const BasicTerm &var : init_length_sensitive_vars) {
            conjuncts.push_back(init_aut_ass.get_lengths(var));
        }

        return LenNode(LenFormulaType::AND, conjuncts);
    }

    std::pair<LenNode, LenNodePrecision> DecisionProcedure::get_lengths() {
        LenNodePrecision precision = LenNodePrecision::PRECISE; // start with precise and possibly change it later

        if (solution.length_sensitive_vars.empty()) {
            // There are no length vars (which also means no disequations nor conversions), it is not needed to create the lengths formula.
            return {LenNode(LenFormulaType::TRUE), precision};
        }

        // start with formula for disequations
        std::vector<LenNode> conjuncts = disequations_len_formula_conjuncts;
        // add length formula from preprocessing
        conjuncts.push_back(preprocessing_len_formula);

        // create length constraints from the solution, we only need to look at length sensitive vars
        for (const BasicTerm &len_var : solution.length_sensitive_vars) {
            conjuncts.push_back(solution.get_lengths(len_var));
        }

        // the following functions (getting formula for conversions) assume that we have flattened substitution map
        solution.flatten_substition_map();

        // add formula for conversions
        auto conv_form_with_precision = get_formula_for_conversions();
        conjuncts.push_back(conv_form_with_precision.first);
        precision = conv_form_with_precision.second;

        return {LenNode(LenFormulaType::AND, conjuncts), precision};
    }

    std::pair<LenNode, LenNodePrecision> DecisionProcedure::get_node_lengths(SolvingState solut, std::vector<LenNode> conjuncts) {
        LenNodePrecision precision = LenNodePrecision::PRECISE; // start with precise and possibly change it later

        if (solut.length_sensitive_vars.empty()) {
            // There are no length vars (which also means no disequations nor conversions), it is not needed to create the lengths formula.
            return {LenNode(LenFormulaType::TRUE), precision};
        }

        auto conjunctions = conjuncts;

        // create length constraints from the solution, we only need to look at length sensitive vars
        for (const BasicTerm &len_var : solut.length_sensitive_vars) {
            conjunctions.push_back(solut.get_lengths(len_var));
        }

        // the following functions (getting formula for conversions) assume that we have flattened substitution map
        //solution.flatten_substition_map();

        // add formula for conversions
        // auto conv_form_with_precision = get_formula_for_conversions();
        // conjunctions.push_back(conv_form_with_precision.first);
        // precision = conv_form_with_precision.second;

        return {LenNode(LenFormulaType::AND, conjunctions), precision};
    }

    std::set<BasicTerm> DecisionProcedure::get_vars_substituted_in_code_conversions() {
        std::set<BasicTerm> result;
        for (const TermConversion& conv : conversions) {
            switch (conv.type)
            {
                case ConversionType::FROM_CODE:
                case ConversionType::TO_CODE:
                {
                    for (const BasicTerm& var : solution.get_substituted_vars(conv.string_var)) {
                        result.insert(var);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return result;
    }

    // see the comment of get_formula_for_conversions for explanation
    LenNode DecisionProcedure::get_formula_for_code_subst_vars(const std::set<BasicTerm>& code_subst_vars) {
        LenNode result(LenFormulaType::AND);

        // for each code substituting variable c, create the formula
        //   (|c| != 1 && code_version_of(c) == -1) || (|c| == 1 && code_version_of(c) is code point of one of the chars in the language of automaton for c)
        for (const BasicTerm& c : code_subst_vars) {
            // non_char_case = (|c| != 1 && code_version_of(c) == -1)
            LenNode non_char_case(LenFormulaType::AND, { {LenFormulaType::NEQ, std::vector<LenNode>{c, 1}}, {LenFormulaType::EQ, std::vector<LenNode>{code_version_of(c),-1}} });

            // char_case = (|c| == 1 && code_version_of(c) is code point of one of the chars in the language of automaton for c)
            LenNode char_case(LenFormulaType::AND, { {LenFormulaType::EQ, std::vector<LenNode>{c, 1}}, /* code_version_of(c) is code point of one of the chars in the language of automaton for c */ });

            // the rest is just computing 'code_version_of(c) is code point of one of the chars in the language of automaton for c'

            // chars in the language of c (except dummy symbol)
            std::set<mata::Symbol> real_symbols_of_code_var;
            bool is_there_dummy_symbol = false;
            for (mata::Symbol s : mata::strings::get_accepted_symbols(*solution.aut_ass.at(c))) { // iterate trough chars of c
                if (!is_dummy_symbol(s)) {
                    real_symbols_of_code_var.insert(s);
                } else {
                    is_there_dummy_symbol = true;
                }
            }

            if (!is_there_dummy_symbol) {
                // if there is no dummy symbol, we can just create disjunction that code_version_of(c) is equal to one of the symbols in real_symbols_of_code_var
                std::vector<LenNode> equal_to_one_of_symbols;
                for (mata::Symbol s : real_symbols_of_code_var) {
                    equal_to_one_of_symbols.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{code_version_of(c), s});
                }
                char_case.succ.emplace_back(LenFormulaType::OR, equal_to_one_of_symbols);
            } else {
                // if there is dummy symbol, then code_version_of(c) can be code point of any char, except those in the alphabet but not in real_symbols_of_code_var
                // (0 <= code_version_of(c) <= max_char) - code_version_of(c) is valid code_point
                char_case.succ.emplace_back(LenFormulaType::LEQ, std::vector<LenNode>{0, code_version_of(c)});
                char_case.succ.emplace_back(LenFormulaType::LEQ, std::vector<LenNode>{code_version_of(c), zstring::max_char()});
                // code_version_of(c) is not equal to code point of some symbol in the alphabet that is not in real_symbols_of_code_var
                for (mata::Symbol s : solution.aut_ass.get_alphabet()) {
                    if (!is_dummy_symbol(s) && !real_symbols_of_code_var.contains(s)) {
                        char_case.succ.emplace_back(LenFormulaType::NEQ, std::vector<LenNode>{code_version_of(c), s});
                    }
                }
            }
            
            result.succ.emplace_back(LenFormulaType::OR, std::vector<LenNode>{
                non_char_case,
                char_case
            });
        }

        return result;
    }

    LenNode DecisionProcedure::word_to_int(const mata::Word& word, const BasicTerm &var, bool start_with_one, bool handle_invalid_as_from_int) {
        LenNode result(0);

        bool is_invalid = true;

        rational resulting_int = (start_with_one ? rational(1) : rational(0));

        for (mata::Symbol s : word) {
            is_invalid = false; // word is not empty, it might not be invalid
            if (AutAssignment::DIGIT_SYMBOL_START <= s && s <= AutAssignment::DIGIT_SYMBOL_END) { // s is a code point of digit
                rational real_digit(s - AutAssignment::DIGIT_SYMBOL_START);
                resulting_int = resulting_int*10 + real_digit;
            } else {
                // it is possible that s is a dummy symbol, but we assume that all digits are explicitly in the alphabet, see the assumptions
                // therefore s here always represents a non-digit symbol
                is_invalid = true;
                break;
            }
        }

        if (!is_invalid) {
            return LenNode(LenFormulaType::EQ, std::vector<LenNode>{var, resulting_int});
        } else if (!handle_invalid_as_from_int) {
            // var == -1
            return LenNode(LenFormulaType::EQ, std::vector<LenNode>{var, -1});
        } else {
            // var < 0
            return LenNode(LenFormulaType::LT, std::vector<LenNode>{var, 0});
        }
    };

    // see the comment of get_formula_for_conversions for explanation
    LenNode DecisionProcedure::get_formula_for_code_conversion(const TermConversion& conv) {
        const BasicTerm& s = conv.string_var;
        const BasicTerm& c = conv.int_var;

        // First we create the first conjunct of (1)
        LenNode invalid_value(LenFormulaType::AND);
        if (conv.type == ConversionType::TO_CODE) {
            // (|s| != 1 && c == -1)
            invalid_value.succ.emplace_back(LenFormulaType::NEQ, std::vector<LenNode>{s, 1});
            invalid_value.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{c, -1});
        } else {
            // (|s| == 0 && c is not a valid code point)
            invalid_value.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{s, 0});
            // non-valid code point means that 'c < 0 || c > max_char'
            invalid_value.succ.emplace_back(LenFormulaType::LT, std::vector<LenNode>{c, 0});
            invalid_value.succ.emplace_back(LenFormulaType::LT, std::vector<LenNode>{zstring::max_char(), c});
        }

        // Now we create the second disjunct of (1):
        //    (|s| == 1 && c >= 0 && c is equal to one of code_version_of(s_i))
        // that is shared in both to_code and from_code

        // c is equal to one of code_version_of(s_i)
        LenNode equal_to_one_subst_var(LenFormulaType::OR);
        for (const BasicTerm& subst_var : solution.get_substituted_vars(s)) {
            equal_to_one_subst_var.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{c, code_version_of(subst_var)});
        }

        
        return LenNode(LenFormulaType::OR, std::vector<LenNode>{
            // (|s| == 1 && c >= 0 && equal_to_one_subst_var)
            LenNode(LenFormulaType::AND, { {LenFormulaType::EQ, std::vector<LenNode>{s, 1}}, {LenFormulaType::LEQ, std::vector<LenNode>{0, c}}, equal_to_one_subst_var}),
            invalid_value
        });
    }

    // see the comment of get_formula_for_conversions for explanation
    std::pair<LenNode, LenNodePrecision> DecisionProcedure::get_formula_for_int_conversion(const TermConversion& conv, const std::set<BasicTerm>& code_subst_vars, const unsigned underapproximating_length) {
        const BasicTerm& s = conv.string_var;
        const BasicTerm& i = conv.int_var;

        LenNode result(LenFormulaType::OR);
        LenNodePrecision res_precision = LenNodePrecision::PRECISE;

        // s = s_1 ... s_n, subst_vars = <s_1, ..., s_n>
        const std::vector<BasicTerm>& subst_vars = solution.get_substituted_vars(s);

        // automaton representing all valid inputs (only digits)
        // - we also keep empty word, because we will use it for substituted vars, and one of them can be empty, while other has only digits (for example s1="12", s2="" but s="12" is valid)
        mata::nfa::Nfa only_digits(1, {0}, {0});
        for (mata::Symbol digit = AutAssignment::DIGIT_SYMBOL_START; digit <= AutAssignment::DIGIT_SYMBOL_END; ++digit) {
            only_digits.delta.add(0, digit, 0);
        }
        STRACE("str-conversion-int", tout << "only-digit NFA:" << std::endl << only_digits << std::endl;);
        // automaton representing all non-valid inputs (contain non-digit)
        mata::nfa::Nfa contain_non_digit = solution.aut_ass.complement_aut(only_digits);
        STRACE("str-conversion-int", tout << "contains-non-digit NFA:" << std::endl << contain_non_digit << std::endl;);

        // cases should be the collection of all words w = w_1 ... w_n, where w_i is the word of the language L_i of the automaton for s_i
        std::vector<std::vector<mata::Word>> cases = {{}};
        for (const BasicTerm& subst_var : solution.get_substituted_vars(s)) { // s_i = subst_var
            std::shared_ptr<mata::nfa::Nfa> aut = solution.aut_ass.at(subst_var);
            STRACE("str-conversion-int", tout << "NFA for " << subst_var << ":" << std::endl << *aut << std::endl;);

            // part containing only digits
            std::shared_ptr<mata::nfa::Nfa> aut_valid_part;
            // part containing some non-digit
            std::shared_ptr<mata::nfa::Nfa> aut_non_valid_part;

            if (conv.type == ConversionType::TO_INT) {
                aut_valid_part = std::make_shared<mata::nfa::Nfa>(mata::nfa::reduce(mata::nfa::intersection(*aut, only_digits).trim()));
                STRACE("str-conversion-int", tout << "only-digit NFA:" << std::endl << *aut_valid_part << std::endl;);
                aut_non_valid_part = std::make_shared<mata::nfa::Nfa>(mata::nfa::reduce(mata::nfa::intersection(*aut, contain_non_digit).trim()));
                STRACE("str-conversion-int", tout << "contains-non-digit NFA:" << std::endl << *aut_non_valid_part << std::endl;);
                if (!aut_non_valid_part->is_lang_empty()) {
                    // aut_non_valid_part is language of words that contain at least one non-digit
                    //  - we can get here only for the case that conv.type is to_int (for from_int, by assumptions, s should be restricted to language of "valid numbers + empty string")
                    //  - if s_i = one of these words, then s must also contain non-digit => result i = -1
                    // we therefore create following formula:
                    //       |s_i| is length of some word from aut_non_valid_part && int_version_of(s_i) = -1 && i = -1
                    result.succ.emplace_back(LenFormulaType::AND, std::vector<LenNode>{ solution.aut_ass.get_lengths(*aut_non_valid_part, subst_var), LenNode(LenFormulaType::EQ, { int_version_of(subst_var), -1 }), LenNode(LenFormulaType::EQ, {i, -1}) });
                    if (code_subst_vars.contains(subst_var)) {
                        // s_i is used in some to_code/from_code
                        // => we need to add to the previous formula also the fact, that s_i cannot encode code point of a digit
                        //      .. && !(AutAssignment::DIGIT_SYMBOL_START <= code_version_of(s_i) <= AutAssignment::DIGIT_SYMBOL_END)
                        result.succ.back().succ.emplace_back(LenFormulaType::LT, std::vector<LenNode>{ code_version_of(subst_var), AutAssignment::DIGIT_SYMBOL_START });
                        result.succ.back().succ.emplace_back(LenFormulaType::LT, std::vector<LenNode>{ AutAssignment::DIGIT_SYMBOL_END, code_version_of(subst_var) });
                    }
                }
            } else {
                // for from_int, we assume that s is restricted to language of "valid numbers + empty string", which means that
                // s_i should also be restricted to this language => 'aut intersected with only_digits == aut'
                aut_valid_part = aut;
                STRACE("str-conversion-int", tout << "only-digit NFA:" << std::endl << *aut_valid_part << std::endl;);
            }

            unsigned max_length_of_words = aut_valid_part->num_of_states();

            // we want to enumerate all words containing digits -> cannot be infinite language
            if (!aut_valid_part->is_acyclic()) {
                STRACE("str-conversion", tout << "failing NFA:" << std::endl << *aut_valid_part << std::endl;);
                res_precision = LenNodePrecision::UNDERAPPROX;
                if (max_length_of_words > underapproximating_length) {
                    // there are 10^max_length_of_words possible cases, we put limit so there is not MEMOUT
                    // but (experimentally) it seems to be better to reduce it even more if the automaton has less states
                    max_length_of_words = underapproximating_length;
                }
            }

            std::vector<std::vector<mata::Word>> new_cases;
            for (auto word : aut_valid_part->get_words(max_length_of_words)) {
                for (const auto& old_case : cases) {
                    std::vector<mata::Word> new_case = old_case;
                    new_case.push_back(word);
                    new_cases.push_back(new_case);
                }
            }
            cases = new_cases;
        }

        for (const auto& one_case : cases) {
            assert(subst_vars.size() == one_case.size());

            mata::Word full_word; // the word w
            LenNode formula_for_case(LenFormulaType::AND); // conjunct C
            for (unsigned i = 0; i < subst_vars.size(); ++i) {
                const BasicTerm& subst_var = subst_vars[i]; // var s_i
                mata::Word word_of_subst_var = one_case[i]; // word w_i (must contain only digits, from the way it was constructed)

                // creating formula
                //   |s_i| == |w_i| && int_version_of(s_i) = to_int('1'.w_i) [ && code_version_of(s_i) = to_code(w_i) ]

                // |s_i| = |w_i|
                formula_for_case.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{ subst_var, word_of_subst_var.size() });

                // int_version_of(s_i) = to_int('1'.w_i)
                // we add 1 at the beginning for the cases with 0s at the beginning of w_i,
                // so that for example if w_i="00013", it does not turn it into 13 but into 100013
                formula_for_case.succ.push_back(word_to_int(word_of_subst_var, int_version_of(subst_var), true, false));

                if (code_subst_vars.contains(subst_var)) {
                    // in the case that s_i is also one of the code vars, we need to force the exact value of code var, i.e., we add the optional part
                    //      code_version_of(s_i) = to_code(w_i)
                    if (word_of_subst_var.size() == 1) { // |w_i| = 1
                        mata::Symbol code_point = word_of_subst_var[0]; // this must be a code point of a digit, as w_i can only contain digits
                        formula_for_case.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{ code_version_of(subst_var), code_point });
                    } // "else" part is not needed, that should be solved by setting "|s_i| = |w_i|" and by using the formula from function get_formula_for_code_subst_vars()
                }

                // add w_i to the end of w
                full_word.insert(full_word.end(), word_of_subst_var.begin(), word_of_subst_var.end());
            }

            // add "i == to_int(w)" to C (we do not need to add |s| = |w|, that should come from |s| = |s1| + ... + |sn| and in C we talk about lengths of s_1,...,s_n)
            formula_for_case.succ.push_back(word_to_int(full_word, i, false, conv.type == ConversionType::FROM_INT));

            result.succ.push_back(formula_for_case);

        }

        return {result, res_precision};
    }

    /**
     * Creates a LIA formula that encodes to_code/from_code/to_int/from_int functions.
     * Assumes that
     *      - solution is flattened,
     *      - will be used in conjunction with the result of solution.get_lengths,
     *      - the resulting string variable of from_code/from_int is restricted to only valid results of from_code/from_int (should be done in theory_str_noodler::handle_conversion),
     *      - if to_int/from_int will be processed, code points of all digits (symbols 48,..,57) should be in the alphabet (should be done in theory_str_noodler::final_check_eh).
     * 
     * c = to_code(s)
     *  - we have the possible solutions of s, from these, we want to create LIA formula for all possible values of c
     *  - in the solution, s can be substituted: s = s_1 ... s_n (note that we should have |s| = |s_1| + ... + |s_n| from solution.get_lengths)
     *  - note that there can be another c' = to_code(s'), where some s_i can be also in the substitution of s',
     *    therefore, we need to share the information about s_i between s and s'
     *  - hence, for each s_i, we create an int variable code_version_of(s_i) which represents the possible code values of s_i
     *  - we then create formula
     *      (|s_i| != 1 && code_version_of(s_i) == -1) || (|s_i| == 1 && code_version_of(s_i) is code point of one of the chars in the language of automaton for s_i)
     *  - finally, we put
     *      (|s| != 1 && c == -1) || (|s| == 1 && c >= 0 && c is equal to one of code_version_of(s_i))                                               (1)
     *  - 'c >= 0' should force that c is equal to the code_version_of(s_i) which has '|s_i| == 1', because all others should be -1
     * 
     * s = from_code(c)
     *  - we have solutions for the result s, from this we can create LIA formula restricting the possible values of c
     *  - we can do the same thing as for to_code, but the first conjunct (|s| != 1 && c == -1) in (1) is replaced with
     *      (|s| == 0 && c is not a valid code point)
     *  - we assume that s is restricted to only values that can come from "from_code" (empty string or some char)
     *      - should be done during processing of from_code in theory_str_noodler, by adding a regular constraint
     *      - basically means that |s| <= 1
     * 
     * i = to_int(s)
     *  - same as to_code, we want to encode into LIA the possible values of i
     *  - again, s can be substituted: s = s_1 ... s_n, each s_i can be shared with variables from different to_int (or even to_code/from_code)
     *  - for each s_i, we create an int variable int_version_of(s_i), encoding the possible values of s_i as int (so that we can synch this value between different to_int...)
     *  - take each word w = w_1 ... w_n, where w_i is the word containing only digits from the language of the automaton for s_i (we assume there are only finite number of such words, otherwise ERROR)
     *      - create conjunction C of following conjuncts
     *              |s_i| == |w_i| && int_version_of(s_i) = to_int('1'.w_i) [ && code_version_of(s_i) = to_code(w_i) ]
     *          - last part in [] is optional, happens only if s_i substitutes some s used in to_code/from_code
     *          - to_int('1'.w_i) and to_code(w_i) can be computed, as w_i is a literal, not a variable
     *          - we add 1 at the beginning of int_version_of(s_i), because w_i can potentionally start with 0, but we want to encode the exact value of w_i ("005" and "5" should be taken as two different words)
     *          - if w_i is empty/contains non-digits, then to_int('1'.w_i) returns -1 (we do not need to differentiate between non-valid inputs)
     *      - add to C
     *              |s| == |w| && i == to_int(w)                                                                                            (2)
     *          - again, to_int(w) returns -1 for non-valid input (w is empt/contains non-digits)
     *      - the cases where w/w_i 
     *  - final LIA formula is taken as a disjunction of all the conjunctions from the previous step + for each s_i we also need to sort the part of its language with words containing some non-digit, see get_formula_for_int_conversion()
     * 
     * s = from_int(i)
     *  - similarly to from_code, we want to restrict the values of the argument i from the possible valuations of result s
     *  - we do the same thing as to_int, but instead of 'i == to_int(w)' in (2) for non-valid w, we put 'i < 0'
     *  - we assume (as in from_code) that s is restricted to only possible outputs of from_int, mainly that w cannot start with 0 and the only non-valid w is empty string
     */
    std::pair<LenNode, LenNodePrecision> DecisionProcedure::get_formula_for_conversions() {
        STRACE("str-conversion",
            tout << "Creating formula for conversions" << std::endl;
        );

        // the resulting formula 
        LenNode result(LenFormulaType::AND);
        LenNodePrecision res_precision = LenNodePrecision::PRECISE;

        // collect all code variables, i.e. those that substitute string variables used in to_code/from_code predicates
        std::set<BasicTerm> code_subst_vars = get_vars_substituted_in_code_conversions();

        result.succ.push_back(get_formula_for_code_subst_vars(code_subst_vars));

        for (const TermConversion& conv : conversions) {
            STRACE("str-conversion",
                tout << " processing " << get_conversion_name(conv.type) << " with string var " << conv.string_var << " and int var " << conv.int_var << std::endl;
            );

            switch (conv.type)
            {
            case ConversionType::TO_CODE:
            case ConversionType::FROM_CODE:
            {
                result.succ.push_back(get_formula_for_code_conversion(conv));
                break;
            }
            case ConversionType::TO_INT:
            case ConversionType::FROM_INT:
            {
                auto int_conv_formula_with_precision = get_formula_for_int_conversion(conv, code_subst_vars);
                result.succ.push_back(int_conv_formula_with_precision.first);
                if (int_conv_formula_with_precision.second != LenNodePrecision::PRECISE) {
                    res_precision = int_conv_formula_with_precision.second;
                }
                break;
            }
            default:
                UNREACHABLE();
            }
        }

        STRACE("str-conversion",
            tout << "Formula for conversions: " << result << std::endl;
        );
        return {result, res_precision};
    }

    /**
     * @brief Creates initial inclusion graph according to the preprocessed instance.
     */
    void DecisionProcedure::init_computation() {
        Formula equations;
        for (auto const &dis_or_eq : formula.get_predicates()) {
            if (dis_or_eq.is_equation()) {
                equations.add_predicate(dis_or_eq);
            } else if (dis_or_eq.is_inequation()) {
                for (auto const &eq_from_diseq : replace_disequality(dis_or_eq)) {
                    equations.add_predicate(eq_from_diseq);
                }
            } else {
                util::throw_error("Decision procedure can handle only equations and disequations");
            }
        }

        STRACE("str-dis",
            tout << "Disequation len formula: " << LenNode(LenFormulaType::AND, disequations_len_formula_conjuncts) << std::endl;
        );

        STRACE("str-dis",
            tout << "Equations after removing disequations" << std::endl;
            for (const auto &eq : equations.get_predicates()) {
                tout << "    " << eq << std::endl;
            }
        );

        SolvingState init_solving_state;
        init_solving_state.length_sensitive_vars = std::move(this->init_length_sensitive_vars);
        init_solving_state.aut_ass = std::move(this->init_aut_ass);

        if (!equations.get_predicates().empty()) {
            // TODO we probably want to completely get rid of inclusion graphs
            std::deque<std::shared_ptr<GraphNode>> tmp;
            Graph incl_graph = Graph::create_inclusion_graph(equations, tmp);
            for (auto const &node : incl_graph.get_nodes()) {
                init_solving_state.inclusions.insert(node->get_predicate());
                if (!incl_graph.is_on_cycle(node)) {
                    init_solving_state.inclusions_not_on_cycle.insert(node->get_predicate());
                }
            }
            // TODO the ordering of inclusions_to_process right now is given by how they were added from the splitting graph, should we use something different? also it is not deterministic now, depends on hashes
            while (!tmp.empty()) {
                init_solving_state.inclusions_to_process.push_back(tmp.front()->get_predicate());
                tmp.pop_front();
            }
        }

        worklist.push_back(init_solving_state);
    }

    lbool DecisionProcedure::preprocess(PreprocessType opt, const BasicTermEqiv &len_eq_vars) {
        FormulaPreprocessor prep_handler{std::move(this->formula), std::move(this->init_aut_ass), std::move(this->init_length_sensitive_vars), m_params};

        // we collect variables used in conversions, some preprocessing rules cannot be applied for them
        std::unordered_set<BasicTerm> conv_vars;
        for (const auto &conv : conversions) {
            conv_vars.insert(conv.string_var);
        }

        // So-far just lightweight preprocessing
        prep_handler.remove_trivial();
        prep_handler.reduce_diseqalities();
        if (opt == PreprocessType::UNDERAPPROX) {
            prep_handler.underapprox_languages();
        }
        prep_handler.propagate_eps();
        // Refinement of languages is beneficial only for instances containing not(contains) or disequalities (it is used to reduce the number of 
        // disequations/not(contains). For a strong reduction you need to have languages as precise as possible). In the case of 
        // pure equalitities it could create bigger automata, which may be problem later during the noodlification.
        if(this->formula.contains_pred_type(PredicateType::Inequation) || this->not_contains.get_predicates().size() > 0) {
            // Refine languages is applied in the order given by the predicates. Single iteration 
            // might not update crucial variables that could contradict the formula. 
            // Two iterations seem to be a good trade-off since the automata could explode in the fixpoint.
            prep_handler.refine_languages();
            prep_handler.refine_languages();
        }
        prep_handler.propagate_variables();
        prep_handler.propagate_eps();
        prep_handler.infer_alignment();
        prep_handler.remove_regular(conv_vars);
        // Skip_len_sat is not compatible with not(contains) and conversions as the preprocessing may skip equations with variables 
        // inside not(contains)/conversion. (Note that if opt == PreprocessType::UNDERAPPROX, there is no not(contains)).
        if(this->not_contains.get_predicates().empty() && this->conversions.empty()) {
            prep_handler.skip_len_sat();
        }
        prep_handler.generate_identities();
        prep_handler.propagate_variables();
        prep_handler.refine_languages();
        prep_handler.reduce_diseqalities();
        prep_handler.remove_trivial();
        prep_handler.reduce_regular_sequence(3);
        prep_handler.remove_regular(conv_vars);

        // the following should help with Leetcode
        /// TODO: should be simplyfied? So many preprocessing steps now
        STRACE("str",
            tout << "Variable equivalence classes: " << std::endl;
            for(const auto& t : len_eq_vars) {
                for (const auto& s : t) {
                    tout << s.to_string() << " ";
                }
                tout << std::endl;
            }   
        );
        prep_handler.generate_equiv(len_eq_vars);
        prep_handler.common_prefix_propagation();
        prep_handler.propagate_variables();
        prep_handler.generate_identities();
        prep_handler.remove_regular(conv_vars);
        prep_handler.propagate_variables();
        // underapproximation
        if(opt == PreprocessType::UNDERAPPROX) {
            prep_handler.underapprox_languages();
            prep_handler.skip_len_sat();
            prep_handler.reduce_regular_sequence(3);
            prep_handler.remove_regular(conv_vars);
            prep_handler.skip_len_sat();
        }
        prep_handler.reduce_regular_sequence(1);
        prep_handler.remove_regular(conv_vars);

        prep_handler.conversions_validity(conversions);

        // Refresh the instance
        this->formula = prep_handler.get_modified_formula();
        this->init_aut_ass = prep_handler.get_aut_assignment();
        this->init_length_sensitive_vars = prep_handler.get_len_variables();
        this->preprocessing_len_formula = prep_handler.get_len_formula();

        if (!this->init_aut_ass.is_sat()) {
            // some automaton in the assignment is empty => we won't find solution
            return l_false;
        }

        // try to replace the not contains predicates (so-far we replace it by regular constraints)
        if(replace_not_contains() == l_false || can_unify_not_contains(prep_handler) == l_true) {
            return l_false;
        }

        // there remains some not contains --> return undef
        if(this->not_contains.get_predicates().size() > 0) {
            return l_undef;
        }

        if(this->formula.get_predicates().size() > 0) {
            this->init_aut_ass.reduce(); // reduce all automata in the automata assignment
        }

        if(prep_handler.contains_unsat_eqs_or_diseqs()) {
            return l_false;
        }

        STRACE("str-nfa", tout << "Automata after preprocessing" << std::endl << init_aut_ass.print());
        STRACE("str", tout << "Lenght formula from preprocessing:" << preprocessing_len_formula << std::endl);
        STRACE("str",
            tout << "Length variables after preprocesssing:";
            for (const auto &len_var : init_length_sensitive_vars) {
                tout << " " << len_var;
            }
            tout << std::endl);
        STRACE("str", tout << "Formula after preprocessing:" << std::endl << this->formula.to_string() << std::endl; );

        if (!this->init_aut_ass.is_sat()) {
            // some automaton in the assignment is empty => we won't find solution
            return l_false;
        } else if (this->formula.get_predicates().empty()) {
            // preprocessing solved all (dis)equations => we set the solution (for lengths check)
            this->solution = SolvingState(this->init_aut_ass, {}, {}, {}, this->init_length_sensitive_vars, {});
            return l_true;
        } else {
            // preprocessing was not able to solve it, we at least reduce the size of created automata
            this->init_aut_ass.reduce();
            return l_undef;
        }
    }

    /**
     * Replace disequality @p diseq L != P by equalities L = x1a1y1 and R = x2a2y2
     * where x1,x2,y1,y2 \in \Sigma* and a1,a2 \in \Sigma \cup {\epsilon} and
     * also create arithmetic formula:
     *   |x1| = |x2| && to_code(a1) != to_code(a2) && (|a1| = 0 => |y1| = 0) && (|a2| = 0 => |y2| = 0)
     * The variables a1/a2 represent the characters on which the two sides differ
     * (they have different code values). They have to occur on the same position,
     * i.e. lengths of x1 and x2 are equal. The situation where one of the a1/a2
     * is empty word (to_code returns -1) represents that one of the sides is
     * longer than the other (they differ on the character just after the last
     * character of the shorter side). We have to force that nothing is after
     * the empty a1/a2, i.e. length of y1/y2 must be 0.
     */
    std::vector<Predicate> DecisionProcedure::replace_disequality(Predicate diseq) {

        // automaton accepting empty word or exactly one symbol
        std::shared_ptr<mata::nfa::Nfa> sigma_eps_automaton = std::make_shared<mata::nfa::Nfa>(init_aut_ass.sigma_eps_automaton());

        // function that will take a1 and a2 and create the "to_code(a1) != to_code(a2)" part of the arithmetic formula
        auto create_to_code_ineq = [this](const BasicTerm& var1, const BasicTerm& var2) {
                // we are going to check that to_code(var1) != to_code(var2), we need exact languages, so we make them length
                init_length_sensitive_vars.insert(var1);
                init_length_sensitive_vars.insert(var2);

                // variables that are results of to_code applied to var1/var2
                BasicTerm var1_to_code = BasicTerm(BasicTermType::Variable, var1.get_name().encode() + "!ineq_to_code");
                BasicTerm var2_to_code = BasicTerm(BasicTermType::Variable, var2.get_name().encode() + "!ineq_to_code");

                // add the information that we need to process "var1_to_code = to_code(var1)" and "var2_to_code = to_code(var2)"
                conversions.emplace_back(ConversionType::TO_CODE, var1, var1_to_code);
                conversions.emplace_back(ConversionType::TO_CODE, var2, var2_to_code);

                // add to_code(var1) != to_code(var2) to the len formula for disequations
                disequations_len_formula_conjuncts.push_back(LenNode(LenFormulaType::NEQ, {var1_to_code, var2_to_code}));
        };

        // This optimization represents the situation where L = a1 and R = a2
        // and we know that a1,a2 \in \Sigma \cup {\epsilon}, i.e. we do not create new equations.
        if(diseq.get_left_side().size() == 1 && diseq.get_right_side().size() == 1) {
            BasicTerm a1 = diseq.get_left_side()[0];
            BasicTerm a2 = diseq.get_right_side()[0];
            auto autl = init_aut_ass.at(a1);
            auto autr = init_aut_ass.at(a2);

            if(mata::nfa::is_included(*autl, *sigma_eps_automaton) && mata::nfa::is_included(*autr, *sigma_eps_automaton)) {
                // create to_code(a1) != to_code(a2)
                create_to_code_ineq(a1, a2);
                STRACE("str-dis", tout << "from disequation " << diseq << " no new equations were created" << std::endl;);
                return std::vector<Predicate>();
            }
        }

        // automaton accepting everything
        std::shared_ptr<mata::nfa::Nfa> sigma_star_automaton = std::make_shared<mata::nfa::Nfa>(init_aut_ass.sigma_star_automaton());

        BasicTerm x1 = util::mk_noodler_var_fresh("diseq_start");
        init_aut_ass[x1] = sigma_star_automaton;
        BasicTerm a1 = util::mk_noodler_var_fresh("diseq_char");
        init_aut_ass[a1] = sigma_eps_automaton;
        BasicTerm y1 = util::mk_noodler_var_fresh("diseq_end");
        init_aut_ass[y1] = sigma_star_automaton;
        BasicTerm x2 = util::mk_noodler_var_fresh("diseq_start");
        init_aut_ass[x2] = sigma_star_automaton;
        BasicTerm a2 = util::mk_noodler_var_fresh("diseq_char");
        init_aut_ass[a2] = sigma_eps_automaton;
        BasicTerm y2 = util::mk_noodler_var_fresh("diseq_end");
        init_aut_ass[y2] = sigma_star_automaton;

        std::vector<Predicate> new_eqs;
        // L = x1a1y1
        new_eqs.push_back(Predicate(PredicateType::Equation, {diseq.get_left_side(), Concat{x1, a1, y1}}));
        // R = x2a2y2
        new_eqs.push_back(Predicate(PredicateType::Equation, {diseq.get_right_side(), Concat{x2, a2, y2}}));

        // we want |x1| == |x2|, making x1 and x2 length ones
        init_length_sensitive_vars.insert(x1);
        init_length_sensitive_vars.insert(x2);
        // |x1| = |x2|
        disequations_len_formula_conjuncts.push_back(LenNode(LenFormulaType::EQ, {x1, x2}));

        // create to_code(a1) != to_code(a2)
        create_to_code_ineq(a1, a2);

        // we are also going to check for the lengths of y1 and y2, so they have to be length
        init_length_sensitive_vars.insert(y1);
        init_length_sensitive_vars.insert(y2);
        // (|a1| = 0) => (|y1| = 0)
        disequations_len_formula_conjuncts.push_back(LenNode(LenFormulaType::OR, {LenNode(LenFormulaType::NEQ, {a1, 0}), LenNode(LenFormulaType::EQ, {y1, 0})}));
        // (|a2| = 0) => (|y2| = 0)
        disequations_len_formula_conjuncts.push_back(LenNode(LenFormulaType::OR, {LenNode(LenFormulaType::NEQ, {a2, 0}), LenNode(LenFormulaType::EQ, {y2, 0})}));

        STRACE("str-dis", tout << "from disequation " << diseq << " created equations: " << new_eqs[0] << " and " << new_eqs[1] << std::endl;);
        return new_eqs;
    }

    /**
     * @brief Try to replace not contains predicates. In particular, we replace predicates of the form (not_contains lit x) where 
     * lit is a literal by a regular constraint x notin Alit' where  Alit' was obtained from A(lit) by setting all 
     * states initial and final. 
     */
    lbool DecisionProcedure::replace_not_contains() {
        Formula remain_not_contains{};
        for(const Predicate& pred : this->not_contains.get_predicates()) {
            Concat left = pred.get_params()[0];
            Concat right = pred.get_params()[1];
            if(left.size() == 1 && right.size() == 1) {
                if(this->init_aut_ass.is_singleton(left[0]) && this->init_aut_ass.is_singleton(right[0])) {
                    if(mata::nfa::are_equivalent(*this->init_aut_ass.at(left[0]), *this->init_aut_ass.at(right[0]))) {
                        return l_false;
                    }
                }
            }
            if(left.size() == 1 && right.size() == 1) {
                if(this->init_aut_ass.is_singleton(left[0]) && right[0].is_variable()) {
                    mata::nfa::Nfa nfa_copy = *this->init_aut_ass.at(left[0]);
                    for(unsigned i = 0; i < nfa_copy.num_of_states(); i++) {
                        nfa_copy.initial.insert(i);
                        nfa_copy.final.insert(i);
                    }

                    mata::nfa::Nfa complement = this->init_aut_ass.complement_aut(nfa_copy);
                    this->init_aut_ass.restrict_lang(right[0], complement);
                    continue;
                }
            }
            if(right.size() == 1 && this->init_aut_ass.is_epsilon(right[0])) {
                return l_false;
            }
            remain_not_contains.add_predicate(pred);
        }
        this->not_contains = remain_not_contains;
        return l_undef;
    }

    /**
     * @brief Check if it is possible to syntactically unify not contains terms. If they are included (in the sense of vectors) the 
     * not(contain) is unsatisfiable.
     * 
     * @param prep FormulaPreprocessor
     * @return l_true -> can be unified
     */
    lbool DecisionProcedure::can_unify_not_contains(const FormulaPreprocessor& prep) {
        for(const Predicate& pred : this->not_contains.get_predicates()) {
            if(prep.can_unify_contain(pred.get_params()[0], pred.get_params()[1])) {
                return l_true;
            }

        }
        return l_undef;
    }

    expr_ref DecisionProcedure::len_node_to_z3_formula(const LenNode& len_formula) {
        auto [a, b, c, d] = this->vars_for_lengths;
        printf("%d", a.empty());
        return util::len_to_expr(
                 len_formula,
                 a,
                 b, c, d );
    }

} // Namespace smt::noodler.
