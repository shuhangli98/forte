#include <libplugin/plugin.h>
#include "psi4-dec.h"
#include <libdpd/dpd.h>
#include "psifiles.h"
#include <libpsio/psio.hpp>
#include <libtrans/integraltransform.h>
#include <libmints/wavefunction.h>
#include <cmath>

#include "explorer.h"

// This allows us to be lazy in getting the spaces in DPD calls
#define ID(x) ints.DPD_ID(x)

INIT_PLUGIN

namespace psi{ namespace libadaptive{

extern "C" int
read_options(std::string name, Options &options)
{
    if (name == "LIBADAPTIVE" || options.read_globals()) {
        /*- The amount of information printed
            to the output file -*/
        options.add_int("PRINT", 1);

        // Options for the Explorer class
        /*- The symmetry of the electronic state.  If a value is provided
            it overrides the multiplicity of the SCF solution. -*/
        options.add_int("SYMMETRY",0);
        /*- The multiplicity of the electronic state.  If a value is provided
            it overrides the multiplicity of the SCF solution. -*/
        options.add_int("MULTIPLICITY",0);
        /*- The charge of the molecule.  If a value is provided
            it overrides the multiplicity of the SCF solution. -*/
        options.add_int("CHARGE",0);
        /*- The minimum excitation level (Default value: 0) -*/
        options.add_int("MIN_EXC_LEVEL",0);
        /*- The maximum excitation level (Default value: 0 = number of electrons) -*/
        options.add_int("MAX_EXC_LEVEL",0);
        /*- The frozen doubly occupied orbitals -*/
        options.add("FROZEN_DOCC",new ArrayType());
        /*- The frozen doubly occupied orbitals -*/
        options.add("FROZEN_UOCC",new ArrayType());
        /*- The energy threshold for the determinant energy in Hartree -*/
        options.add_double("DET_THRESHOLD",1.0);
        /*- The energy threshold for the MP denominators energy in Hartree -*/
        options.add_double("DEN_THRESHOLD",1.5);
        /*- The criteria used to screen the strings -*/
        options.add_str("SCREENING_TYPE","MP","MP DET");
        /*- The form of the Hamiltonian matrix.
         *  - FIXED diagonalizes a matrix of fixed dimension
         *  - SMOOTH forms a matrix with smoothed matrix elements -*/
        options.add_str("H_TYPE","FIXED_SIZE","FIXED_ENERGY FIXED_SIZE");
        /*- The number of determinants used to build the Hamiltonian -*/        
        options.add_int("NDETS",100);
        /*- The maximum dimension of the Hamiltonian -*/
        options.add_int("MAX_NDETS",15000);
        /*- The number of determinants used to build the Hamiltonian -*/
        options.add_double("SMO_THRESHOLD",0.0);
        /*- The method used to smooth the Hamiltonian -*/
        options.add_str("SMOOTHING_FUNCTION","I","I");
        /*- The diagonalization method -*/
        options.add_str("DIAG_ALGORITHM","DAVIDSON","DAVIDSON FULL");
        /*- The number of roots computed -*/
        options.add_int("NROOT",4);

        // Options for the Cartographer class //
        /*- Density of determinants format -*/
        options.add_str("DOD_FORMAT","HISTOGRAM","GAUSSIAN HISTOGRAM");
        /*- Number of bins used to form the DOD plot -*/
        options.add_int("DOD_BINS",2000);
        /*- Width of the DOD Gaussian/histogram.  Default 0.02 Hartree ~ 0.5 eV -*/
        options.add_double("DOD_BIN_WIDTH",0.05);
        /*- Write an output file? -*/
        options.add_bool("DETTOUR_WRITE_FILE",false);
        /*- Write the determinant occupation? -*/
        options.add_bool("WRITE_OCCUPATION",true);
        /*- Write the determinant energy? -*/
        options.add_bool("WRITE_DET_ENERGY",true);
        /*- Write the denominator energy? -*/
        options.add_bool("WRITE_DEN_ENERGY",false);
        /*- Write the excitation level? -*/
        options.add_bool("WRITE_EXC_LEVEL",false);
        /*- Write information only for a given excitation level.
            0 (default) means print all -*/
        options.add_int("RESTRICT_EXCITATION",0);
        /*- The energy buffer for building the Hamiltonian matrix in Hartree -*/
        options.add_double("H_BUFFER",0.0);
    }
    return true;
}


extern "C" PsiReturnType
libadaptive(Options &options)
{
    // Get the one- and two-electron integrals in the MO basis
    ExplorerIntegrals* ints_ = new ExplorerIntegrals(options);

    // The explorer object will do its job
    Explorer* explorer = new Explorer(options,ints_);
    delete explorer;

    // Delete ints_;
    delete ints_;

    return Success;
}

}} // End Namespaces
