#
# @BEGIN LICENSE
#
# forte_inversion by Psi4 Developer, a plugin to:
#
# Psi4: an open-source quantum chemistry software package
#
# Copyright (c) 2007-2016 The Psi4 Developers.
#
# The copyrights for code used from other parties are included in
# the corresponding files.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# @END LICENSE
#

import time
import os

import psi4
import psi4.driver.p4util as p4util

import forte
from forte.data import ForteData
from forte.modules import (
    OptionsFactory,
    ObjectsFromFCIDUMP,
    ObjectsFromPsi4,
    ActiveSpaceInts,
    ActiveSpaceSolver,
    ActiveSpaceRDMs,
    OrbitalTransformation,
    MCSCF,
    TDACI,
)
from forte.proc.external_active_space_solver import (
    write_external_active_space_file,
    write_external_rdm_file,
    write_wavefunction,
    read_wavefunction,
    make_hamiltonian,
)
from forte.proc.dsrg import ProcedureDSRG


def forte_driver(data: ForteData):
    """
    Driver to perform a Forte calculation using new solvers.

    Parameters
    ----------
    data: ForteData
        A ForteData object initialized with the following attributes:
        state_weights_map: dictionary of {state: weights}
        scf_info: a SCFInfo object of Forte
        options: a ForteOptions object of Forte
        ints: a ForteIntegrals object of Forte
        mo_space_info: a MOSpaceInfo object of Forte
    Returns
    -------
    return_en: float
        The computed energy
    """

    state_weights_map, scf_info, options, ints, mo_space_info = (
        data.state_weights_map,
        data.scf_info,
        data.options,
        data.ints,
        data.mo_space_info,
    )

    # map state to number of roots
    state_map = forte.to_state_nroots_map(state_weights_map)

    # create an active space solver object and compute the energy
    data = ActiveSpaceInts(active="ACTIVE", core=["RESTRICTED_DOCC"]).run(data)
    as_ints = data.as_ints

    active_space_solver_type = options.get_str("ACTIVE_SPACE_SOLVER")
    data = ActiveSpaceSolver(solver_type=active_space_solver_type).run(data)
    state_energies_list = data.state_energies_list

    if options.get_bool("WRITE_RDM"):
        max_rdm_level = 3  # TODO allow the user to change this variable
        data = ActiveSpaceRDMs(max_rdm_level=max_rdm_level).run(data)
        write_external_rdm_file(data.rdms)

    if options.get_bool("SPIN_ANALYSIS"):
        data = ActiveSpaceRDMs(max_rdm_level=2, rdms_type=forte.RDMsType.spin_dependent).run(data)
        forte.perform_spin_analysis(data.rdms, options, mo_space_info, as_ints)

    # solver for dynamical correlation from DSRG
    correlation_solver_type = options.get_str("CORRELATION_SOLVER")
    if correlation_solver_type != "NONE":
        dsrg_proc = ProcedureDSRG(data.active_space_solver, state_weights_map, mo_space_info, ints, options, scf_info)
        return_en = dsrg_proc.compute_energy()
        dsrg_proc.print_summary()
        dsrg_proc.push_to_psi4_environment()

        if options.get_str("DERTYPE") == "FIRST" and active_space_solver_type in ["DETCI", "GENCI"]:
            # Compute coupling coefficients
            # NOTE: 1. Orbitals have to be semicanonicalized already to make sure
            #          DSRG reads consistent CI coefficients before and after SemiCanonical class.
            #       2. This is OK only when running ground-state calculations
            state = list(state_map.keys())[0]
            psi4.core.print_out(f"\n  ==> Coupling Coefficients for {state} <==")
            ci_vectors = data.active_space_solver.eigenvectors(state)
            dsrg_proc.compute_gradient(ci_vectors)
        else:
            psi4.core.print_out("\n  Semicanonical orbitals must be used!\n")
    else:
        average_energy = forte.compute_average_state_energy(state_energies_list, state_weights_map)
        return_en = average_energy

    return return_en


