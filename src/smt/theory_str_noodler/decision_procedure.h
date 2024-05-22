#ifndef _NOODLER_DECISION_PROCEDURE_H_
#define _NOODLER_DECISION_PROCEDURE_H_

#include <memory>
#include <deque>
#include <algorithm>

#include "smt/params/theory_str_noodler_params.h"
#include "formula.h"
#include "inclusion_graph.h"
#include "aut_assignment.h"
#include "state_len.h"
#include "formula_preprocess.h"
#include "expr_solver.h"

namespace smt::noodler {

    /**
     * @brief Preprocess options
     */
    enum struct PreprocessType {
        PLAIN,
        UNDERAPPROX
    };

    /**
     * @brief Length formula precision
     */
    enum struct LenNodePrecision {
        PRECISE,
        UNDERAPPROX,
        OVERAPPROX,
    };

    /**
     * @brief Get the value of the symbol representing all symbols not ocurring in the formula (i.e. a minterm)
     * 
     * Dummy symbol represents all symbols not occuring in the problem. It is needed,
     * because if we have for example disequation x != y and nothing else, we would
     * have no symbols and incorrectly say it is unsat. Similarly, for 'x not in "aaa"
     * and |x| = 3', we would only get symbol 'a' and say (incorrectly) unsat. This
     * symbol however needs to have special semantics, for example to_code should
     * interpret is as anything but used symbols.
     */
    inline mata::Symbol get_dummy_symbol() { static const mata::Symbol DUMMY_SYMBOL = zstring::max_char() + 1; return DUMMY_SYMBOL; }
    inline bool is_dummy_symbol(mata::Symbol sym) { return sym == get_dummy_symbol(); }

    /**
     * @brief Abstract decision procedure. Defines interface for decision
     * procedures to be used within z3.
     */
    class AbstractDecisionProcedure {
    public:

        /**
         * @brief Initialize the computation (supposed to be called after preprocess)
         */
        virtual void init_computation() {
            throw std::runtime_error("Unimplemented");
        }

        /**
         * Do some preprocessing (can be called only before init_computation). Can
         * potentionally already solve the formula.
         * 
         * @param opt The type of preprocessing
         * @param len_eq_vars Equivalence class holding variables with the same length
         * @return lbool representing whether preprocessing solved the formula
         */
        virtual lbool preprocess(PreprocessType opt = PreprocessType::PLAIN, const BasicTermEqiv &len_eq_vars = {}) {
            throw std::runtime_error("preprocess unimplemented");
        }

        /**
         * Compute next solution and save the satisfiable solution.
         * @return True if there is a satisfiable element in the worklist.
         */
        virtual lbool compute_next_solution() {
            throw std::runtime_error("Not implemented");
        }

        /**
         * Get length constraints for the initial assignment (possibly modified by preprocessing).
         * If it is unsatisfiable, it means that there is no possible solution.
         */
        virtual LenNode get_initial_lengths() {
            throw std::runtime_error("Unimplemented");
        }

        /**
         * Get length constraints for the solution. Assumes that we have some solution from
         * running compute_next_solution(), the solution is actually solution if the length
         * constraints hold.
         * 
         * The second element of the resulting pair marks whether the lennode is precise or
         * over/underapproximation.
         */
        virtual std::pair<LenNode, LenNodePrecision> get_lengths() {
            throw std::runtime_error("Unimplemented");
        }

        virtual ~AbstractDecisionProcedure()=default;
    };

    /// A state of decision procedure that can lead to a solution
    struct SolvingState {
        // aut_ass[x] assigns variable x to some automaton while substitution_map[x] maps variable x to
        // the concatenation of variables for which x was substituted (i.e. its automaton is concatenation
        // of the automata from these variables). Each variable is either assigned in aut_ass or
        // substituted in substitution_map, but not both!
        AutAssignment aut_ass;
        std::unordered_map<BasicTerm, std::vector<BasicTerm>> substitution_map;

