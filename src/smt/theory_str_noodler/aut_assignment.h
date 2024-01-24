
#ifndef Z3_STR_AUT_ASSIGNMENT_H_
#define Z3_STR_AUT_ASSIGNMENT_H_

#include <iostream>
#include <map>
#include <set>
#include <queue>
#include <string>
#include <memory>

#include <mata/nfa/nfa.hh>
#include <mata/nfa/strings.hh>
#include <mata/nfa/builder.hh>

#include "formula.h"

namespace smt::noodler {

    /**
     * hints for using AutAssignment:
     *   - use at() instead of [] operator for getting the value, use [] only for assigning
     *   - if you want to assign some NFA, use std::make_shared<mata::nfa::Nfa>(NFA)
     */
    class AutAssignment : public std::unordered_map<BasicTerm, std::shared_ptr<mata::nfa::Nfa>> {

    private:
        /// Union of all alphabets of automata in the aut assignment
        std::set<mata::Symbol> alphabet;

        void update_alphabet() {
            this->alphabet.clear();
            for (const auto& pr : *this) {
                auto alph_symbols = pr.second->alphabet == nullptr ? mata::nfa::create_alphabet(*(pr.second)).get_alphabet_symbols() : pr.second->alphabet->get_alphabet_symbols();
                this->alphabet.insert(alph_symbols.begin(), alph_symbols.end());
            }
        }

    public:
        using std::unordered_map<BasicTerm, std::shared_ptr<mata::nfa::Nfa>>::unordered_map;

        // used for tests, do not use normally
        AutAssignment(std::map<BasicTerm, mata::nfa::Nfa> val) {
            for (const auto &key_value : val) {
                this->operator[](key_value.first) = std::make_shared<mata::nfa::Nfa>(key_value.second);
            }
            update_alphabet();
        };

        mata::nfa::Nfa sigma_star_automaton() const {
            mata::nfa::Nfa nfa{};
            nfa.initial = {0};
            nfa.final = {0};
            for (const mata::Symbol& symb : this->alphabet) {
                nfa.delta.add(0, symb, 0);
            }
            return nfa;
        }

        mata::nfa::Nfa sigma_automaton() const {
            mata::nfa::Nfa nfa{};
            nfa.initial = {0};
            nfa.final = {1};
            for (const mata::Symbol& symb : this->alphabet) {
                nfa.delta.add(0, symb, 1);
            }
            return nfa;
        }

        mata::nfa::Nfa sigma_eps_automaton() const {
            mata::nfa::Nfa nfa{};
            nfa.initial = {0};
            nfa.final = {0,1};
            for (const mata::Symbol& symb : this->alphabet) {
                nfa.delta.add(0, symb, 1);
            }
            return nfa;
        }

        mata::nfa::Nfa get_automaton_concat(const std::vector<BasicTerm>& concat) const {
            mata::nfa::Nfa ret = mata::nfa::builder::create_empty_string_nfa();
            for(const BasicTerm& t : concat) {
                ret = mata::nfa::concatenate(ret, *(this->at(t)));  // fails when not found
            }
            return ret;
        }

        /**
         * @brief Checks if the automaton for @p t is equal to language containing only empty word.
         */
        bool is_epsilon(const BasicTerm &t) const {
            return mata::strings::is_lang_eps(*(this->at(t)));
        }

        /**
         * @brief Checks if the automaton for @p t contains empty word in its language.
         */
        bool contains_epsilon(const BasicTerm &t) const {
            return this->at(t)->is_in_lang({});
        }

        // adds all mappings of variables from other to this assignment except those which already exists in this assignment
        // i.e. if this[var] exists, then nothing happens for var, if it does not, then this[var] = other[var]
        // TODO: probably this is the same as just doing this->insert(other.begin(), other.end())
        // TODO: or even better, if we do not care what happens with other, we can use this->merge(other)
        void add_to_assignment(const AutAssignment &other) {
            for (const auto &it : other) {
                if (this->count(it.first) == 0) {
                    (*this)[it.first] = it.second;
                }
            }
        }

        const std::set<mata::Symbol> get_alphabet(bool recompute=false) {
            if(recompute) update_alphabet();
            return this->alphabet;
        }

        void set_alphabet(const std::set<uint32_t>& alphabet) {
            this->alphabet.clear();
            for (const auto& symbol : alphabet) {
                this->alphabet.insert(symbol);
            }
        }

        /**
         * @brief Is language complement of a finite language?
         * 
         * @param t Variable whose language to be checked
         * @return true Is complement of a finite language
         */
        bool is_co_finite(const BasicTerm& t) const {
            mata::OnTheFlyAlphabet mata_alphabet{};
            for (const auto& symbol : this->alphabet) {
                mata_alphabet.add_new_symbol(std::to_string(symbol), symbol);
            }

            mata::nfa::Nfa cmp = mata::nfa::minimize(mata::nfa::complement(*(*this).at(t), mata_alphabet));
            return cmp.trim().is_acyclic();
        }