def energy_forte(name, **kwargs):
    """
    This function is called when the user calls energy('forte').
    It sets up the computation and calls the Forte driver.

    Parameters
    ----------
    name: str
        The name of the module (forte)
    kwargs: dict
        The kwargs dictionary
    """
    # # Start Forte, initialize ambit
    # my_proc_n_nodes = forte.startup()
    # my_proc, n_nodes = my_proc_n_nodes

    # Start timer
    start_pre_ints = time.time()

    # Print the banner
    forte.banner()

    # Build Forte options
    data = OptionsFactory(options=kwargs.get("forte_options")).run()

    job_type = data.options.get_str("JOB_TYPE")
    # Prepare Forte objects
    if "FCIDUMP" in data.options.get_str("INT_TYPE"):
        data = ObjectsFromFCIDUMP(options=kwargs).run(data)
    else:
        data = ObjectsFromPsi4(**kwargs).run(data)

    start = time.time()

    # Rotate orbitals before computation (e.g. localization, MP2 natural orbitals, etc.)
    orb_type = data.options.get_str("ORBITAL_TYPE")
    if orb_type != "CANONICAL":
        OrbitalTransformation(orb_type, job_type != "NONE").run(data)

    energy = 0.0

    # Run a method
    if job_type == "NONE":
        psi4.core.set_scalar_variable("CURRENT ENERGY", energy)
        return data.psi_wfn

    if job_type == "CASSCF":
        # raise Exception("Forte: CASSCF_REFERENCE is not supported")
        if data.options.get_str("INT_TYPE") == "FCIDUMP":
            raise Exception("Forte: the CASSCF code cannot use integrals read from a FCIDUMP file")

        casscf = forte.make_casscf(data.state_weights_map, data.scf_info, data.options, data.mo_space_info, data.ints)
        energy = casscf.compute_energy()

    if data.options.get_bool("CASSCF_REFERENCE") or job_type == "MCSCF_TWO_STEP":
        data = MCSCF(data.options.get_str("ACTIVE_SPACE_SOLVER")).run(data)
        energy = data.results.value("energy")

    if job_type == "TDCI":
        data = TDACI().run(data)
        energy = data.results.value("energy")
        # state = forte.make_state_info_from_psi(data.options)
        # data = ActiveSpaceInts(active="ACTIVE", core=["RESTRICTED_DOCC"]).run(data)
        # state_map = forte.to_state_nroots_map(data.state_weights_map)
        # active_space_method = forte.make_active_space_method(
        #     "ACI", state, data.options.get_int("NROOT"), data.scf_info, data.mo_space_info, data.as_ints, data.options
        # )
        # active_space_method.set_quiet_mode()
        # active_space_method.compute_energy()

        # tdci = forte.TDCI(active_space_method, data.scf_info, data.options, data.mo_space_info, data.as_ints)
        # energy = tdci.compute_energy()

    if job_type == "NEWDRIVER":
        energy = forte_driver(data)
    elif job_type == "MR-DSRG-PT2":
        energy = mr_dsrg_pt2(job_type, data)

    end = time.time()

    # Close ambit, etc.
    # forte.cleanup()

    psi4.core.set_scalar_variable("CURRENT ENERGY", energy)

    psi4.core.print_out(f"\n\n  Time to prepare integrals: {start - start_pre_ints:12.3f} seconds")
    psi4.core.print_out(f"\n  Time to run job          : {end - start:12.3f} seconds")
    psi4.core.print_out(f"\n  Total                    : {end - start_pre_ints:12.3f} seconds\n")

    if "FCIDUMP" not in data.options.get_str("INT_TYPE"):
        if data.options.get_bool("DUMP_ORBITALS"):
            dump_orbitals(data.psi_wfn)
        return data.psi_wfn
    return None


