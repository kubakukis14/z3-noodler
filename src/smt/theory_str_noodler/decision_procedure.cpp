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

        while (!worklist.empty()) {
            SolvingState element_to_process = std::move(worklist.front());
            worklist.pop_front();

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

            STRACE("str", tout << "noodlecount: " << noodles.size() << std::endl;);
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

            /**
             * The following code focuses on reducing the case split, created by the noodlification, as much as possible.
             * The algorithm is based on inclusion checking between all produced noodles. The inclusions are processed
             * on the level of separate bubbles that make the noodles up. First, we compare the right sides of the bubbles (the alignments),
             * and if they get past, we check inclusions of their respective left sides (automata). The final product 
             * is a distinct set of noodles, contained within the noodles vector.
             */
            std::vector<std::vector<std::pair<std::shared_ptr<mata::nfa::Nfa>, mata::strings::seg_nfa::VisitedEpsilonsCounterVector>>> newNoodles;
            for (size_t i = 0; i < noodles.size(); i++){
                // load the i-th noodle into variable udon
                auto udon = noodles[i];
                bool push_udon = true;
                size_t newNoodles_size = newNoodles.size();
                // loop through all variables inside the noodle
                for (size_t j = 0; j < newNoodles_size; j++){
                    auto soba = newNoodles[j];
                    size_t l = 0;
                    // Now the sizes of the udon.first and soba.first do not have to be the same so how do we compare them?
                    size_t bubble_count = udon.size() < soba.size() ? udon.size() : soba.size();
                    bool soba_larger = udon.size() < soba.size() ? true : false;
                    auto smaller_nood = udon.size() < soba.size() ? udon : soba;
                    auto bigger_nood = udon.size() >= soba.size() ? udon : soba;
                    bool skipped_noodle = false;
                    bool deletion_time_udon = false;
                    bool deletion_time_soba = false;

                    // based on these two flags, we later decide about the deletion of the noodle
                    bool udon_is_smaller = true;
                    bool soba_is_smaller = true;
                    for (size_t k = 0; k < bubble_count; k++){

                        // In case of alignment mismatches, we try to catch up with the smaller noodle 
                        // if there are epsilons language automata. If there are none, or the bigger noodle overtakes 
                        // the smaller one, we can skip checking inclusions on the noodle and push it.
                        while ((smaller_nood[k].second != bigger_nood[l].second) && (l < bigger_nood.size())){
                            if (!(bigger_nood[l].first->final == bigger_nood[l].first->initial)) {
                                STRACE("str", tout << "skipping inclusion" << std::endl;);
                                skipped_noodle = true;
                                break;
                            }

                            l++;

                            if  ((bigger_nood[l].second[0] > smaller_nood[k].second[0]) ||
                                 (bigger_nood[l].second[1] > smaller_nood[k].second[1])) {
                                STRACE("str", tout << "skipping inclusion" << std::endl;);
                                skipped_noodle = true;
                                break;
                            }
                        }

                        if (skipped_noodle){
                            break;
                        }

                        // helper pointers to the right and left variable sides
                        std::vector<unsigned int> soba_right_side;
                        std::vector<unsigned int> udon_right_side;
                        std::shared_ptr<mata::nfa::Nfa> soba_first;
                        std::shared_ptr<mata::nfa::Nfa> udon_first;

                        if (soba_larger){
                            soba_right_side = soba[l].second;
                            udon_right_side = udon[k].second;
                            soba_first = soba[l].first;
                            udon_first = udon[k].first;
                        } else {
                            soba_right_side = soba[k].second;
                            udon_right_side = udon[l].second;
                            soba_first = soba[k].first;
                            udon_first = udon[l].first;
                        }


                        // if alignments match, we can compare the automata
                        if (udon_right_side == soba_right_side){
                            // the right sides are the same, we can call is_included
                            bool udon_in_soba = mata::nfa::is_included(*udon_first, *soba_first);
                            bool soba_in_udon = mata::nfa::is_included(*soba_first, *udon_first);
                            if (udon_in_soba && soba_in_udon){
                                STRACE("str", tout << "soba variable is the same as udon" << std::endl;);
                            }
                            else if (udon_in_soba){
                                // udon is smaller, soba is eaten
                                soba_is_smaller = false;
                                STRACE("str", tout << "udon variable is smaller than soba" << std::endl;);
                            } else if (soba_in_udon){
                                // soba is smaller, udon is eaten
                                udon_is_smaller = false;
                                STRACE("str", tout << "soba variable is smaller than udon" << std::endl;);
                            } else {
                                // they are not the same, we can push it to newNoodles
                                STRACE("str", tout << "soba and udon are not the same" << std::endl;);
                                break;
                            }
                        }

                        // at the end of the noodle, we can decide if we keep the noodle or not
                        if (k == bubble_count-1) {
                            if (udon_is_smaller && soba_is_smaller){
                                // both noodles are completely the same, we can delete one
                                deletion_time_udon = true;
                                STRACE("str", tout << "soba and udon are the same -> eaten" << std::endl;);
                                break;
                            } else if (udon_is_smaller){
                                // udon is smaller, soba is eaten
                                deletion_time_udon = true;
                                STRACE("str", tout << "udon is smaller -> eaten" << std::endl;);
                                break;
                            } else if (soba_is_smaller){
                                // soba is smaller, udon is eaten
                                deletion_time_soba = true;
                                STRACE("str", tout << "soba is smaller -> eaten" << std::endl;);
                                break;
                            }
                        }
                        l++;
                    }

                    // if the udon is smaller, we dont have to push it to newNoodles so we can continue
                    if (deletion_time_udon){
                        push_udon = false;
                        break;
                    }
                    // if the soba is smaller, we can delete it from newNoodles and resize the newNoodles
                    if (deletion_time_soba){
                        newNoodles.erase(newNoodles.begin() + j);
                    }
                }

                // We can push it to newNoodles?
                if (push_udon) {
                    newNoodles.push_back(udon);
                }
            }
            noodles = newNoodles;
            STRACE("str", tout << "noodlecount: " << noodles.size() << std::endl;);
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

    LenNode DecisionProcedure::get_lengths() {
        if (solution.length_sensitive_vars.empty()) {
            // There are no length vars (which also means no disequations nor conversions), it is not needed to create the lengths formula.
            return LenNode(LenFormulaType::TRUE);
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
        conjuncts.push_back(get_formula_for_conversions());

        return LenNode(LenFormulaType::AND, conjuncts);
    }

    LenNode DecisionProcedure::get_formula_for_conversions() {
        STRACE("str-conversion",
            tout << "Creating formula for conversions" << std::endl;
        );
        auto to_code_var = [](const BasicTerm& var) -> BasicTerm { return BasicTerm(BasicTermType::Variable, var.get_name() + "!to_code"); };

        std::vector<LenNode> result_conjuncts;
        for (const std::tuple<BasicTerm, BasicTerm, ConversionType>& transf : conversions) {
            BasicTerm result = std::get<0>(transf);
            BasicTerm argument = std::get<1>(transf);
            ConversionType type = std::get<2>(transf);
            switch (type)
            {
            case ConversionType::FROM_CODE:
                STRACE("str-conversion",
                    tout << " procesing from_code with result " << result << " and argument " << argument << " which is handled by" << std::flush;
                );
                std::swap(result, argument);
                // fall trough, we do nearly the same thing
            case ConversionType::TO_CODE:
            {
                STRACE("str-conversion",
                    tout << " procesing to_code with result " << result << " and argument " << argument << std::endl;
                );
                /* Having result=to_code(argument) we need to take all var_1 ... var_n
                 * substituting argument in solution. We need to have one of the |var_i| = 1
                 * and all others |var_j| = 0; var_i will be then the char whose code point
                 * we want to return. We keep this code point in int variable var_i!to_code,
                 * where result = var_i!to_code (we do this, so that different conversions
                 * are connected - they use the same !to_code variables).
                 * If this does not happen (i.e. argument is not word of length one), the result
                 * is equal to -1.
                 * 
                 * We also handle from_code here, we swapped arugment and result so we have
                 * argument = from_code(result). We can do exactly the same, we compute the
                 * code point from which we are constructing the char by checking possible
                 * chars in solution. The only difference is that if result is not a valid
                 * code point, then argument must be an empty string.
                 */
                mata::nfa::Nfa sigma_aut = solution.aut_ass.sigma_automaton();
                std::vector<BasicTerm> substituted_vars = solution.get_substituted_vars(argument);

                // Disjunction representing that result is equal to code point of one of the chars of some var_i
                LenNode result_solution(LenFormulaType::OR, {});
                for (const BasicTerm& var : substituted_vars) {
                    // disjunction that will say that var!to_code is equal to code point of one of the chars in var
                    LenNode to_code_disjunction = LenNode(LenFormulaType::OR, {});
                    for (mata::Symbol s : mata::strings::get_accepted_symbols(*solution.aut_ass.at(var))) { // iterate trough chars of var
                        if (!is_dummy_symbol(s)) {
                            // var!to_code == s
                            to_code_disjunction.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{to_code_var(var), s});
                        } else {
                            // if s represents symbols not in the alphabet => var!to_code must be a code point of such a symbol which is not in alphabet, i.e. it is...
                            // ...valid code point (0 <= var!to_code <= max_char) and...
                            std::vector<LenNode> minterm_to_code{LenNode(LenFormulaType::LEQ, {0, to_code_var(var)}), LenNode(LenFormulaType::LEQ, {to_code_var(var), zstring::max_char()})};
                            // ...it is not equal to code point of some symbol in the alphabet
                            for (mata::Symbol s2 : solution.aut_ass.get_alphabet()) {
                                if (!is_dummy_symbol(s2)) {
                                    minterm_to_code.emplace_back(LenFormulaType::NEQ, std::vector<LenNode>{to_code_var(var), s2});
                                }
                            }
                            to_code_disjunction.succ.emplace_back(LenFormulaType::AND, minterm_to_code);
                        }
                    }
                    result_solution.succ.emplace_back(LenFormulaType::AND, std::vector<LenNode>{
                        to_code_disjunction, // var!to_code is equal to code point of one of its symbols TODO: do this only once for given var
                        LenNode(LenFormulaType::EQ, {result, to_code_var(var)}), // result is equal to var!to_code
                        LenNode(LenFormulaType::EQ, {var, 1}) // lenght of var is 1
                    });
                }

                // sum_of_substituted_vars = |var_1| + |var_2| + ... + |var_n|
                LenNode sum_of_substituted_vars(LenFormulaType::PLUS, std::vector<LenNode>(substituted_vars.begin(), substituted_vars.end()));
                // result is defined, i.e. exactly one |var_i| = 1 (by checking sum_of_substituted_vars = 1) and the result is the code point of one of the chars of var_i
                LenNode result_is_defined(LenFormulaType::AND, {result_solution, LenNode(LenFormulaType::EQ, {sum_of_substituted_vars, 1})});

                LenNode result_is_undefined(LenFormulaType::AND, {});
                if (type == ConversionType::TO_CODE) {
                    // result is undefined, i.e. the argument is not a word of length one and result is then equal to -1
                    result_is_undefined.succ.emplace_back(LenFormulaType::NEQ, std::vector<LenNode>{sum_of_substituted_vars, 1});
                    result_is_undefined.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{result, -1});
                } else {
                    // we have argument = from_code(result), if result is not valid code point (0 <= result <= max_char() does not hold), then argument must be empty word (length is equal to 0)
                    result_is_undefined.succ.emplace_back(LenFormulaType::NOT, std::vector<LenNode>{LenNode(LenFormulaType::AND, {LenNode(LenFormulaType::LEQ, {0, result}), LenNode(LenFormulaType::LEQ, {result, zstring::max_char()})})});
                    result_is_undefined.succ.emplace_back(LenFormulaType::EQ, std::vector<LenNode>{argument, 0});
                }

                result_conjuncts.emplace_back(LenFormulaType::OR, std::vector<LenNode>{result_is_defined, result_is_undefined});

                // TODO needs to get all the to_code vars, so that it can be properly handled by to_int
                break;
            }
            case ConversionType::TO_INT:
            case ConversionType::FROM_INT:
                //???
                util::throw_error("unimplemented");
                break;
            
            default:
                UNREACHABLE();
            }
        }
        STRACE("str-conversion",
            tout << "Formula for conversions: " << LenNode(LenFormulaType::AND, result_conjuncts) << std::endl;
        );
        return LenNode(LenFormulaType::AND, result_conjuncts);
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
        prep_handler.remove_regular();
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
        prep_handler.remove_regular();

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
        prep_handler.remove_regular();
        prep_handler.propagate_variables();
        // underapproximation
        if(opt == PreprocessType::UNDERAPPROX) {
            prep_handler.underapprox_languages();
            prep_handler.skip_len_sat();
            prep_handler.reduce_regular_sequence(3);
            prep_handler.remove_regular();
            prep_handler.skip_len_sat();
        }
        prep_handler.reduce_regular_sequence(1);
        prep_handler.remove_regular();

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
                conversions.emplace_back(var1_to_code, var1, ConversionType::TO_CODE);
                conversions.emplace_back(var2_to_code, var2, ConversionType::TO_CODE);

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

                    mata::OnTheFlyAlphabet mata_alphabet{};
                    for (const auto& symbol : this->init_aut_ass.get_alphabet()) {
                        mata_alphabet.add_new_symbol(std::to_string(symbol), symbol);
                    }

                    mata::nfa::Nfa complement = mata::nfa::complement(nfa_copy, mata_alphabet);
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

} // Namespace smt::noodler.