        // set of inclusions where we are trying to find aut_ass + substitution_map such that they hold 
        std::set<Predicate> inclusions;
        // set of inclusion from the previous set that for sure are not on cycle in the inclusion graph
        // that would be generated from inclusions
        std::set<Predicate> inclusions_not_on_cycle;

        // contains inclusions where we need to check if it holds (and if not, do something so that the inclusion holds)
        std::deque<Predicate> inclusions_to_process;

        // the variables that have length constraint on them in the rest of formula
        std::unordered_set<BasicTerm> length_sensitive_vars;


        SolvingState() = default;
        SolvingState(AutAssignment aut_ass,
                     std::deque<Predicate> inclusions_to_process,
                     std::set<Predicate> inclusions,
                     std::set<Predicate> inclusions_not_on_cycle,
                     std::unordered_set<BasicTerm> length_sensitive_vars,
                     std::unordered_map<BasicTerm, std::vector<BasicTerm>> substitution_map)
                        : aut_ass(aut_ass),
                          substitution_map(substitution_map),
                          inclusions(inclusions),
                          inclusions_not_on_cycle(inclusions_not_on_cycle),
                          inclusions_to_process(inclusions_to_process),
                          length_sensitive_vars(length_sensitive_vars) {}

        /// pushes inclusion to the beginning of inclusions_to_process but only if it is not in it yet
        void push_front_unique(const Predicate &inclusion) {
            if (std::find(inclusions_to_process.begin(), inclusions_to_process.end(), inclusion) == inclusions_to_process.end()) {
                inclusions_to_process.push_front(inclusion);
            }
        }

        /// pushes node to the end of nodes_to_process but only if it is not in it yet
        void push_back_unique(const Predicate &inclusion) {
            if (std::find(inclusions_to_process.begin(), inclusions_to_process.end(), inclusion) == inclusions_to_process.end()) {
                inclusions_to_process.push_back(inclusion);
            }
        }

        /// pushes node either to the end or beginning of inclusions_to_process (according to @p to_back) but only if it is not in it yet
        void push_unique(const Predicate &inclusion, bool to_back) {
            if (to_back) {
                push_back_unique(inclusion);
            } else {
                push_front_unique(inclusion);
            }
        }

        /**
         * Checks whether @p inclusion would be on cycle in the inclusion graph (can overapproxamte
         * and say that inclusion is on cycle even if it is not).
         */
        bool is_inclusion_on_cycle(const Predicate &inclusion) {
            return (inclusions_not_on_cycle.count(inclusion) == 0);
        }

        /**
         * Adds inclusion @p inclusion to this solving state (i.e. we will start checking if
         * this inclusion should not be added to inclusion_to_process during the decision procedure). 
         * 
         * @param inclusion Inclusion to add
         * @param is_on_cycle Whether the inclusion would be on cycle in the inclusion graph (if not sure, set to true)
         */
        void add_inclusion(const Predicate &inclusion, bool is_on_cycle = true) {
            inclusions.insert(inclusion);
            if (!is_on_cycle) {
                inclusions_not_on_cycle.insert(inclusion);
            }
        }

        /**
         * Adds inclusion with sides @p left_side and @p right_side to this solving state (i.e. we will start checking if
         * this inclusion should not be added to inclusion_to_process during the decision procedure).
         * 
         * @param left_side Left side of the new inclusion
         * @param right_side Right side of the new inclusion
         * @param is_on_cycle Whether the inclusion would be on cycle in the inclusion graph (if not sure, set to true)
         * @return The newly added inclusion
         */
        Predicate add_inclusion(const std::vector<BasicTerm> &left_side, const std::vector<BasicTerm> &right_side, bool is_on_cycle = true) {
            Predicate new_inclusion{PredicateType::Equation, std::vector<std::vector<BasicTerm>> {left_side, right_side}};
            add_inclusion(new_inclusion);
            return new_inclusion;
        }

