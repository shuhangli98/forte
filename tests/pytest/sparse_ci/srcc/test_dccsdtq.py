#!/usr/bin/env python
# -*- coding: utf-8 -*-


def test_ccsdtq():
    """Test CCSD on H4 using RHF/DZ orbitals"""

    import pytest
    import forte.proc.scc as scc
    import forte
    import psi4

    ref_energy = -2.225370535177  # from psi4

    geom = """
     H 0.0 0.0 0.0
     H 0.0 0.0 1.0
     H 0.0 0.0 2.0
     H 0.0 0.0 3.0
    """

    scf_energy, psi4_wfn = forte.utils.psi4_scf(geom, basis="DZ", reference="RHF")
    data = forte.modules.ObjectsUtilPsi4(ref_wnf=psi4_wfn, mo_spaces={}).run()
    calc_data = scc.run_cc(data.as_ints, data.scf_info, data.mo_space_info, cc_type="dcc", max_exc=4, on_the_fly=True)

    psi4.core.clean()

    energy = calc_data[-1][1]

    print(f"  HF energy:     {scf_energy}")
    print(f"  CCSDTQ energy: {energy}")
    print(f"  E - Eref:      {energy - ref_energy}")

    assert energy == pytest.approx(ref_energy, 1.0e-11)


if __name__ == "__main__":
    test_ccsdtq()
