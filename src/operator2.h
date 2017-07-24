/*
 * @BEGIN LICENSE
 *
 * Forte: an open-source plugin to Psi4 (https://github.com/psi4/psi4)
 * that implements a variety of quantum chemistry methods for strongly
 * correlated electrons.
 *
 * Copyright (c) 2012-2017 by its authors (see COPYING, COPYING.LESSER, AUTHORS).
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * @END LICENSE
 */

#ifndef _wfn_operator_h_
#define _wfn_operator_h_

#include "determinant_map2.h"
#include "stl_determinant.h"
#include "fci/fci_integrals.h"

namespace psi {
namespace forte {

/**
 * @brief A class to compute various expectation values, projections,
 * and matrix elements of quantum mechanical operators on wavefunction objects.
 */

using wfn_hash = stldet_hash<double>;

class WFNOperator2 {
  public:
    /// Default constructor
    WFNOperator2(std::vector<int>& symmetry, std::shared_ptr<FCIIntegrals> fci_ints);

    /// Empty constructor
    WFNOperator2();

    /// Initializer
    void initialize(std::vector<int>& symmetry, std::shared_ptr<FCIIntegrals> );

    /// Set print level
    void set_quiet_mode(bool mode);

    /// Build the coupling lists for one-particle operators
    void op_s_lists(DeterminantMap2& wfn);

    /// Build the coupling lists for two-particle operators
    void tp_s_lists(DeterminantMap2& wfn);

    /// Build the coupling lists for three-particle operators
    void three_s_lists(DeterminantMap2& wfn);

    void clear_op_s_lists();
    void clear_tp_s_lists();
    /*- Operators -*/

    /// Single excitations, a_p^(+) a_q|>
    void add_singles(DeterminantMap2& wfn);

    /// Double excitations, a_p^(+) a_q^(+) a_r a_s|>
    void add_doubles(DeterminantMap2& wfn);

    /// Compute total spin expectation value <|S^2|>
    double s2(DeterminantMap2& wfn, SharedMatrix& evecs, int root);

    void build_strings(DeterminantMap2& wfn);

    /// Build the sparse Hamiltonian
    std::vector<std::pair<std::vector<size_t>, std::vector<double>>>
    build_H_sparse(const DeterminantMap2& wfn);

    /// Build the sparse Hamiltonian -v2
    std::vector<std::pair<std::vector<size_t>, std::vector<double>>>
    build_H_sparse2(const DeterminantMap2& wfn);

    std::vector<std::vector<std::pair<size_t, short>>> a_list_;
    std::vector<std::vector<std::pair<size_t, short>>> b_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> aa_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> bb_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> ab_list_;

    /// The alpha single-annihilation/creation list
    std::vector<std::vector<std::pair<size_t, short>>> a_ann_list_;
    std::vector<std::vector<std::pair<size_t, short>>> a_cre_list_;

    /// The beta single-annihilation/creation list
    std::vector<std::vector<std::pair<size_t, short>>> b_ann_list_;
    std::vector<std::vector<std::pair<size_t, short>>> b_cre_list_;

    /// The alpha-alpha double-annihilation/creation list
    std::vector<std::vector<std::tuple<size_t, short, short>>> aa_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> aa_cre_list_;

    /// The beta-beta single-annihilation/creation list
    std::vector<std::vector<std::tuple<size_t, short, short>>> bb_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> bb_cre_list_;

    /// The alfa-beta single-annihilation/creation list
    std::vector<std::vector<std::tuple<size_t, short, short>>> ab_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short>>> ab_cre_list_;

    /// Three particle lists
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aaa_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aab_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> abb_ann_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> bbb_ann_list_;

    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aaa_cre_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aab_cre_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> abb_cre_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> bbb_cre_list_;

    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aaa_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> aab_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> abb_list_;
    std::vector<std::vector<std::tuple<size_t, short, short, short>>> bbb_list_;

  protected:
    /// Initialize important variables on construction
    void startup();

    std::vector<std::vector<size_t>> beta_strings_;
    std::vector<std::vector<size_t>> alpha_strings_;
    std::vector<std::vector<std::pair<int, size_t>>> alpha_a_strings_;
    std::vector<std::vector<std::pair<int, size_t>>> beta_a_strings_;

    /// Active space symmetry
    std::vector<int> mo_symmetry_;
    std::shared_ptr<FCIIntegrals> fci_ints_;

    /// Print level
    bool quiet_ = false;
};
}
}

#endif // _wfn_operator_h_