        void remove_inclusion(const Predicate &inclusion) {
            inclusions.erase(inclusion);
            inclusions_not_on_cycle.erase(inclusion);
        }

        /**
         * Returns the vector of inclusions that would depend on the given @p inclusion in the inclusion graph.
         * That this all inclusions whose right side contain some variable from the left side of the given @p inclusion.
         * 
         * @param inclusion Inclusion whose dependencies we are looking for
         * @return The set of inclusions that depend on @p inclusion
         */
        std::vector<Predicate> get_dependent_inclusions(const Predicate &inclusion) {
            std::vector<Predicate> dependent_inclusions;
            auto left_vars_set = inclusion.get_left_set();
            for (const Predicate &other_inclusion : inclusions) {
                if (is_dependent(left_vars_set, other_inclusion.get_right_set())) {
                    dependent_inclusions.push_back(other_inclusion);
                }
            }
            return dependent_inclusions;
        }

        /**
         * Check if the vector @p right_side_vars depends on @p left_side_vars, i.e. if some variable
         * (NOT literal) occuring in @p right_side_vars occurs also in @p left_side_vars
         */
        static bool is_dependent(const std::set<BasicTerm> &left_side_vars, const std::set<BasicTerm> &right_side_vars) {
            if (left_side_vars.empty()) {
                return false;
            }
            for (auto const &right_var : right_side_vars) {
                if (right_var.is_variable() && left_side_vars.count(right_var) > 0) {
                    return true;
                }
            }
            return false;
        }

        // substitutes vars and merge same nodes + delete copies of the merged nodes from the inclusions_to_process (and also nodes that have same sides are deleted)
        void substitute_vars(std::unordered_map<BasicTerm, std::vector<BasicTerm>> &substitution_map);

        /**
         * @brief Get the length constraints for variable @p var
         * 
         * If @p var is substituted by x1x2x3... then it creates
         * |var| = |x1| + |x2| + |x3| + ... otherwise, if @p var
         * has an automaton assigned, it creates length constraint
         * representing all possible lengths of words in the automaton.
         */
        LenNode get_lengths(const BasicTerm& var) const;

        /**
         * @brief Flattens substitution_map so that each var maps only to vars in aut_assignment
         *
         * For example, if we have substitution_map[x] = yz, substitution_map[y] = z, and z is in aut_assignment, then at the end
         * we will have substitution_map[x] = zz and substitution_map[y] = z
         */
        void flatten_substition_map();

        /**
         * @brief Checks if @p var is substituted by empty word
         */
        bool is_var_empty_word(const BasicTerm& var) { return (substitution_map.count(var) > 0 && substitution_map.at(var).empty()); }

        /**
         * @brief Get the vector of variables substituting @p var.
         * 
         * In the case that @p var is not substituted (it is mapped to automaton), we return { @p var }.
         * Useful especially after calling flatten_subtitution_map().
         */
        std::vector<BasicTerm> get_substituted_vars(const BasicTerm& var) {
            if (substitution_map.count(var) > 0)
                return substitution_map.at(var);
            else
                return std::vector<BasicTerm>{var};
        }
    };

    class DecisionProcedure : public AbstractDecisionProcedure {
    protected:
        // counter of noodlifications
        unsigned noodlification_no = 0;

        // a deque containing states of decision procedure, each of them can lead to a solution
        std::deque<SolvingState> worklist;

        /// State of a found satisfiable solution set when one is computed using
        ///  compute_next_solution() or after preprocess()
        SolvingState solution;

        // initial length vars, formula and automata assignment, can be updated by preprocessing, used for initializing the decision procedure
        std::unordered_set<BasicTerm> init_length_sensitive_vars;
        Formula formula;
        AutAssignment init_aut_ass;
        // contains to/from_code/int conversions
        std::vector<TermConversion> conversions;

