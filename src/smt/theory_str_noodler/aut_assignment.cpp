#include "aut_assignment.h"
#include "util.h"

namespace smt::noodler {
    LenNode AutAssignment::get_lengths(const BasicTerm& var) const {
        // each (c1, c2) from following set represents the lengths of automaton for var
        // where we take c1 + k*c2 for each k >= 0
        std::set<std::pair<int, int>> aut_constr = Mata::Strings::get_word_lengths(*at(var));

        LenNode res(LenFormulaType::FALSE, {});
        for(const auto& cns : aut_constr) { // for each (c1, c2) representing lengths of var
            LenNode c1(cns.first);
            LenNode c2(cns.second);
            LenNode k(util::mk_noodler_var_fresh("k"));

            if(cns.second != 0) {
                // c1 + k*c2
                LenNode right(LenFormulaType::PLUS, {c1, LenNode(LenFormulaType::TIMES, {k, c2})});
                // add (var = c1 + k*c2 && 0 <= k) to result
                res = LenNode(LenFormulaType::OR, 
                                {res,
                                 LenNode(LenFormulaType::AND,
                                            {LenNode(LenFormulaType::EQ, {var, right}),
                                             LenNode(LenFormulaType::LEQ, {LenNode(0), k})
                                            })
                                });
            } else {
                // add (var = c1) to result
                res = LenNode(LenFormulaType::OR, {res, LenNode(LenFormulaType::EQ, {var, c1})});
            }
        }

        // to be safe, var must be >= 0
        res = LenNode(LenFormulaType::AND, {res, LenNode(LenFormulaType::LEQ, {0, var})});
        return res;
    }
}