def gradient_forte(name, **kwargs):
    """
    This funtion is called when the user calls gradient('forte').
    It sets up the computation and calls the Forte driver.
    This function is currently only implemented for CASSCF and MCSCF_TWO_STEP and DSRG-MRPT2.

    Parameters
    ----------
    name: str
        The name of the module (forte)
    kwargs: dict
        The kwargs dictionary
    """

    # Get the psi4 option object
    optstash = p4util.OptionsState(["GLOBALS", "DERTYPE"])
    psi4.core.set_global_option("DERTYPE", "FIRST")

    # Build Forte options
    data = OptionsFactory(options=kwargs.get("forte_options")).run()

    # Print the banner
    forte.banner()

    # Run a method
    job_type = data.options.get_str("JOB_TYPE")
    int_type = data.options.get_str("INT_TYPE")
    correlation_solver = data.options.get_str("CORRELATION_SOLVER")

    if job_type not in {"CASSCF", "MCSCF_TWO_STEP"} and correlation_solver != "DSRG-MRPT2":
        raise Exception("Analytic energy gradients are only implemented for" " CASSCF, MCSCF_TWO_STEP, or DSRG-MRPT2.")

    # Prepare Forte objects: state_weights_map, mo_space_info, scf_info
    data = ObjectsFromPsi4(**kwargs).run(data)

    # Make an integral object
    time_pre_ints = time.time()

    data.ints = forte.make_ints_from_psi4(data.psi_wfn, data.options, data.mo_space_info)

    start = time.time()

    # Rotate orbitals before computation
    orb_type = data.options.get_str("ORBITAL_TYPE")
    if orb_type != "CANONICAL":
        OrbitalTransformation(orb_type, job_type != "NONE").run(data)

    if job_type == "CASSCF":
        casscf = forte.make_casscf(data.state_weights_map, data.scf_info, data.options, data.mo_space_info, data.ints)
        energy = casscf.compute_energy()
        casscf.compute_gradient()

    if job_type == "MCSCF_TWO_STEP":
        data = MCSCF(data.options.get_str("ACTIVE_SPACE_SOLVER")).run(data)
        energy = data.results.value("energy")

    if job_type == "NEWDRIVER" and correlation_solver == "DSRG-MRPT2":
        forte_driver(data)

    time_pre_deriv = time.time()

    derivobj = psi4.core.Deriv(data.psi_wfn)
    derivobj.set_deriv_density_backtransformed(True)
    derivobj.set_ignore_reference(True)
    if int_type == "DF":
        grad = derivobj.compute_df("DF_BASIS_SCF", "DF_BASIS_MP2")
    else:
        grad = derivobj.compute(psi4.core.DerivCalcType.Correlated)
    data.psi_wfn.set_gradient(grad)
    optstash.restore()

    end = time.time()

    # Close ambit, etc.
    # forte.cleanup()

    # Print timings
    psi4.core.print_out("\n\n ==> Forte Timings <==\n")
    times = [
        ("prepare integrals", start - time_pre_ints),
        ("run forte energy", time_pre_deriv - start),
        ("compute derivative integrals", end - time_pre_deriv),
    ]
    max_key_size = max(len(k) for k, v in times)
    for key, value in times:
        psi4.core.print_out(f"\n  Time to {key:{max_key_size}} : {value:12.3f} seconds")
    psi4.core.print_out(f'\n  {"Total":{max_key_size + 8}} : {end - time_pre_ints:12.3f} seconds\n')

    # Dump orbitals if needed
    if data.options.get_bool("DUMP_ORBITALS"):
        dump_orbitals(data.psi_wfn)

    return data.psi_wfn


def mr_dsrg_pt2(job_type, data):
    """
    Driver to perform a MCSRGPT2_MO computation.

    :return: the computed energy
    """
    final_energy = 0.0

    options = data.options
    ref_wfn = data.psi_wfn
    state_weights_map = data.state_weights_map
    mo_space_info = data.mo_space_info
    scf_info = data.scf_info
    ints = data.ints

    state = forte.make_state_info_from_psi(options)
    # generate a list of states with their own weights
    state_map = forte.to_state_nroots_map(state_weights_map)

    cas_type = options.get_str("ACTIVE_SPACE_SOLVER")
    actv_type = options.get_str("FCIMO_ACTV_TYPE")
    if actv_type == "CIS" or actv_type == "CISD":
        raise Exception("Forte: VCIS/VCISD is not supported for MR-DSRG-PT2")
    max_rdm_level = 2 if options.get_str("THREEPDC") == "ZERO" else 3
    data = ActiveSpaceInts(active="ACTIVE", core=["RESTRICTED_DOCC"]).run(data)
    ci = forte.make_active_space_solver(cas_type, state_map, scf_info, mo_space_info, options, data.as_ints)
    ci.compute_energy()

    rdms = ci.compute_average_rdms(state_weights_map, max_rdm_level, forte.RDMsType.spin_dependent)
    semi = forte.SemiCanonical(mo_space_info, ints, options)
    semi.semicanonicalize(rdms)

    mcsrgpt2_mo = forte.MCSRGPT2_MO(rdms, options, ints, mo_space_info)
    energy = mcsrgpt2_mo.compute_energy()
    return energy


# Integration with driver routines
psi4.driver.procedures["energy"]["forte"] = energy_forte
psi4.driver.procedures["gradient"]["forte"] = gradient_forte
