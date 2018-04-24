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

#include "psi4/libpsi4util/process.h"
#include "psi4/libmints/molecule.h"

#include "../mini-boost/boost/format.hpp"

#include "../sparse_ci/determinant.h"
#include "../iterative_solvers.h"

#include "fci_solver.h"

#ifdef HAVE_GA
#include <ga.h>
#include <macdecls.h>
#endif

#include "psi4/psi4-dec.h"

#include <iostream>
#include <fstream>

using namespace psi;

namespace psi {
namespace forte {

class MOSpaceInfo;

void FCISolver::py_mat_print(SharedMatrix C_h, const std::string& input)
{
  std::ofstream myfile1;
  myfile1.open (input);
  for (int i=0; i<C_h->coldim(); i++){
    myfile1 << "\n";
    for (int j=0; j<C_h->rowdim(); j++){
        myfile1 << C_h->get(i,j) << " ";
    }
  }
  myfile1.close();
}

void FCISolver::tile_chopper(std::vector<SharedMatrix>& C, double tile_norm_cut,
                             FCIVector& HC, std::shared_ptr<FCIIntegrals> fci_ints,
                             double fci_energy, int dim)
{
  double nuclear_repulsion_energy =
      Process::environment.molecule()->nuclear_repulsion_energy({0, 0, 0});

    //std::vector<SharedMatrix> C = C_->coefficients_blocks();

    int nirrep = C.size();

    std::vector<int> b_r(1);
    std::vector<int> e_r(1);
    std::vector<int> b_c(1);
    std::vector<int> e_c(1);

    //find total number of parameters
    int Npar = 0;
    for( auto C_h: C){
      Npar += (C_h->rowdim()) * (C_h->coldim());
    }



    for (int h=0; h<nirrep; h++) {
      // loop over irreps
      int ncol = C[h]->rowdim();
      int nrow = C[h]->coldim();

      int nt_cols = ncol/dim;
      int nt_rows = nrow/dim;

      int last_col_dim = ncol%dim;
      int last_row_dim = nrow%dim;

      //std::cout << "nt_cols:  "  << nt_cols <<std::endl;

      for(int i=0; i<nt_rows+1; i++){
        for(int j=0; j<nt_cols+1; j++){
          // make dimension objects
          //std::cout << "I get here i: " << i << "  j:  " << j <<std::endl;
          if(j == nt_cols && i == nt_rows){
            // make dimension objects for case of very last tile
            zero_tile(C, b_r, e_r, b_c, e_c, tile_norm_cut, dim, last_row_dim, last_col_dim, h, i, j, Npar);

          } else if(j == nt_cols){
            //std::cout << "I get here i: " << i << "  j:  " << j <<std::endl;
            // case of last tile colum
            zero_tile(C, b_r, e_r, b_c, e_c, tile_norm_cut, dim, dim, last_col_dim, h, i, j, Npar);


          } else if(i == nt_rows){
            // case of last tile row
            zero_tile(C, b_r, e_r, b_c, e_c, tile_norm_cut, dim, last_row_dim, dim, h, i, j, Npar);

          } else {
            // most cases (main block of tiles of dim x dim)
            zero_tile(C, b_r, e_r, b_c, e_c, tile_norm_cut, dim, dim, dim, h, i, j, Npar);
            //std::cout << "I get here i: " << i << "  j:  " << j <<std::endl;
            }
        //outfile->Printf("\nh dim: %6d  i dim: %6d  j dim: %6d  ",rank_tile_inirrep.size(), rank_tile_inirrep[h].size(), rank_tile_inirrep[h][i].size());
        }
      }
    }

    //re-nomralize Wfn
    double norm_C_chopped = 0.0;
    for (auto C_h : C) {
        norm_C_chopped += C_h->sum_of_squares();
    }
    norm_C_chopped = std::sqrt(norm_C_chopped);
    for (auto C_h : C) {
        C_h->scale(1. / norm_C_chopped);
    }

    double Norm = 0.0;
    for(auto C_h: C){
      Norm += C_h->sum_of_squares();
    }

    //print Block Sparse C
    py_mat_print(C[0], "C_block_chopped.dat");

    //compute energy

    // Set HC = H C
    C_->Hamiltonian(HC, fci_ints, twoSubstituitionVVOO);
    // Calculate E = C^T HC
    double E_block_chop = HC.dot(C_) + nuclear_repulsion_energy;

    int Nsto = Npar;

    outfile->Printf("\n////////////////// Tile Chopper /////////////////");
    outfile->Printf("\n");
    outfile->Printf("\n Tile Norm Cut     = %20.12f", tile_norm_cut);
    outfile->Printf("\n Norm              = %20.12f", Norm);
    outfile->Printf("\n E_fci             = %20.12f", fci_energy);
    outfile->Printf("\n E_block_chop      = %20.12f", E_block_chop);
    outfile->Printf("\n Delta(E_blk_chp)  = %20.12f", E_block_chop-fci_energy);
    outfile->Printf("\n Npar blk_chp      =     %6d", Npar);
    outfile->Printf("\n Nsto blk_chp      =     %6d", Nsto);
    outfile->Printf("\n");
}

void FCISolver::string_trimmer(std::vector<SharedMatrix>& C, double sum_cut, FCIVector& HC, std::shared_ptr<FCIIntegrals> fci_ints, double fci_energy)
{
  double nuclear_repulsion_energy =
      Process::environment.molecule()->nuclear_repulsion_energy({0, 0, 0});

  double Om_a;
  double Om_b;

  int N_par = 0;
  std::vector<int> Ia_bool(252, 1);
  std::vector<int> Ib_bool(252, 1);

  // for(auto C_h: C){
  //   N_par += C_h->rowdim() * C_h->coldim();
  // }

  for(auto C_h: C){
    // for alpha strings
    for(int i=0; i<C_h->coldim(); i++){
      Om_a = 0;
      for(int j=0; j<C_h->rowdim(); j++){
        Om_a += std::pow(C_h->get(i,j), 2);
      }
      if(Om_a < sum_cut){
      Ia_bool[i]--;
        // for(int j=0; j<C_h->rowdim(); j++){
        //   //C_h->set(i,j,0);
        //   N_par--;
        // }
      }
    }

    // for beta strings
    for(int j=0; j<C_h->rowdim(); j++){
      Om_b = 0;
      for(int i=0; i<C_h->coldim(); i++){
        Om_b += std::pow(C_h->get(i,j), 2);
      }
      if(Om_b < sum_cut){
        Ib_bool[j]--;
        // for(int i=0; i<C_h->coldim(); i++){
        //   //C_h->set(i,j,0);
        //   N_par--;
        }
      }


    //set blocks
    for(int i=0; i<C_h->coldim(); i++){
      for(int j=0; j<C_h->rowdim(); j++){
        if(Ia_bool[i]*Ib_bool[j] > 0) N_par++;
        C_h->set(i ,j , Ia_bool[i]*Ib_bool[j]*C_h->get(i,j));
      }
    }
  }
  // Re-Normalize Wvfn

  double norm_C_trimmed = 0.0;
  for (auto C_h : C) {
      norm_C_trimmed += C_h->sum_of_squares();
  }
  norm_C_trimmed = std::sqrt(norm_C_trimmed);
  for (auto C_h : C) {
      C_h->scale(1. / norm_C_trimmed);
  }

  double Norm = 0.0;
  for(auto C_h: C){
    Norm += C_h->sum_of_squares();
  }

  //Print MATRIX
  py_mat_print(C[0], "Cmat_trimmed.dat");

  //Compute energy

  // Set HC = H C
  C_->Hamiltonian(HC, fci_ints, twoSubstituitionVVOO);
  // Calculate E = C^T HC
  double E_string_trim = HC.dot(C_) + nuclear_repulsion_energy;

  int N_sto = N_par;

  outfile->Printf("\n////////////////// String Trimmer /////////////////");
  outfile->Printf("\n");
  outfile->Printf("\n Sum Cut           = %20.12f", sum_cut);
  outfile->Printf("\n Norm              = %20.12f", Norm);
  outfile->Printf("\n E_fci             = %20.12f", fci_energy);
  outfile->Printf("\n E_red_rank        = %20.12f", E_string_trim);
  outfile->Printf("\n Delta(E_red_rank) = %20.12f", E_string_trim-fci_energy);
  outfile->Printf("\n Npar              =     %6d", N_par);
  outfile->Printf("\n Nsto              =     %6d", N_sto);
  outfile->Printf("\n");
}

void FCISolver::string_stats(std::vector<SharedMatrix> C)
{
  int n_a = 0;
  int n_b = 0;

  //may need to apend loop bounds to account for multiple irreps if using symmetry
  for(auto C_h: C){
    n_a += C_h->rowdim();
    n_b += C_h->coldim();
  }


  double Om_a;
  double Om_b;
  std::vector<double> C_Ia_mag(n_a, 0);
  std::vector<double> C_Ib_mag(n_b, 0);
  std::vector<int> C_Ia_mag_histo(11, 0);
  std::vector<int> C_Ib_mag_histo(11, 0);
  std::vector<int> C_Ia_nuses(n_a, 0); //range 0 to n_b
  std::vector<int> C_Ib_nuses(n_b, 0); //range 0 to n_a

  for(auto C_h: C){
    // alpha string statistics
    for(int i=0; i<C_h->coldim(); i++){
      Om_a = 0;
      for(int j=0; j<C_h->rowdim(); j++){
        //std::cout << std::abs(C_h->get(i,j)) << std::endl;
        Om_a += std::pow(C_h->get(i,j), 2);
        //std::cout << "Om_a: " << Om_a << std::endl;
        C_Ia_mag[i] += std::pow(C_h->get(i,j), 2);
        if(std::pow(C_h->get(i,j),2) > 1e-12){
          C_Ia_nuses[i]++;
        }
      }
      //std::cout << "  Om_a:  " << Om_a << std::endl;
      Om_a = -std::log10(Om_a);
      //std::cout << "i: " << i << "  Om_a:  " << Om_a << std::endl;
      if(Om_a >= 10){
        C_Ia_mag_histo[10]++;
      } else; C_Ia_mag_histo[std::floor(Om_a)]++;
    }

    // now for beta strings (should be the same for symmetric Cmat)
    for(int i=0; i<C_h->rowdim(); i++){
      Om_b = 0;
      for(int j=0; j<C_h->coldim(); j++){
        C_Ib_mag[i] += std::pow(C_h->get(j,i), 2);
        Om_b += std::pow(C_h->get(j,i), 2);
        if(std::pow(C_h->get(j,i),2) > 1e-12){
          C_Ib_nuses[i]++;
        }
      }
      Om_b = -std::log10(Om_b);
      if(Om_b >= 10){
        C_Ib_mag_histo[10]++;
      } else C_Ib_mag_histo[std::floor(Om_b)]++;
    }
  }

  std::sort(C_Ia_mag.rbegin(), C_Ia_mag.rend());
  std::sort(C_Ib_mag.rbegin(), C_Ib_mag.rend());

  std::ofstream myfile1;
  std::ofstream myfile2;

  myfile1.open ("Ia.dat");
  for(int j=0; j<C_Ia_mag.size(); j++){ myfile1 << " " << C_Ia_mag[j]; }
  myfile1 << "\n";
  for(int j=0; j<C_Ia_mag.size(); j++){ myfile1 << " " << C_Ia_nuses[j]; }
  myfile1.close();

  myfile2.open ("Ib.dat");
  for(int j=0; j<C_Ib_mag.size(); j++){ myfile2 << " " << C_Ib_mag[j]; }
  myfile2 << "\n";
  for(int j=0; j<C_Ib_mag.size(); j++){ myfile2 << " " << C_Ib_nuses[j]; }
  myfile2.close();

  outfile->Printf("\n alpha Strings in bin:");
  for(auto nC_Ia: C_Ia_mag_histo){
    outfile->Printf(" %1d,", nC_Ia);
  }

  outfile->Printf("\n beta  Strings in bin:");
  for(auto nC_Ib: C_Ib_mag_histo){
    outfile->Printf(" %1d,", nC_Ib);
  }

}

void FCISolver::zero_tile(std::vector<SharedMatrix>& C,
                                 std::vector<int> b_r,
                                 std::vector<int> e_r,
                                 std::vector<int> b_c,
                                 std::vector<int> e_c,
                                 double tile_norm_cut,
                                 int dim, int n, int d,
                                 int h, int i, int j, int& Npar )
{
  //prepare dimension objects
  b_r[0] = i*dim;
  e_r[0] = i*dim + n;
  b_c[0] = j*dim;
  e_c[0] = j*dim + d;

  Dimension begin_row(b_r);
  Dimension end_row(e_r);
  Dimension begin_col(b_c);
  Dimension end_col(e_c);


  // make slice objects
  Slice row_slice(begin_row, end_row);
  Slice col_slice(begin_col, end_col);

  // get matrix block
  auto M = C[h]->get_block(row_slice, col_slice);

  double area_factor = (n*d) / (dim*dim);
  double tile_factor = (M->sum_of_squares()) / area_factor;

  if(tile_factor < tile_norm_cut){
    M->set(0.0);
    C[h]->set_block(row_slice, col_slice, M);
    Npar -= n*d;
  }

}

void FCISolver::add_to_sig_vect(std::vector<std::tuple<double, int, int, int> >& sorted_sigma,
                                 std::vector<SharedMatrix> C,
                                 std::vector<int> b_r,
                                 std::vector<int> e_r,
                                 std::vector<int> b_c,
                                 std::vector<int> e_c,
                                 int dim, int n, int d,
                                 int h, int i, int j)
{
  //prepare dimension objects
  b_r[0] = i*dim;
  e_r[0] = i*dim + n;
  b_c[0] = j*dim;
  e_c[0] = j*dim + d;

  Dimension begin_row(b_r);
  Dimension end_row(e_r);
  Dimension begin_col(b_c);
  Dimension end_col(e_c);

  // make slice objects
  Slice row_slice(begin_row, end_row);
  Slice col_slice(begin_col, end_col);

  // get matrix block
  auto M = C[h]->get_block(row_slice, col_slice);

  auto u = std::make_shared<Matrix>("u", n, n);
  auto s = std::make_shared<Vector>("s", std::min(n, d));
  auto v = std::make_shared<Matrix>("v", d, d);

  M->svd(u, s, v);
  //s->print();

  // add to sigma vector
  for (int k = 0; k < std::min(n, d); k++) {
      sorted_sigma.push_back(std::make_tuple(s->get(k), h, i, j));
  }
  //outfile->Printf("\n size of sigma vect: %4d h: %4d i: %4d j: %4d",sorted_sigma.size(),h,i,j);
}


void FCISolver::patch_Cmat(std::vector<std::tuple<double, int, int, int> >& sorted_sigma,
                                 std::vector<SharedMatrix>& C,
                                 std::vector<std::vector<std::vector<int> > > rank_tile_inirrep,
                                 std::vector<int> b_r,
                                 std::vector<int> e_r,
                                 std::vector<int> b_c,
                                 std::vector<int> e_c,
                                 int dim, int n, int d,
                                 int h, int i, int j,
                                 int& N_par)
{
  //prepare dimension objects
  b_r[0] = i*dim;
  e_r[0] = i*dim + n;
  b_c[0] = j*dim;
  e_c[0] = j*dim + d;

  Dimension begin_row(b_r);
  Dimension end_row(e_r);
  Dimension begin_col(b_c);
  Dimension end_col(e_c);

  // make slice objects
  Slice row_slice(begin_row, end_row);
  Slice col_slice(begin_col, end_col);

  // get matrix block
  auto M = C[h]->get_block(row_slice, col_slice);

  auto u = std::make_shared<Matrix>("u", n, n);
  auto s = std::make_shared<Vector>("s", std::min(n, d));
  auto v = std::make_shared<Matrix>("v", d, d);

  M->svd(u, s, v);
  //s->print();

  //rebuild sigma as matrix
  auto sig_mat = std::make_shared<Matrix>("sig_mat", n, d);
  int rank_ef = rank_tile_inirrep[h][i][j];
  for (int l = 0; l < rank_ef; l++) {
      sig_mat->set(l, l, s->get(l));
  }

  //count paramaters
  N_par += rank_ef*(n + d);

  //rebuild M with reduced rank
  auto M_red_rank = Matrix::triplet(u, sig_mat, v, false, false, false);

  //splice M_red_rank back into C
  C[h]->set_block(row_slice, col_slice, M_red_rank);
}


void FCISolver::fci_svd_tiles(FCIVector& HC, std::shared_ptr<FCIIntegrals> fci_ints, double fci_energy, int dim, double OMEGA)
{
  double nuclear_repulsion_energy =
      Process::environment.molecule()->nuclear_repulsion_energy({0, 0, 0});

    std::vector<SharedMatrix> C = C_->coefficients_blocks();
    std::vector<SharedMatrix> C_tiled_rr = C_->coefficients_blocks();

    int nirrep = C.size();
    int total_rank = 0;
    int total_red_rank = 0;

    ///// PRE MATRIX TILER BEGIN /////

    std::vector<std::tuple<double, int, int, int> > sorted_sigma;
    int size_of_ssv = 0;
    std::vector<int> b_r(1);
    std::vector<int> e_r(1);
    std::vector<int> b_c(1);
    std::vector<int> e_c(1);

    std::vector<std::vector<std::vector<int> > > rank_tile_inirrep;
    rank_tile_inirrep.resize(nirrep);

    // now need to put singular values into said vector ...

    for (int h=0; h<nirrep; h++) {
        // loop over irreps
        int ncol = C[h]->rowdim();
        int nrow = C[h]->coldim();

        int nt_cols = ncol/dim;
        int nt_rows = nrow/dim;

        int last_col_dim = ncol%dim;
        int last_row_dim = nrow%dim;

        int n_sing_vals_h = dim*nt_cols*nt_rows
                             + last_col_dim*nt_rows
                             + last_row_dim*nt_cols
                             + std::min(last_col_dim, last_row_dim);

        size_of_ssv += n_sing_vals_h;
        rank_tile_inirrep[h].resize(nt_rows+1);

        //outfile->Printf("\n-------------------- NEXT IRREP --------------------\n");
        //C[h]->print();

        for(int i=0; i<nt_rows+1; i++){
          // allocate memory for sorting vector
          rank_tile_inirrep[h][i].resize(nt_cols+1);
          for(int j=0; j<nt_cols+1; j++){
            // make dimension objects
            if(j == nt_cols && i == nt_rows){
              // case of very last tile
              add_to_sig_vect(sorted_sigma, C, b_r, e_r, b_c, e_c, dim, last_row_dim, last_col_dim, h, i, j);

            } else if(j == nt_cols){
              // case of last tile colum
              add_to_sig_vect(sorted_sigma, C, b_r, e_r, b_c, e_c, dim, dim, last_col_dim, h, i, j);

            } else if(i == nt_rows){
              // case of last tile row
              add_to_sig_vect(sorted_sigma, C, b_r, e_r, b_c, e_c, dim, last_row_dim, dim, h, i, j);

            } else {
              // most cases (main block of tiles of dim x dim)
              add_to_sig_vect(sorted_sigma, C, b_r, e_r, b_c, e_c, dim, dim, dim, h, i, j);
          }
          //outfile->Printf("\nh dim: %6d  i dim: %6d  j dim: %6d  ",rank_tile_inirrep.size(), rank_tile_inirrep[h].size(), rank_tile_inirrep[h][i].size());
          }
        }
    }

    /////////////////////////////////// check status ////////////////////////////////////

    std::sort(sorted_sigma.rbegin(), sorted_sigma.rend());
    double sigma_norm = 0.0;

    outfile->Printf("\n");
    //outfile->Printf("\n        singular value    irrep      tile(i)     tile(j)");
    //outfile->Printf("\n------------------------------------------------------------");

    for (auto sigma_h : sorted_sigma) {
        //outfile->Printf("\n   %20.12f      %d      %d      %d", std::get<0>(sigma_h), std::get<1>(sigma_h), std::get<2>(sigma_h), std::get<3>(sigma_h));
        sigma_norm += std::pow(std::get<0>(sigma_h), 2.0);
    }

    outfile->Printf("\n");
    outfile->Printf("\n size of sorted sigma vec: %6d ", size_of_ssv);
    outfile->Printf("\n size of count: %6d ", sorted_sigma.size());
    outfile->Printf("\n Norm of sigma: %20.12f ", sigma_norm);

    // find cutoff OMEGA
    double norm_cut = 1.0 - OMEGA;
    double sig_sum = 0;
    double omega_norm;

    for (auto sigma_h : sorted_sigma) {
        rank_tile_inirrep[std::get<1>(sigma_h)][std::get<2>(sigma_h)][std::get<3>(sigma_h)] += 1;
        sig_sum += std::pow(std::get<0>(sigma_h), 2.0);
        if (sig_sum > norm_cut * sigma_norm) {
            break;
        }
    }

    // outfile->Printf("\n  irrep     tile(i)     tile(j)     red rank");
    // outfile->Printf("\n------------------------------------------------------------");
    //
    // outfile->Printf("\n");
    // for(int h = 0; h<nirrep; h++){
    //   for(int i = 0; i<rank_tile_inirrep[h].size(); i++){
    //     for(int j = 0; j<rank_tile_inirrep[h][i].size(); j++){
    //       outfile->Printf("\n  %6d     %6d     %6d     %6d", h,i,j,rank_tile_inirrep[h][i][j]);
    //     }
    //   }
    // }

    ///// PRE MATRIX TILER END /////

    //// Count how many tiles are of each rank in dimension
    std::vector<int> tile_rank_histo(dim+1,0);
    //std::cout << "\n test:  " << tile_rank_histo[5] << "\n";

    for(int h = 0; h<nirrep; h++){
      for(int i = 0; i<rank_tile_inirrep[h].size(); i++){
        for(int j = 0; j<rank_tile_inirrep[h][i].size(); j++){
          tile_rank_histo[rank_tile_inirrep[h][i][j]]++;
        }
      }
    }

    outfile->Printf("\n \n Tile rank histo: ");
    for(auto quantity_of_rank : tile_rank_histo){
      outfile->Printf("%d, ", quantity_of_rank);
    }


    ///// MAIN MATRIX TILER BEGIN /////

    int N_par = 0;

    outfile->Printf("\n");
    outfile->Printf("\n///////////////////////////// REBUILDING RED RANK TILES //////////////////////////////////////\n");
    outfile->Printf("\n");

    //now reduce rank accordingly!
    for (int h=0; h<nirrep; h++) {
        // loop over irreps
        int ncol = C_tiled_rr[h]->rowdim();
        int nrow = C_tiled_rr[h]->coldim();

        int nt_cols = ncol/dim;
        int nt_rows = nrow/dim;

        int last_col_dim = ncol%dim;
        int last_row_dim = nrow%dim;

        int n_sing_vals_h = dim*nt_cols*nt_rows
                             + last_col_dim*nt_rows
                             + last_row_dim*nt_cols
                             + std::min(last_col_dim, last_row_dim);

        size_of_ssv += n_sing_vals_h;
        rank_tile_inirrep[h].resize(nt_rows+1);

        //outfile->Printf("\n-------------------- NEXT IRREP --------------------\n");

        for(int i=0; i<nt_rows+1; i++){
          // allocate memory for sorting vector
          rank_tile_inirrep[h][i].resize(nt_cols+1);
          for(int j=0; j<nt_cols+1; j++){
            // make dimension objects
            if(j == nt_cols && i == nt_rows){
              // make dimension objects for case of very last tile
              patch_Cmat(sorted_sigma, C_tiled_rr, rank_tile_inirrep, b_r, e_r, b_c, e_c, dim, last_row_dim, last_col_dim, h, i, j, N_par);
            } else if(j == nt_cols){
              // make dimension objects for case of last tile colum
              patch_Cmat(sorted_sigma, C_tiled_rr, rank_tile_inirrep, b_r, e_r, b_c, e_c, dim, dim, last_col_dim, h, i, j, N_par);
            } else if(i == nt_rows){
              // make dimension objects for case of last tile row
              patch_Cmat(sorted_sigma, C_tiled_rr, rank_tile_inirrep, b_r, e_r, b_c, e_c, dim, last_row_dim, dim, h, i, j, N_par);
            } else {
              // make dimension objects
              patch_Cmat(sorted_sigma, C_tiled_rr, rank_tile_inirrep, b_r, e_r, b_c, e_c, dim, dim, dim, h, i, j, N_par);
              }
          //outfile->Printf("\nh dim: %6d  i dim: %6d  j dim: %6d  ",rank_tile_inirrep.size(), rank_tile_inirrep[h].size(), rank_tile_inirrep[h][i].size());
          } // end j
        } // end i
    } // end h

    //re-normalize

    double norm_C_tiled_rr = 0.0;
    for (auto C_tiled_rr_h : C_tiled_rr) {
        norm_C_tiled_rr += C_tiled_rr_h->sum_of_squares();
    }
    norm_C_tiled_rr = std::sqrt(norm_C_tiled_rr);
    for (auto C_tiled_rr_h : C_tiled_rr) {
        C_tiled_rr_h->scale(1. / norm_C_tiled_rr);
    }

/*
    outfile->Printf("\n irrep red        ||C - C_rr||\n");

    for(int h = 0; h<nirrep; h++){
      auto C_diff = C[h]->clone();
      C_diff->subtract(C_tiled_rr[h]);
      double norm = std::sqrt(C_diff->sum_of_squares());
      outfile->Printf(" %1d %20.12f \n", h, norm);
      //outfile->Printf("\n //// C ////\n");
      //C[h]->print();
      //outfile->Printf("\n //// C_tiled_rr ////\n");
      //C_tiled_rr[h]->print();
    }
*/
    // Compute the energy

    C_->set_coefficient_blocks(C_tiled_rr);
    // HC = H C
    C_->Hamiltonian(HC, fci_ints, twoSubstituitionVVOO);
    // E = C^T HC
    double E_red_rank = HC.dot(C_) + nuclear_repulsion_energy;

    outfile->Printf("\n OMEGA             = %20.12f", OMEGA);
    outfile->Printf("\n tile size         =     %6d", dim);
    outfile->Printf("\n E_fci             = %20.12f", fci_energy);
    outfile->Printf("\n E_red_rank        = %20.12f", E_red_rank);
    outfile->Printf("\n Delta(E_red_rank) = %20.12f", E_red_rank-fci_energy);
    outfile->Printf("\n Npar              =     %6d", N_par/2);
    outfile->Printf("\n Nsto              =     %6d", N_par/2);

    ///// MAIN MATRIX TILER END /////

}




void FCISolver::fci_svd(FCIVector& HC, std::shared_ptr<FCIIntegrals> fci_ints, double fci_energy, double TAU)
{
  double nuclear_repulsion_energy =
      Process::environment.molecule()->nuclear_repulsion_energy({0, 0, 0});

    std::vector<SharedMatrix> C = C_->coefficients_blocks();
    std::vector<SharedMatrix> C_red_rank;

    //outfile->Printf("\n");
    //outfile->Printf("\n PRINTING CLEAN C MATRICIES\n");

    double norm_C = 0.0;
    for (auto C_h : C) {
        //C_h->print();
        norm_C += C_h->sum_of_squares();
    }

    int nirrep = C.size();
    int total_rank = 0;
    int total_red_rank = 0;

    std::vector<std::pair<double, int>> sorted_sigma; // [(sigma_i, irrep)]

    for (int h = 0; h < nirrep; h++) {
        int nrow = C[h]->rowdim();
        int ncol = C[h]->coldim();

        auto u_p = std::make_shared<Matrix>("u_p", nrow, nrow);
        auto s_p = std::make_shared<Vector>("s_p", std::min(ncol, nrow));
        auto v_p = std::make_shared<Matrix>("v_p", ncol, ncol);

        C[h]->svd(u_p, s_p, v_p);

        std::string mat_file_U = "U_mat_ft_irrep_" + std::to_string(h);
        std::string mat_file_V = "V_mat_ft_irrep_" + std::to_string(h);
        py_mat_print(u_p, mat_file_U);
        py_mat_print(v_p, mat_file_V);

        for (int i = 0; i < std::min(ncol, nrow); i++) {
            sorted_sigma.push_back(std::make_pair(s_p->get(i), h));
        }
    }

    std::sort(sorted_sigma.rbegin(), sorted_sigma.rend());
    double sigma_norm = 0.0;
    int k = 0;


    //outfile->Printf("\n    singular value      irrep");
    //outfile->Printf("\n--------------------------------");

    for (auto sigma_h : sorted_sigma) {
        //outfile->Printf("\n   %20.12f      %d", sigma_h.first, sigma_h.second);
        sigma_norm += std::pow(sigma_h.first, 2.0);
    }

    outfile->Printf("\n");
    outfile->Printf("\n Norm of sigma: %20.12f \n", sigma_norm);

    double norm_cut = 1.0 - TAU;
    double sig_sum = 0;
    double tao_norm;
    std::vector<int> rank_irrep(nirrep, 0);
    for (auto sigma_h : sorted_sigma) {
        rank_irrep[sigma_h.second] += 1;
        sig_sum += std::pow(sigma_h.first, 2.0);
        if (sig_sum > norm_cut * sigma_norm) {
            break;
        }
    }

    outfile->Printf(" Rank for each irrep:");
    for (int r : rank_irrep) {
        outfile->Printf(" %d", r);
    }
    outfile->Printf("\n");

    double tao = options_.get_double("FCI_SVD_TAU");

    outfile->Printf("\n ||C|| = %20.12f", std::sqrt(norm_C));

    outfile->Printf("\n irrep red  full        ||C - C_rr||\n");

    int N_par = 0;

    for (int h = 0; h < nirrep; h++) {
        int nrow = C[h]->rowdim();
        int ncol = C[h]->coldim();

        auto u_p = std::make_shared<Matrix>("u_p", nrow, nrow);
        auto s_p = std::make_shared<Vector>("s_p", std::min(ncol, nrow));
        auto v_p = std::make_shared<Matrix>("v_p", ncol, ncol);

        C[h]->svd(u_p, s_p, v_p);

        int rank_ef = std::min(ncol, nrow);
        if (options_.get_str("FCI_SVD_TYPE") == "THRESHOLD") {
            for (int i = 0; i < std::min(ncol, nrow); i++) {
                if (s_p->get(i) < tao) {
                    rank_ef = i;
                    break;
                }
            }
        } else {
            rank_ef = rank_irrep[h];
        }

        total_rank += std::min(ncol, nrow);
        total_red_rank += rank_ef;

        // Copy diagonal of s_p to a matrix
        auto sig_mat = std::make_shared<Matrix>("sig_mat", nrow, ncol);
        for (int i = 0; i < rank_ef; i++) {
            sig_mat->set(i, i, s_p->get(i));
        }

        auto C_red_rank_h = Matrix::triplet(u_p, sig_mat, v_p, false, false, false);
        C_red_rank.push_back(C_red_rank_h);

        auto C_diff = C[h]->clone();
        C_diff->subtract(C_red_rank_h);
        double norm = std::sqrt(C_diff->sum_of_squares());

        N_par += rank_ef*std::min(ncol, nrow);

        outfile->Printf(" %1d %6d %6d %20.12f \n", h, rank_ef, std::min(ncol, nrow), norm);
    }
    outfile->Printf("   %6d %6d\n", total_red_rank, total_rank);

    int N_sto = N_par;

    // re-normalize C_red_rank
    double norm_C_red_rank = 0.0;
    for (auto C_red_rank_h : C_red_rank) {
        norm_C_red_rank += C_red_rank_h->sum_of_squares();
    }
    norm_C_red_rank = std::sqrt(norm_C_red_rank);
    for (auto C_red_rank_h : C_red_rank) {
        C_red_rank_h->scale(1. / norm_C_red_rank);
    }

    outfile->Printf("\n");
    outfile->Printf("\n PRINTING RR C MATRICIES\n");

    {
        double norm_C_red_rank = 0.0;
        for (auto C_red_rank_h : C_red_rank) {
            norm_C_red_rank += C_red_rank_h->sum_of_squares();
            //C_red_rank_h->print();
        }
        norm_C_red_rank = std::sqrt(norm_C_red_rank);
        outfile->Printf("\n ||C|| = %20.12f", std::sqrt(norm_C_red_rank));
    }

    // Compute the energy

    C_->set_coefficient_blocks(C_red_rank);
    // HC = H C
    C_->Hamiltonian(HC, fci_ints, twoSubstituitionVVOO);
    // E = C^T HC
    double E_red_rank = HC.dot(C_) + nuclear_repulsion_energy;

    outfile->Printf("\n Tau               = %20.12f", TAU);
    outfile->Printf("\n E_fci             = %20.12f", fci_energy);
    outfile->Printf("\n E_red_rank        = %20.12f", E_red_rank);
    outfile->Printf("\n Delta(E_red_rank) = %20.12f", E_red_rank-fci_energy);
    outfile->Printf("\n Npar              =     %6d", N_par);
    outfile->Printf("\n Nsto              =     %6d", N_sto);

}

} // namespace forte
} // namespace psi