        /**
         * @brief Check if the automaton accepts only a single word. It is only underapproximation,
         * as is_singleton is false for the NFA p1 -a-> p2, q1 -a-> q2 where p1, q1 are initial and p2, q2 are final.
         * To get a precise information, the determinization+minimization might be necessary, which is often 
         * expensive. 
         * 
         * @param t Variable
         * @return True -> is surely singleton, False -> inconclusive
         */
        bool is_singleton(const BasicTerm& t) const {
            mata::nfa::Nfa aut = *this->at(t);
            aut.trim();
            return aut.num_of_states() == aut.delta.num_of_transitions() + 1 && aut.initial.size() == 1 && aut.final.size() == 1;
        }

        /**
         * @brief Check if the given terms have disjoint languages.
         * 
         * @param t1 First term
         * @param t2 Second term
         * @return true <-> L(t1) and L(t2) are disjoint.
         */
        bool are_disjoint(const BasicTerm &t1, const BasicTerm& t2) const {
            mata::nfa::Nfa aut_t1 = *this->at(t1);
            mata::nfa::Nfa aut_t2 = *this->at(t2);
            return  mata::nfa::intersection(aut_t1, aut_t2).is_lang_empty();
        }

        /**
         * @brief Check if the language of the basic term has a fixed length
         * 
         * @param t BasicTerm
         * @param n[out] fixed length
         * @return True->fixed length
         */
        bool fixed_length(const BasicTerm& t, int& n) const {
            auto lengths = mata::strings::get_word_lengths(*this->at(t));
            if(lengths.size() != 1) {
                return false;
            }
            auto pr = *lengths.begin();
            n = pr.first;
            return pr.second == 0;
        }

        /**
         * @brief Check if all automata in the map have non-empty language.
         *
         * @return true All have non-empty language
         * @return false There is at least one NFA with the empty language
         */
        bool is_sat() const {
            for (const auto& pr : *this) {
                if(pr.second->final.size() == 0) {
                    return false;
                }
                if(pr.second->is_lang_empty())
                    return false;
            }
            return true;
        }

        /**
         * @brief Reduce all automata occurring in the map.
         */
        void reduce() {
             for (auto& pr : *this) {
                pr.second = std::make_shared<mata::nfa::Nfa>(mata::nfa::reduce(*pr.second));
            }
        }

        /**
         * @brief Get all keys from the assignment
         * 
         * @return std::unordered_set<BasicTerm> Keys
         */
        std::unordered_set<BasicTerm> get_keys() const {
            std::unordered_set<BasicTerm> ret;
            for(const auto & pr : *this) {
                ret.insert(pr.first);
            }
            return ret;
        }

        std::string print() {
            std::stringstream res;
            for (const auto &key_val : *this) {
                res << "Automaton for " << key_val.first.get_name() << ":" << std::endl;
                key_val.second->print_to_DOT(res);
            }
            return res.str();
        }

        /**
         * @brief Restrict language of the given basic term @p t by @p restr_nfa.  
         * 
         * @param t Basic term to be restricted
         * @param restr_nfa Language restriction represented by an NFA.
         */
        void restrict_lang(const BasicTerm& t, const mata::nfa::Nfa& restr_nfa) {
            (*this)[t] = std::make_shared<mata::nfa::Nfa>(mata::nfa::intersection(restr_nfa, *this->at(t)));
        }

        /**
         * @brief Get the length formula representing all possible lengths of the automaton for @p var
         */
        LenNode get_lengths(const BasicTerm& var) const;

        /**
         * @brief Get the lengths formula representing all possible lengths of the automaton for @p var and corresponding NFA @p aut.
         */
        static LenNode get_lengths(const mata::nfa::Nfa& aut, const BasicTerm& var);

        /**
         * Create NFA accepting a word in Z3 zstring representation.
         * @param word Word to accept.
         * @return NFA.
         */
        static mata::nfa::Nfa create_word_nfa(const zstring& word);

        /**
         * @brief Complement the given automaton wrt the alphabet induced by the AutAssignment.
         * 
         * @param aut Automaton to be complemented
         * @return mata::nfa::Nfa 
         */
        mata::nfa::Nfa complement_aut(mata::nfa::Nfa& aut) {
            auto alphabet =  this->get_alphabet(false);
            mata::OnTheFlyAlphabet mata_alphabet{};
            for (const auto& symbol : alphabet) {
                mata_alphabet.add_new_symbol(std::to_string(symbol), symbol);
            }
            return mata::nfa::complement(aut, mata_alphabet);
        }

        /**
         * @brief Get complement of the term language wrt the alphabet induced by the AutAssignment. 
         * 
         * @param t Term 
         * @return mata::nfa::Nfa 
         */
        mata::nfa::Nfa complement_lang(const BasicTerm& t) {
            return complement_aut(*(this->at(t)));
        }

    };

} // Namespace smt::noodler.

#endif //Z3_STR_AUT_ASSIGNMENT_H_
