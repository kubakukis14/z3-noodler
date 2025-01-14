
#pragma once

#include "util/params.h"

struct theory_str_noodler_params {
   
    bool m_underapproximation = false;
    bool m_preprocess_red = false;
    bool m_loop_protect = false;
    bool m_try_nielsen = false;

    theory_str_noodler_params(params_ref const & p = params_ref()) {
        updt_params(p);
    }

    void updt_params(params_ref const & p);
    void display(std::ostream & out) const;
};