        // the length formula from preprocessing, get_lengths should create conjunct with it
        LenNode preprocessing_len_formula = LenNode(LenFormulaType::TRUE,{});
        // keeps the length formulas from replace_disequality(), they need to hold for solution to be satisfiable (get_lengths should create conjunct from them)
        std::vector<LenNode> disequations_len_formula_conjuncts;

        int_expr_solver& int_solver;

        std::tuple<const std::map<smt::noodler::BasicTerm, expr_ref>&, ast_manager&, seq_util&, arith_util&> vars_for_lengths;

         ast_manager& manager;

         context& ctx;

         smt_params& fparams;

         vector<std::pair<expr_ref, expr_ref>>& m_word_diseq_todo_rel;

         vector<std::pair<expr_ref, expr_ref>>& m_word_eq_todo_rel;

         scoped_vector<std::pair<expr_ref, expr_ref>>& m_not_contains_todo;

         scoped_vector<std::tuple<expr_ref,expr_ref,ConversionType>>& m_conversion_todo;

         bool check_lengths;

        const theory_str_noodler_params& m_params;

        /**
         * @brief Replace disequality L != R with equalities and a length constraint saved in disequations_len_formula_conjuncts.
         * 
         * @param diseq Disequality to replace
         * @return Vector with created equalities
         */
        std::vector<Predicate> replace_disequality(Predicate diseq);

        /**
         * @brief Gets the formula encoding to_code/from_code/to_int/from_int conversions
         */
        std::pair<LenNode, LenNodePrecision> get_formula_for_conversions();

        /**
         * Returns the code var version of @p var used to encode to_code/from_code in get_formula_for_conversions
         */
        BasicTerm code_version_of(const BasicTerm& var) {
            return BasicTerm(BasicTermType::Variable, var.get_name() + "!to_code");
        }

        /**
         * Returns the int var version of @p var used to encode to_int/from_int in get_formula_for_conversions
         */
        BasicTerm int_version_of(const BasicTerm& var) {
            return BasicTerm(BasicTermType::Variable, var.get_name() + "!to_int");
        }

        /**
         * Gets all vars s_i, such that there exists `c = to_code(s)` or `s = from_code(c)`
         * in conversions where s is substituted by s_1 ... s_i ... s_n in the solution.
         */
        std::set<BasicTerm> get_vars_substituted_in_code_conversions();

        /**
         * @brief Get the formula for code substituting variables
         * 
         * It basically encodes `code_version_of(c) = to_code(c)` for each c in @p code_subst_vars
         */
        LenNode get_formula_for_code_subst_vars(const std::set<BasicTerm>& code_subst_vars);

        /**
         * @brief Returns formula encoding `var = to_int(word)`.
         * 
         * Based on handle_invalid_as_from_int, invalid inputs (word is empty/contains non-digits) are either
         *    - (false) handled normally, i.e., to_int(word) = -1, or
         *    - (true) handled as if we had `word = from_int(var)`, i.e., var < 0.
         * 
         * @param start_with_one if true, encode instead `var = to_int('1'.word)`
         */
        LenNode word_to_int(const mata::Word& word, const BasicTerm &var, bool start_with_one, bool handle_invalid_as_from_int);

        /**
         * @brief Get the formula encoding to_code/from_code conversion
         */
        LenNode get_formula_for_code_conversion(const TermConversion& conv);

        /**
         * @brief Get the formula encoding to_int/from_int conversion
         * 
         * @param underapproximating_length For the case that we need to underapproximate, this variable sets
         * the length up to which we underapproximate
         */
        std::pair<LenNode, LenNodePrecision> get_formula_for_int_conversion(const TermConversion& conv, const std::set<BasicTerm>& code_subst_vars, const unsigned underapproximating_length = 3);

        /**
         * Formula containing all not_contains predicate (nothing else)
         */
        Formula not_contains{};

        /**
         * @brief Construct constraints to get rid of not_contains predicates.
         * @return l_false -> unsatisfiable constaint; l_undef if it is not evident
         */
        lbool replace_not_contains();

