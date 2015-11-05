#include "casscf.h"
#include "reference.h"
#include "integrals.h"
#include <libpsio/psio.hpp>
#include <libpsio/psio.h>
#include <libmints/molecule.h>
#include <libqt/qt.h>
#include <libmints/matrix.h>
#include "helpers.h"
#include <libfock/jk.h>
#include <libmints/mints.h>
#include "fci_solver.h"
#include <psifiles.h>
#include <libmints/factory.h>
#include <libmints/mintshelper.h>
#include <lib3index/cholesky.h>
#include "fci_mo.h"
#include <libthce/lreri.h>
namespace psi{ namespace forte{

CASSCF::CASSCF(Options &options,
               std::shared_ptr<ForteIntegrals> ints, std::shared_ptr<MOSpaceInfo> mo_space_info)
         : options_(options),
         wfn_(Process::environment.wavefunction()),
         ints_(ints),
         mo_space_info_(mo_space_info)
{
    startup();
}


void CASSCF::compute_casscf()
{
    if(na_ == 0)
    {
        outfile->Printf("\n\n\n Please set the active space");
        throw PSIEXCEPTION(" The active space is zero.  Set the active space");
    }
    else if(na_ == nmo_)
    {
        outfile->Printf("\n Your about to do an all active CASSCF");
        throw PSIEXCEPTION("The active space is all the MOs.  Orbitals don't matter at this point");
    }

    int maxiter = options_.get_int("CASSCF_ITERATIONS");

    /// Provide a nice summary at the end for iterations
    std::vector<int> iter_con;
    std::vector<double> g_norm_con;
    std::vector<double> E_casscf_con;
    /// FrozenCore C Matrix is never rotated
    /// Can bring this out of loop
    F_froze_ = set_frozen_core_orbitals();

    ///Start the iteration
    for(int iter = 0; iter < maxiter; iter++)
    {
       iter_con.push_back(iter);
        if(iter==0)
        {
            print_h2("CASSCF Iteration");
        }

        /// Perform a CAS-CI using either York's code or Francesco's
        /// If CASSCF_DEBUG_PRINTING is on, will compare CAS-CI with SPIN-FREE RDM
        E_casscf_ = 0.0;
        cas_ci();
        E_casscf_con.push_back(E_casscf_);


        ///^I F_{pq} = h_{pq} + 2.0 * (pq | ii) - (pi | iq)
        /// This is done via JK build
        /// First gets C_core and then builds fock matrix
        /// Everything is done in C1 BASIS
        form_fock_core();

        ///^A F_{pq} = gamma_{uv} * [(pq | uv) - (pv | uq) * 1/2]
        /// TODO:  Use JK builder.
        /// Also, build using DFERI and use df_basis_scf rather than df_basis_mp2
        form_fock_active();

        /// Compute the orbital gradient
        /// Orbital gradient should go to zero when CASSCF is converged
        orbital_gradient();
        double g_norm = g_->rms();
        g_norm_con.push_back(g_norm);

        if(g_norm < options_.get_double("CASSCF_CONVERGENCE"))
        {
            break;
            outfile->Printf("\n\n CASSCF CONVERGED: @ %8.8f \n\n",E_casscf_ );
            outfile->Printf("\n %8.8f", g_norm);
        }
        ///Build a diagonal hessian update.
        /// TODO:  Write a full Hessian (maybe!?)
        diagonal_hessian();

        //Update MO coefficients
        // Use update provide in Hohenstein paper (Atomic orbital CASSCF)
        SharedMatrix S(new Matrix("S", nmo_, nmo_));

        int offset = 0;
        for(size_t h = 0; h < nirrep_; h++){
            for(int p = 0; p < nmopi_[h]; p++){
                int poff = p + offset;
                for(int q = 0; q < nmopi_[h]; q++){
                    int qoff = q + offset;
                    if(poff < qoff)
                    {
                        if(d_->get(poff,qoff) > 1e-8)
                            S->set(poff,qoff, g_->get(poff,qoff) / d_->get(poff,qoff));
                    }
                    else if(poff > qoff)
                    {
                        if(d_->get(qoff,poff) > 1e-8)
                            S->set(poff,qoff,-1.0 * g_->get(qoff,poff) / d_->get(qoff,poff));
                    }

                }
            }
        offset += nmopi_[h];
        }
        auto na_vec = mo_space_info_->get_corr_abs_mo("ACTIVE");
        for(int u = 0; u < na_; u++)
            for(int v = 0; v < na_; v++)
                S->set(na_vec[u], na_vec[v], 0.0);

        Matrix S_mat;
        S_mat.copy(S);
        S_mat.expm();

        SharedMatrix S_mat_s = S_mat.clone();

        auto nfrozen_docc = mo_space_info_->get_dimension("FROZEN_DOCC");
        Dimension true_nmopi = wfn_->nmopi();

        SharedMatrix S_sym(new Matrix(nirrep_, true_nmopi, true_nmopi));
        offset = 0;
        int frozen = 0;
        for(size_t h = 0; h < nirrep_; h++){
            frozen = nfrozen_docc[h];
            for(int p = 0; p < nmopi_[h]; p++){
                for(int q = 0; q < nmopi_[h]; q++){
                    S_sym->set(h, p + frozen, q + frozen, S_mat_s->get(p + offset, q + offset));
                }
            }
            offset += nmopi_[h];
        }
        for(size_t h = 0; h < nirrep_; h++){
            for(int p = 0; p < nfrozen_docc[h]; p++){
                S_sym->set(h, p, p, 1.0);
            }
        }

        Call_->set_name("symmetry aware C");

        SharedMatrix Ca = wfn_->Ca();
        SharedMatrix Cb = wfn_->Cb();

        SharedMatrix Cp = Matrix::doublet(Ca, S_sym);
        Cp->set_name("Updated C");
        if(casscf_debug_print_)
        {
            Cp->print();
            S_sym->print();
        }

        Ca->copy(Cp);
        Cb->copy(Cp);
        ///Right now, this retransforms all the integrals
        ///Think of ways of avoiding this
        ints_->retransform_integrals();

        if(options_.get_bool("CASSCF_DEBUG_PRINTING"))
        {
            S_sym->print();
            g_->print();
            d_->print();
            Cp->print();
        }

        /// Use the newly transformed MO to create a CMatrix that is aware of symmetry
        Call_->zero();
        Call_ = make_c_sym_aware();

    }
    outfile->Printf("\n iter    g_norm      E_CASSCF");
    for(size_t i = 0; i < iter_con.size(); i++)
    {
        outfile->Printf("\n %d  %10.12f   %10.12f", i, g_norm_con[i], E_casscf_con[i]);
    }
    if(iter_con.size() == size_t(maxiter))
    {
        outfile->Printf("\n CASSCF did not converged");
        throw PSIEXCEPTION("CASSCF did not converged.");
    }
    Process::environment.globals["CURRENT ENERGY"] = E_casscf_;
    Process::environment.globals["CASSCF_ENERGY"] = E_casscf_;


}

void CASSCF::startup()
{
    print_method_banner({"Complete Active Space Self Consistent Field","Kevin Hannon"});
    na_  = mo_space_info_->size("ACTIVE");
    nmo_ = mo_space_info_->size("CORRELATED");
    nsopi_ = wfn_->nsopi();
    Call_ = make_c_sym_aware();
    auto nactive_array = mo_space_info_->get_absolute_mo("ACTIVE");
    auto restricted_array = mo_space_info_->get_absolute_mo("RESTRICTED_DOCC");
    auto virtual_array    = mo_space_info_->get_absolute_mo("RESTRICTED_UOCC");
    nrdocc_ = restricted_array.size();
    nvir_  = virtual_array.size();
    nmopi_ = mo_space_info_->get_dimension("CORRELATED");

    casscf_debug_print_ = options_.get_bool("CASSCF_DEBUG_PRINTING");

    if(casscf_debug_print_)
    {
        outfile->Printf("\n ACTIVE: ");
        for(auto active : nactive_array){outfile->Printf(" %d", active);}
        outfile->Printf("\n RESTRICTED: ");
        for(auto restricted : restricted_array){outfile->Printf(" %d", restricted);}
        outfile->Printf("\n VIRTUAL: ");
        for(auto virtual_index : virtual_array){outfile->Printf(" %d", virtual_index);}

    }
    int_type_ = ints_->integral_type();
    do_soscf_ = options_.get_bool("CASSCF_SOSCF");


}
boost::shared_ptr<Matrix> CASSCF::make_c_sym_aware()
{
    ///Step 1: Obtain guess MO coefficients C_{mup}
    /// Since I want to use these in a symmetry aware basis,
    /// I will move the C matrix into a Pfitzer ordering
    Dimension nmopi = mo_space_info_->get_dimension("ALL");

    SharedMatrix Call_sym = wfn_->Ca();
    Ca_sym_ = Call_sym;

    SharedMatrix aotoso = wfn_->aotoso();

    /// I want a C matrix in the C1 basis but symmetry aware
    size_t nso = wfn_->nso();
    nirrep_ = wfn_->nirrep();
    SharedMatrix Call(new Matrix(nso, nmopi.sum()));

    // Transform from the SO to the AO basis for the C matrix.  
    // just transfroms the C_{mu_ao i} -> C_{mu_so i}
    for (size_t h = 0, index = 0; h < nirrep_; ++h){
        for (int i = 0; i < nmopi[h]; ++i){
            size_t nao = nso;
            size_t nso = nsopi_[h];

            if (!nso) continue;

            C_DGEMV('N',nao,nso,1.0,aotoso->pointer(h)[0],nso,&Call_sym->pointer(h)[0][i],nmopi[h],0.0,&Call->pointer()[0][index],nmopi.sum());

            index += 1;
        }

    }

    return Call;
}
void CASSCF::cas_ci()
{
    ///Calls francisco's FCI code and does a CAS-CI with the active given in the input
    SharedMatrix gamma2_matrix(new Matrix("gamma2", na_*na_, na_*na_));
    if(options_.get_str("CAS_TYPE") == "FCI")
    {
        boost::shared_ptr<FCI> fci_casscf(new FCI(wfn_,options_,ints_,mo_space_info_));
        fci_casscf->set_max_rdm_level(3);
        fci_casscf->compute_energy();

        //Used to grab the computed energy and RDMs.
        cas_ref_ = fci_casscf->reference();
        E_casscf_ = cas_ref_.get_Eref();
        if(options_.get_bool("CASSCF_DEBUG_PRINTING"))
        {
            double E_casscf_check = cas_check(cas_ref_);
            outfile->Printf("\n E_casscf_check - E_casscf_ = difference\n");
            outfile->Printf("\n %8.8f - %8.8f = %8.8f", E_casscf_check, E_casscf_, E_casscf_check - E_casscf_);
        }
        ambit::Tensor L2aa = cas_ref_.g2aa();
        ambit::Tensor L2ab = cas_ref_.g2ab();
        ambit::Tensor L2bb = cas_ref_.g2bb();

        ambit::Tensor gamma2 = ambit::Tensor::build(ambit::kCore, "gamma2", {na_, na_, na_, na_});

        //// This may or may not be correct.  Really need to find a way to check this code
        gamma2("u, v, x, y") = L2aa("u, v, x, y") + L2ab("u, v, x, y") + L2ab("v, u, y, x") + L2bb("u, v, x, y");
        gamma2_ = ambit::Tensor::build(ambit::kCore, "gamma2", {na_, na_, na_, na_});
        gamma2_("u, v, x, y") = gamma2("u, v, x, y") + gamma2("v, u, x, y") + gamma2("u, v, y, x") + gamma2("v, u, y, x");
        gamma2_.scale(0.25);
        gamma2_.iterate([&](const std::vector<size_t>& i,double& value){
            gamma2_matrix->set(i[0] * i[1] + i[1], i[2] * i[3] + i[3], value);});


    }
    else if(options_.get_str("CAS_TYPE") == "CAS")
    {
        FCI_MO cas(options_, ints_, mo_space_info_, false);
        cas_ref_ = cas.reference();
        E_casscf_ = cas_ref_.get_Eref();

        ambit::Tensor L2aa = cas_ref_.L2aa();
        ambit::Tensor L2ab = cas_ref_.L2ab();
        ambit::Tensor L2bb = cas_ref_.L2bb();
        ambit::Tensor L1a  = cas_ref_.L1a();
        ambit::Tensor L1b  = cas_ref_.L1b();

        L2aa("p,q,r,s") += L1a("p,r") * L1a("q,s");
        L2aa("p,q,r,s") -= L1a("p,s") * L1a("q,r");

        L2ab("pqrs") +=  L1a("pr") * L1b("qs");
        //L2ab("pqrs") += L1b("pr") * L1a("qs");

        L2bb("pqrs") += L1b("pr") * L1b("qs");
        L2bb("pqrs") -= L1b("ps") * L1b("qr");

        ambit::Tensor gamma2 = ambit::Tensor::build(ambit::kCore, "gamma2", {na_, na_, na_, na_});

        // This may or may not be correct.  Really need to find a way to check this code
        gamma2("u,v,x,y") +=  L2aa("u,v,x, y");
        gamma2("u,v,x,y") +=  L2ab("u,v,x,y");
        //gamma2("u,v,x,y") +=  L2ab("v, u, y, x");
        //gamma2("u,v,x,y") +=  L2bb("u,v,x,y");

        //gamma2_("u,v,x,y") = gamma2_("x,y,u,v");
        //gamma2_("u,v,x,y") = gamma2_("")
        gamma2_ = ambit::Tensor::build(ambit::kCore, "gamma2", {na_, na_, na_, na_});
        gamma2_.copy(gamma2);
        gamma2_.scale(2.0);
        gamma2_.iterate([&](const std::vector<size_t>& i,double& value){
            gamma2_matrix->set(i[0] * i[1] + i[1], i[2] * i[3] + i[3], value);});
        cas_ref_ = cas.reference();
        E_casscf_ = cas_ref_.get_Eref();
        if(options_.get_bool("CASSCF_DEBUG_PRINTING"))
        {
            double E_casscf_check = cas_check(cas_ref_);
            outfile->Printf("\n E_casscf_check - E_casscf_ = difference\n");
            outfile->Printf("\n %8.8f - %8.8f = %8.8f", E_casscf_check, E_casscf_, E_casscf_check - E_casscf_);
        }

    }
    /// Compute the 1RDM
    ambit::Tensor gamma_no_spin = ambit::Tensor::build(ambit::kCore,"Return",{na_, na_});
    gamma1_ = ambit::Tensor::build(ambit::kCore,"Return",{na_, na_});
    ambit::Tensor gamma1a = cas_ref_.L1a();
    ambit::Tensor gamma1b = cas_ref_.L1b();

    gamma_no_spin("i,j") = (gamma1a("i,j") + gamma1b("i,j"));
    //gamma_no_spin("i,j") = 0.5 * (gamma1a("i,j") + gamma1b("i,j") + gamma1a("j,i") + gamma1b("j,i"));

    SharedMatrix gamma_spin_free(new Matrix("Gamma", na_, na_));
    gamma_no_spin.iterate([&](const std::vector<size_t>& i,double& value){
        gamma_spin_free->set(i[0], i[1], value);});
    gamma1M_ = gamma_spin_free;
    gamma1_ = gamma_no_spin;
    gamma2M_ = gamma2_matrix;

    outfile->Printf("\n Done with CAS-CI function");
}

void CASSCF::form_fock_core()
{

    /// Get the CoreHamiltonian in AO basis

    //boost::shared_ptr<PSIO> psio_ = PSIO::shared_object();

    boost::shared_ptr<MintsHelper> mints(new MintsHelper());
    //SharedMatrix T = mints->ao_kinetic();
    //SharedMatrix V = mints->ao_potential();
    SharedMatrix T = mints->so_kinetic();
    SharedMatrix V = mints->so_potential();

    SharedMatrix H = T->clone();
    H->add(V);

    //H->transform(Call_);
    H->transform(Ca_sym_);
    if(casscf_debug_print_){
        H->set_name("CORR_HAMIL");
        H->print();
    }

    ///Step 2: From Hamiltonian elements
    ///This will use JK builds (Equation 18 - 22)
    /// F_{pq}^{core} = C_{mu p}C_{nu q} [h_{uv} + 2J^{(D_c) - K^{(D_c)}]
    Dimension frozen_dim = mo_space_info_->get_dimension("FROZEN_DOCC");
    Dimension restricted_dim = mo_space_info_->get_dimension("RESTRICTED_DOCC");
    ///Have to go from the full C matrix to the C_core in the SO basis
    /// tricky...tricky
    //SharedMatrix C_core(new Matrix("C_core", nmo_, inactive_dim_abs.size()));

    // Need to get the inactive block of the C matrix
    SharedMatrix C_core(new Matrix("C_core", nirrep_, nsopi_, restricted_dim));
    SharedMatrix F_core_c1(new Matrix("F_core_no_sym", nmo_, nmo_));
    F_core_c1->zero();

    if(restricted_dim.sum() > 0)
    {
        for(size_t h = 0; h < nirrep_; h++){
            for(int mu = 0; mu < nsopi_[h]; mu++){
                for(int i = 0; i <  restricted_dim[h]; i++){
                    C_core->set(h, mu, i, Ca_sym_->get(h, mu, i + frozen_dim[h]));
                }
            }
        }
        if(casscf_debug_print_){
            C_core->print();
        }

        boost::shared_ptr<JK> JK_core = JK::build_JK();

        JK_core->set_memory(Process::environment.get_memory() * 0.8);
        /// Already transform everything to C1 so make sure JK does not do this.

        /////TODO: Make this an option in my code
        //JK_core->set_cutoff(options_.get_double("INTEGRAL_SCREENING"));
        JK_core->set_cutoff(options_.get_double("INTEGRAL_SCREENING"));
        JK_core->initialize();

        std::vector<boost::shared_ptr<Matrix> >&Cl = JK_core->C_left();

        Cl.clear();
        Cl.push_back(C_core);

        JK_core->compute();

        SharedMatrix J_core = JK_core->J()[0];
        SharedMatrix K_core = JK_core->K()[0];

        J_core->scale(2.0);
        SharedMatrix F_core = J_core->clone();
        F_core->subtract(K_core);

        /// If there are frozen orbitals, need to add
        /// FrozenCore Fock matrix to inactive block
        if(frozen_dim.sum() > 0)
        {
            F_core->add(F_froze_);
        }
        F_core->transform(Ca_sym_);
        F_core->add(H);

        int offset = 0;
        for(size_t h = 0; h < nirrep_; h++){
            for(int p = 0; p < nmopi_[h]; p++){
                for(int q = 0; q < nmopi_[h]; q++){
                    F_core_c1->set(p + offset, q + offset, F_core->get(h, p + frozen_dim[h], q + frozen_dim[h]));
                }
            }
            offset += nmopi_[h];
        }
    }
    F_core_   = F_core_c1;
    if(casscf_debug_print_)
    {
        F_core_->set_name("INACTIVE_FOCK");
        F_core_->print();
    }

}

void CASSCF::form_fock_active()
{
    ///Step 3:
    ///Compute equation 10:
    /// The active OPM is defined by gamma = gamma_{alpha} + gamma_{beta}
    std::vector<size_t> active_abs_mo = mo_space_info_->get_absolute_mo("ACTIVE");

    size_t nso = wfn_->nso();
    SharedMatrix C_active(new Matrix("C_active", nso,na_));

    for(size_t mu = 0; mu < nso; mu++){
        for(size_t u = 0; u <  na_; u++){
            C_active->set(mu, u, Call_->get(mu, active_abs_mo[u]));
        }
    }

    ambit::Tensor Cact = ambit::Tensor::build(ambit::kCore, "Cact", {nso, na_});
    ambit::Tensor OPDM_aoT = ambit::Tensor::build(ambit::kCore, "OPDM_aoT", {nso, nso});

    Cact.iterate([&](const std::vector<size_t>& i,double& value){
        value = C_active->get(i[0], i[1]);});

    ///Transfrom the all active OPDM to the AO basis
    OPDM_aoT("mu,nu") = gamma1_("u,v")*Cact("mu, u") * Cact("nu, v");
    SharedMatrix OPDM_ao(new Matrix("OPDM_AO", nso, nso));

    OPDM_aoT.iterate([&](const std::vector<size_t>& i,double& value){
        OPDM_ao->set(i[0], i[1], value);});

    ///In order to use JK builder for active part, need a Cmatrix like matrix
    /// AO OPDM looks to be semi positive definite so perform CholeskyDecomp and feed this to JK builder
    boost::shared_ptr<CholeskyMatrix> Ch (new CholeskyMatrix(OPDM_ao, 1e-12, Process::environment.get_memory()));
    Ch->choleskify();
    SharedMatrix L_C = Ch->L();
    SharedMatrix L_C_correct(new Matrix("L_C_order", nso, Ch->Q()));

    for(size_t mu = 0; mu < nso; mu++){
        for(int Q = 0; Q < Ch->Q(); Q++){
            L_C_correct->set(mu, Q, L_C->get(Q, mu));
        }
    }

    boost::shared_ptr<JK> JK_act = JK::build_JK();

    JK_act->set_memory(Process::environment.get_memory() * 0.8);

    /////TODO: Make this an option in my code
    JK_act->set_cutoff(options_.get_double("INTEGRAL_SCREENING"));
    JK_act->initialize();

    std::vector<boost::shared_ptr<Matrix> >&Cl = JK_act->C_left();

    auto active_dim = mo_space_info_->get_dimension("ACTIVE");
    Cl.clear();
    Cl.push_back(L_C_correct);


    JK_act->set_allow_desymmetrization(false);
    JK_act->compute();

    SharedMatrix J_core = JK_act->J()[0];
    SharedMatrix K_core = JK_act->K()[0];

    SharedMatrix F_act = J_core->clone();
    K_core->scale(0.5);
    F_act->subtract(K_core);
    F_act->transform(Call_);
    SharedMatrix F_act_no_frozen(new Matrix("F_act", nmo_, nmo_));
    outfile->Printf("\n nmo_:%d", nmo_);
    F_act->set_name("FOCK_ACTIVE");

    auto nfrozen_abs = mo_space_info_->get_absolute_mo("FROZEN_DOCC");
    Dimension frozen_dim = mo_space_info_->get_dimension("FROZEN_DOCC");
    Dimension nmopi      = mo_space_info_->get_dimension("ALL");
    int offset_nofroze = 0;
    int offset_froze   = 0;
    for(int h = 0; h < nirrep_; h++){
        int froze = frozen_dim[h];
        for(size_t p = froze; p < nmopi[h]; p++){
            for(size_t q = froze; q < nmopi[h]; q++){
                F_act_no_frozen->set(p  - froze + offset_froze, q - froze + offset_froze, F_act->get(p + offset_nofroze, q + offset_nofroze));
            }
        }
        offset_froze   += nmopi_[h];
        offset_nofroze += nmopi[h];
    }
    if(casscf_debug_print_){
        F_act_no_frozen->print();
    }

    if(nfrozen_abs.size() == 0)
    {
        F_act_ = F_act;
    }
    else{
        F_act_ = F_act_no_frozen;
    }

}
void CASSCF::orbital_gradient()
{
    std::vector<size_t> nmo_array = mo_space_info_->get_corr_abs_mo("CORRELATED");
    ///From Y_{pt} = F_{pu}^{core} * Gamma_{tu}
    ambit::Tensor Y = ambit::Tensor::build(ambit::kCore,"Y",{nmo_, na_});
    ambit::Tensor F_pu = ambit::Tensor::build(ambit::kCore, "F_pu", {nmo_, na_});
    auto active_mo = mo_space_info_->get_corr_abs_mo("ACTIVE");
    if(nrdocc_ > 0)
    {
        F_pu.iterate([&](const std::vector<size_t>& i,double& value){
            value = F_core_->get(nmo_array[i[0]],active_mo[i[1]]);});
    }
    Y("p,t") = F_pu("p,u") * gamma1_("t, u");

    SharedMatrix Y_m(new Matrix("Y_m", nmo_, na_));

    Y.iterate([&](const std::vector<size_t>& i,double& value){
        Y_m->set(nmo_array[i[0]],i[1], value);});
    Y_ = Y_m;
    Y_->set_name("F * gamma");
    if(casscf_debug_print_)
    {
        Y_->print();
    }

    //Form Z (pu | v w) * Gamma2(tuvw)
    //One thing I am not sure about for Gamma2->how to get spin free RDM from spin based RDM
    //gamma1 = gamma1a + gamma1b;
    //gamma2 = gamma2aa + gamma2ab + gamma2ba + gamma2bb
    /// lambda2 = gamma1*gamma1

    ambit::Tensor tei_puvy = ambit::Tensor::build(ambit::kCore, "puvy", {nmo_, na_, na_, na_});
    std::vector<size_t> na_array = mo_space_info_->get_corr_abs_mo("ACTIVE");
    tei_puvy = ints_->aptei_ab_block(nmo_array, na_array, na_array, na_array);
    ambit::Tensor Z = ambit::Tensor::build(ambit::kCore, "Z", {nmo_, na_});

    //(pu | x y) -> <px | uy> * gamma2_{"t, u, x, y"
    Z("p, t") = tei_puvy("p,u,x,y") * gamma2_("t, u, x, y");
    SharedMatrix Zm(new Matrix("Zm", nmo_, na_));
    Z.iterate([&](const std::vector<size_t>& i,double& value){
        Zm->set(nmo_array[i[0]],i[1], value);});

    Z_ = Zm;
    Z_->set_name("g * rdm2");
    if(casscf_debug_print_)
    {
        Z_->print();
    }
    // g_ia = 4F_core + 2F_act
    // g_ta = 2Y + 4Z
    // g_it = 4F_core + 2 F_act - 2Y - 4Z;

    //GOTCHA:  Z and T are of size nmo by na
    //The absolute MO should not be used to access elements of Z, Y, or Gamma since these are of 0....na_ arrays
    auto occ_array = mo_space_info_->get_corr_abs_mo("RESTRICTED_DOCC");
    auto virt_array = mo_space_info_->get_corr_abs_mo("RESTRICTED_UOCC");
    auto active_array = mo_space_info_->get_corr_abs_mo("ACTIVE");

    SharedMatrix Orb_grad(new Matrix("G_pq", nmo_, nmo_));
    Orb_grad->set_name("CASSCF Gradient");

    for(size_t ii = 0; ii < occ_array.size(); ii++)
        for(size_t ti = 0; ti < active_array.size(); ti++){
            {
                size_t i = occ_array[ii];
                size_t t = active_array[ti];
                //double value_it = 4 * F_core_->get(i, t) + 2 * F_act_->get(i,t) - 2 * Y_->get(i,ti) - 4 * Z_->get(i, ti);
                double value_it = 4 * F_core_->get(i, t) + 4 * F_act_->get(i,t) - 2 * Y_->get(i,ti) - 2 * Z_->get(i, ti);
                Orb_grad->set(i,t, value_it) ;
            }
    }
    for(auto i : occ_array){
        for(auto a : virt_array){
            //double value_ia = F_core_->get(i, a) * 4.0 + F_act_->get(i, a) * 2.0;
            double value_ia = F_core_->get(i, a) * 4.0 + F_act_->get(i, a) * 4.0;
            Orb_grad->set(i, a, value_ia);
        }
    }
    for(size_t ai = 0; ai < virt_array.size(); ai++){
        for(size_t ti = 0; ti < active_array.size(); ti++){
            size_t t = active_array[ti];
            size_t a = virt_array[ai];
            double value_ta = 2.0 * Y_->get(a, ti) + 2.0 * Z_->get(a, ti);
            Orb_grad->set(t,a, value_ta);
        }
    }

    if(casscf_debug_print_)
    {
        Orb_grad->print();
    }
    for(size_t u = 0; u < na_; u++){
        for(size_t v = 0; v < na_; v++){
            Orb_grad->set(active_array[u], active_array[v], 0.0);
        }
    }

    g_ = Orb_grad;
    if(casscf_debug_print_)
    {
        g_->print();
    }


}
void CASSCF::diagonal_hessian()
{
    SharedMatrix D(new Matrix("DH", nmo_, nmo_));

    auto i_array = mo_space_info_->get_corr_abs_mo("RESTRICTED_DOCC");
    auto a_array = mo_space_info_->get_corr_abs_mo("RESTRICTED_UOCC");
    auto t_array = mo_space_info_->get_corr_abs_mo("ACTIVE");

    for(size_t ii = 0; ii < i_array.size(); ii++){
        for(size_t ai = 0; ai < a_array.size(); ai++){
            size_t a = a_array[ai];
            size_t i = i_array[ii];
            //double value_ia = F_core_->get(a,a) * 4.0 + 2 * F_act_->get(a,a);
            //value_ia -= 4.0 * F_core_->get(i,i)  - 2 * F_act_->get(i,i);
            double value_ia = (F_core_->get(a,a) * 4.0 + 4.0 * F_act_->get(a,a));
            value_ia -= (4.0 * F_core_->get(i,i)  + 4.0 * F_act_->get(i,i));
            D->set(i,a,value_ia);
        }
    }
    for(size_t ai = 0; ai < a_array.size(); ai++){
        for(size_t ti = 0; ti < t_array.size(); ti++){
            size_t a = a_array[ai];
            size_t t = t_array[ti];
            //double value_ta = 2.0 * gamma1M_->get(ti,ti) * F_core_->get(a,a);
            //value_ta += gamma1M_->get(ti,ti) * F_act_->get(a,a);
            //value_ta -= 2*Y_->get(t,ti) + 4.0 *Z_->get(t,ti);
            double value_ta = 2.0 * gamma1M_->get(ti,ti) * F_core_->get(a,a);
            value_ta += 2.0 * gamma1M_->get(ti,ti) * F_act_->get(a,a);
            value_ta -= (2*Y_->get(t,ti) + 2.0 *Z_->get(t,ti));
            D->set(t,a, value_ta);
        }
    }
    for(size_t ii = 0; ii < i_array.size(); ii++){
        for(size_t ti = 0; ti < t_array.size(); ti++){
            size_t i = i_array[ii];
            size_t t = t_array[ti];

            double value_it = 4.0 * F_core_->get(t,t)
                    + 4.0 * F_act_->get(t,t)
                    + 2.0 * gamma1M_->get(ti,ti) * F_core_->get(i,i);
            value_it+=2.0 * gamma1M_->get(ti,ti) * F_act_->get(i,i);
            value_it-=(4.0 * F_core_->get(i,i) + 4.0 * F_act_->get(i,i));
            value_it-=(2.0*Y_->get(t,ti) + 2.0 * Z_->get(t,ti));
            D->set(i,t, value_it);
        }
    }
    auto na_vec = mo_space_info_->get_corr_abs_mo("ACTIVE");
    for(size_t u = 0; u < na_; u++){
        for(size_t v = 0; v < na_; v++){
            D->set(na_vec[u], na_vec[v], 1.0);
        }
    }
    d_ = D;


}
double CASSCF::cas_check(Reference cas_ref)
{
    ambit::Tensor gamma1 = ambit::Tensor::build(ambit::kCore, "Gamma1", {na_, na_});
    ambit::Tensor gamma2 = ambit::Tensor::build(ambit::kCore, "Gamma2", {na_, na_, na_, na_});
    std::shared_ptr<FCIIntegrals> fci_ints = std::make_shared<FCIIntegrals>(ints_, mo_space_info_);

    /// Spin-free ORDM = gamma1_a + gamma1_b
    ambit::Tensor L1b = cas_ref.L1b();
    ambit::Tensor L1a = cas_ref.L1a();
    gamma1("u, v") = (L1a("u, v") + L1b("u, v"));
    std::string cas_type = options_.get_str("CAS_TYPE");
    if(cas_type=="FCI")
    {
        ambit::Tensor L2aa = cas_ref.g2aa();
        ambit::Tensor L2ab = cas_ref.g2ab();
        ambit::Tensor L2bb = cas_ref.g2bb();

        gamma2("u, v, x, y") = L2aa("u, v, x, y") + L2bb("u, v, x, y") + L2ab("u, v, x, y") + L2ab("v, u, y, x");
    }
    else if(cas_type=="CAS")
    {
        ambit::Tensor L2aa = cas_ref.L2aa();
        ambit::Tensor L2ab = cas_ref.L2ab();
        ambit::Tensor L2bb = cas_ref.L2bb();
        ambit::Tensor L1a  = cas_ref.L1a();
        ambit::Tensor L1b  = cas_ref.L1b();

        L2aa("p,q,r,s") += L1a("p,r") * L1a("q,s");
        L2aa("p,q,r,s") -= L1a("p,s") * L1a("q,r");

        L2ab("pqrs") +=  L1a("pr") * L1b("qs");
        //L2ab("pqrs") += L1b("pr") * L1a("qs");

        L2bb("pqrs") += L1b("pr") * L1b("qs");
        L2bb("pqrs") -= L1b("ps") * L1b("qr");

        // This may or may not be correct.  Really need to find a way to check this code
        gamma2.copy(L2aa);
        gamma2("u,v,x,y") +=  L2ab("u,v,x,y");
        //gamma2("u,v,x,y") +=  L2ab("v, u, y, x");
        //gamma2("u,v,x,y") +=  L2bb("u,v,x,y");

        //gamma2_("u,v,x,y") = gamma2_("x,y,u,v");
        //gamma2_("u,v,x,y") = gamma2_("")
        gamma2.scale(2.0);

    }

    double E_casscf = 0.0;

    std::vector<size_t> nmo_array = mo_space_info_->get_corr_abs_mo("CORRELATED");
    std::vector<size_t> na_array = mo_space_info_->get_corr_abs_mo("ACTIVE");

    ambit::Tensor tei_ab = ints_->aptei_ab_block(na_array, na_array, na_array, na_array);
    for (size_t p = 0; p < na_array.size(); ++p){
        for (size_t q = 0; q < na_array.size(); ++q){
            E_casscf += gamma1.data()[na_ * p + q] * fci_ints->oei_a(p,q);
        }
    }

    E_casscf += 0.5 * gamma2("u, v, x, y") * tei_ab("u, v, x, y");
    E_casscf += ints_->frozen_core_energy();
    E_casscf += fci_ints->scalar_energy();
    E_casscf += Process::environment.molecule()->nuclear_repulsion_energy();
    return E_casscf;

}
boost::shared_ptr<Matrix> CASSCF::set_frozen_core_orbitals()
{
    Dimension nmopi = mo_space_info_->get_dimension("ALL");
    Dimension frozen_dim = mo_space_info_->get_dimension("FROZEN_DOCC");
    SharedMatrix C_core(new Matrix("C_core",nirrep_, nmopi, frozen_dim));
    // Need to get the frozen block of the C matrix
    for(size_t h = 0; h < nirrep_; h++){
        for(int mu = 0; mu < nmopi[h]; mu++){
            for(int i = 0; i < frozen_dim[h]; i++){
                C_core->set(h, mu, i, Ca_sym_->get(h, mu, i));
            }
        }
    }

    boost::shared_ptr<JK> JK_core = JK::build_JK();

    JK_core->set_memory(Process::environment.get_memory() * 0.8);
    /// Already transform everything to C1 so make sure JK does not do this.
    //JK_core->set_allow_desymmetrization(false);

    /////TODO: Make this an option in my code
    //JK_core->set_cutoff(options_.get_double("INTEGRAL_SCREENING"));
    JK_core->set_cutoff(options_.get_double("INTEGRAL_SCREENING"));
    JK_core->initialize();

    std::vector<boost::shared_ptr<Matrix> >&Cl = JK_core->C_left();

    Cl.clear();
    Cl.push_back(C_core);

    JK_core->compute();

    SharedMatrix F_core = JK_core->J()[0];
    SharedMatrix K_core = JK_core->K()[0];

    F_core->scale(2.0);
    F_core->subtract(K_core);

    return F_core;

}

//boost::shared_ptr<Matrix> CASSCF::compute_so_hamiltonian()
//{
//
//    boost::shared_ptr<MintsHelper> mints(new MintsHelper());
//    SharedMatrix T = mints->so_kinetic();
//    SharedMatrix V = mints->so_potential();
//
//    SharedMatrix H = T->clone();
//    H->add(V);
//
//    H->transform(Ca_sym_);
//    return H;
//
//}
//boost::shared_ptr<DFERI> CASSCF::set_df_object()
//{
//    boost::shared_ptr<BasisSet> primary = wfn_->basisset();
//    boost::shared_ptr<BasisSet> auxiliary = BasisSet::pyconstruct_orbital(primary->molecule(), "DF_BASIS_SCF",options_.get_str("DF_BASIS_SCF"));
//    //int nao_ = primary->nbf();
//    //SharedMatrix aotoso = wfn_->aotoso();
//    //SharedMatrix AO_C = SharedMatrix(new Matrix("AO_C", nao_, nmo_));
//    //double** AO_Cp = AO_C->pointer();
//    //for (int h=0, offset=0, offset_act=0; h < nirrep_; h++){
//    //    int hnso = aotoso->colspi()[h];
//    //    if (hnso == 0) continue;
//    //    double** Up = aotoso->pointer(h);
//
//    //    // occupied
//    //    if (noccpi_[h]){
//    //        double** CSOp = matrices_["Cocc"]->pointer(h);
//    //        C_DGEMM('N','N',nao_,noccpi_[h],hnso,1.0,Up[0],hnso,CSOp[0],noccpi_[h],0.0,&AO_Cp[0][offset],aoc_rowdim);
//    //        offset += noccpi_[h];
//    //    }
//    //    // active
//    //    if (nactpi_[h]){
//    //        double** CSOp = matrices_["Cact"]->pointer(h);
//    //        C_DGEMM('N','N',nao_,nactpi_[h],hnso,1.0,Up[0],hnso,CSOp[0],nactpi_[h],0.0,&AO_Cp[0][offset],aoc_rowdim);
//    //        offset += nactpi_[h];
//
//    //        C_DGEMM('N','N',nao_,nactpi_[h],hnso,1.0,Up[0],hnso,CSOp[0],nactpi_[h],0.0,&AO_Cp[0][offset_act + nmo_],aoc_rowdim);
//    //        offset_act += nactpi_[h];
//    //    }
//    //    // virtual
//    //    if (nvirpi_[h]){
//    //        double** CSOp = matrices_["Cvir"]->pointer(h);
//    //        C_DGEMM('N','N',nao_,nvirpi_[h],hnso,1.0,Up[0],hnso,CSOp[0],nvirpi_[h],0.0,&AO_Cp[0][offset],aoc_rowdim);
//    //        offset += nvirpi_[h];
//    //    }
//    //}
//    boost::shared_ptr<DFERI> df = DFERI::build(primary,auxiliary,options_);
//    return df;
//}
//std::map<std::string, boost::shared_ptr<Matrix> > CASSCF::orbital_subset_helper()
//{
//    auto frozen_docc = mo_space_info_->get_dimension("FROZEN_DOCC");
//    auto restricted_docc = mo_space_info_->get_dimension("RESTRICTED_DOCC");
//    auto active = mo_space_info_->get_dimension("ACTIVE");
//    auto restricted_uocc = mo_space_info_->get_dimension("RESTRICTED_UOCC");
//    auto all_orbital = mo_space_info_->get_dimension("CORRELATED");
//
//
//    SharedMatrix C_docc(new Matrix(nirrep_,all_orbital , restricted_docc));
//    SharedMatrix C_active(new Matrix(nirrep_,all_orbital ,active) );
//    SharedMatrix C_virt(new Matrix(nirrep_,all_orbital , restricted_uocc));
//    for(size_t h = 0; h < nirrep_; h++){
//        int target = 0;
//        for(int p = 0; p < all_orbital[h]; p++){
//            for(int q = 0; q < restricted_docc[h]; q++){
//                C_docc->set(h, p, q, Ca_sym_->get(h, p, target + q));
//            }
//            target += frozen_docc[h];
//            outfile->Printf("\n frozen_docc:%d", target);
//            for(int q = 0; q < active[h]; q++){
//                C_active->set(h, p, q, Ca_sym_->get(h, p, target + q));
//            }
//            target += restricted_docc[h];
//            outfile->Printf("\n restricted_docc:%d", target);
//            for(int q = 0; q < restricted_uocc[h]; q++){
//                C_virt->set(h, p, q, Ca_sym_->get(h, p, target + q));
//            }
//            target += active[h];
//            outfile->Printf("\n active:%d", target);
//        }
//    }
//    std::map<std::string, boost::shared_ptr<Matrix> > orb_subset;
//    orb_subset["RESTRICTED_DOCC"] = C_docc;
//    orb_subset["ACTIVE"] = C_active;
//    orb_subset["RESTRICTED_UOCC"] = C_virt;
//
//
//    return orb_subset;
//}
/// Use Daniel's code to compute CASSCF using FCI from forte
//void CASSCF::compute_casscf_soscf()
//{
//    if(na_ == 0)
//    {
//        outfile->Printf("\n\n\n Please set the active space");
//        throw PSIEXCEPTION(" The active space is zero.  Set the active space");
//    }
//    else if(na_ == nmo_)
//    {
//        outfile->Printf("\n Your about to do an all active CASSCF");
//        throw PSIEXCEPTION("The active space is all the MOs.  Orbitals don't matter at this point");
//    }
//
//    int maxiter = options_.get_int("CASSCF_ITERATIONS");
//
//    /// Provide a nice summary at the end for iterations
//    std::vector<int> iter_con;
//    std::vector<double> g_norm_con;
//    std::vector<double> E_casscf_con;
//
//
//    //boost::shared_ptr<Matrix> H = compute_so_hamiltonian();
//    //boost::shared_ptr<DFERI> df = set_df_object();
//
//    //boost::shared_ptr<SOMCSCF> soscf;
//    //boost::shared_ptr<JK> jk = JK::build_JK();
//    //if(int_type_==DF){
//    //    soscf = boost::shared_ptr<SOMCSCF>(new DFSOMCSCF(jk, df, wfn_->aotoso(), H));
//    //}
//    //else if (int_type_==ConventionalInts){
//    ////    soscf = boost::shared_ptr<SOMCSCF>(new DiskSOMSCF(jk,
//    //}
//
//
//    ///Start the iteration
//    maxiter = 1;
//    for(int iter = 0; iter < maxiter; iter++)
//    {
//       iter_con.push_back(iter);
//        if(iter==0)
//        {
//            print_h2("CASSCF Iteration");
//        }
//
//        /// Perform a CAS-CI using either York's code or Francesco's
//        /// If CASSCF_DEBUG_PRINTING is on, will compare CAS-CI with SPIN-FREE RDM
//        E_casscf_ = 0.0;
//        cas_ci();
//        std::map<std::string, boost::shared_ptr<Matrix> > C_matrices = orbital_subset_helper();
//        E_casscf_con.push_back(E_casscf_);
//        soscf->update(C_matrices["RESTRICTED_DOCC"],
//               C_matrices["ACTIVE"],C_matrices["RESTRICTED_UOCC"],
//               gamma1M_, gamma2M_);
//        SharedMatrix Hessian = soscf->H_approx_diag();
//        Hessian->print();
//    }
//
//}

}}