        /**
         * @brief Check if it is possible to syntactically unify not contains terms. If they are included (in the sense of vectors) the 
         * not(contain) is unsatisfiable.
         * 
         * @param prep FormulaPreprocessor
         * @return l_true -> can be unified 
         */
        lbool can_unify_not_contains(const FormulaPreprocessor& prep);

    public:

        /**
         * Initialize a new decision procedure that can solve word equations
         * (equalities of concatenations of string variables) with regular constraints
         * (variables belong to some regular language represented by automaton) while
         * keeping the length dependencies between variables (for the variables that
         * occur in some length constraint in the rest of the formula).
         * 
         * @param formula encodes the string formula (including equations, not contains)
         * @param init_aut_ass gives regular constraints (maps each variable from @p equalities to some NFA), assumes all NFAs are non-empty
         * @param init_length_sensitive_vars the variables that occur in length constraints in the rest of formula
         * @param par Parameters for Noodler string theory.
         * @param conversions Contains to/from_code/int conversions (x,y,conversion) where x = conversion(y)
         */
        DecisionProcedure(
             Formula formula, AutAssignment init_aut_ass,
             std::unordered_set<BasicTerm> init_length_sensitive_vars,
             const theory_str_noodler_params &par,
             std::vector<TermConversion> conversions,
             int_expr_solver &int_solver,
             std::tuple<const std::map<smt::noodler::BasicTerm, obj_ref<expr, ast_manager>>&, ast_manager&, seq_util&, arith_util&> vars_for_lengths,
              ast_manager& manager,
              context& ctx,
              smt_params& fparams,
              vector<std::pair<expr_ref, expr_ref>>& m_word_diseq_todo_rel,
              vector<std::pair<expr_ref, expr_ref>>& m_word_eq_todo_rel,
              scoped_vector<std::pair<expr_ref, expr_ref>>& m_not_contains_todo,
              scoped_vector<std::tuple<expr_ref,expr_ref,ConversionType>>& m_conversion_todo,
              bool check_lengths
        ) : init_length_sensitive_vars(init_length_sensitive_vars),
            formula(formula),
            init_aut_ass(init_aut_ass),
            conversions(conversions),
            int_solver(int_solver),
            vars_for_lengths(vars_for_lengths),
            manager(manager),
            ctx(ctx),
            fparams(fparams),
            m_word_diseq_todo_rel(m_word_diseq_todo_rel),
            m_word_eq_todo_rel(m_word_eq_todo_rel),
            m_not_contains_todo(m_not_contains_todo),
            m_conversion_todo(m_conversion_todo),
            check_lengths(check_lengths),
            m_params(par) {

            // if (int_solver == nullptr) {
            //     std::cout << "bubak" << std::endl;
            // }
            
            // we extract from the input formula all not_contains predicates and add them to not_contains formula
            this->formula.extract_predicates(PredicateType::NotContains, this->not_contains);
        }
        
        /**
         * Do some preprocessing (can be called only before init_computation). Can
         * potentionally already solve the formula. If it solves the formula as sat
         * it is still needed to check for lengths.
         * 
         * @param opt The type of preprocessing
         * @param len_eq_vars Equivalence class holding variables with the same length
         * @return lbool representing whether preprocessing solved the formula
         */
        lbool preprocess(PreprocessType opt = PreprocessType::PLAIN, const BasicTermEqiv &len_eq_vars = {}) override;

        void init_computation() override;
        lbool compute_next_solution() override;

        LenNode get_initial_lengths() override;

        std::pair<LenNode, LenNodePrecision> get_lengths() override;

        std::pair<LenNode, LenNodePrecision> get_node_lengths(SolvingState solut, std::vector<LenNode> conjuncts);

        expr_ref len_node_to_z3_formula_swag(const LenNode& len_formula);
    };
}

#endif
