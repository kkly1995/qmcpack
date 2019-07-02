//////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source
// License.  See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by:
// Miguel A. Morales, moralessilva2@llnl.gov 
//    Lawrence Livermore National Laboratory 
//
// File created by:
// Miguel A. Morales, moralessilva2@llnl.gov 
//    Lawrence Livermore National Laboratory 
////////////////////////////////////////////////////////////////////////////////

#ifndef QMCPLUSPLUS_AFQMC_PHMSD_HPP
#define QMCPLUSPLUS_AFQMC_PHMSD_HPP

#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <tuple>

#include "AFQMC/Utilities/readWfn.h"
#include "AFQMC/config.h"
#include "mpi3/shm/mutex.hpp"
#include "multi/array.hpp"
#include "multi/array_ref.hpp"
#include "AFQMC/Utilities/taskgroup.h"
#include "AFQMC/Matrix/array_of_sequences.hpp"
#include "AFQMC/Numerics/ma_lapack.hpp"

#include "AFQMC/HamiltonianOperations/HamiltonianOperations.hpp"
#include "AFQMC/Hamiltonians/Hamiltonian.hpp"
#include "AFQMC/SlaterDeterminantOperations/SlaterDetOperations.hpp"

#include "AFQMC/Wavefunctions/phmsd_helpers.hpp"
#include "AFQMC/Wavefunctions/Excitations.hpp"


namespace qmcplusplus
{

namespace afqmc
{

/*
 * Class that implements a multi-Slater determinant trial wave-function.
 * Single determinant wfns are also allowed. 
 * No relation between different determinants in the expansion is assumed.
 * Designed for non-orthogonal MSD expansions. 
 * For particle-hole orthogonal MSD wfns, use FastMSD.
 * NOTE: Optimization note: CLOSED and NONCOLLINEAR calculations with a unique reference
 *                          only need a single set of unique overlaps/determinants/energies.
 *                          Fix this to improve performance!!!! 
 * THERE IS A PROBLEM WITH CLOSED SHELL CALCULATIONS!!!!
 * INCONSISTENCY WHEN SPIN DEPENDENT QUANTITIES ARE REUESTED, e.g. G. 
 * SOLUTION: For CLOSED and NONCOLLINEAR, force a single reference PHMSD.
 *           Then, only calculate alpha component of things and assume beta==alpha when needed,
 *           even if this is not true.
 */  
class PHMSD: public AFQMCInfo
{

  // allocators
  using Allocator = std::allocator<ComplexType>; //device_allocator<ComplexType>;
  using Allocator_shared = shared_allocator<ComplexType>; //localTG_allocator<ComplexType>;

  // type defs
  using pointer = typename Allocator::pointer;
  using const_pointer = typename Allocator::const_pointer;
  using pointer_shared = typename Allocator_shared::pointer;
  using const_pointer_shared = typename Allocator_shared::const_pointer;

  using CVector = boost::multi::array<ComplexType,1,Allocator>;
  using RVector = boost::multi::array<RealType,1,Allocator>;
  using CMatrix = boost::multi::array<ComplexType,2,Allocator>;
  using CTensor = boost::multi::array<ComplexType,3,Allocator>;
  using CVector_ref = boost::multi::array_ref<ComplexType,1,pointer>;
  using CMatrix_ref = boost::multi::array_ref<ComplexType,2,pointer>;
  using CMatrix_cref = boost::multi::array_ref<const ComplexType,2,const_pointer>;
  using CTensor_ref = boost::multi::array_ref<ComplexType,3,pointer>;
  using CTensor_cref = boost::multi::array_ref<const ComplexType,3,const_pointer>;
  using shmCVector = boost::multi::array<ComplexType,1,Allocator_shared>;
  using shmCMatrix = boost::multi::array<ComplexType,2,Allocator_shared>;
  using shmC3Tensor = boost::multi::array<ComplexType,3,Allocator_shared>;
  using shmC4Tensor = boost::multi::array<ComplexType,4,Allocator_shared>;
  using shared_mutex = boost::mpi3::shm::mutex;
  using index_aos = ma::sparse::array_of_sequences<int,int,
                                                   shared_allocator<int>,
                                                   ma::sparse::is_root>;

  using stdCVector = boost::multi::array<ComplexType,1>;
  using stdCMatrix = boost::multi::array<ComplexType,2>;
  using stdCTensor = boost::multi::array<ComplexType,3>;
  using mpi3CVector = boost::multi::array<ComplexType,1,shared_allocator<ComplexType>>;

  public:

    PHMSD(AFQMCInfo& info, xmlNodePtr cur, afqmc::TaskGroup_& tg_, HamiltonianOperations&& hop_, 
          std::map<int,int>&& acta2mo_, std::map<int,int>&& actb2mo_,
          ph_excitations<int,ComplexType>&& abij_, 
          index_aos&& beta_coupled_to_unique_alpha__,
          index_aos&& alpha_coupled_to_unique_beta__,
          std::vector<PsiT_Matrix>&& orbs_, 
          WALKER_TYPES wlk, ValueType nce, int targetNW=1):
                AFQMCInfo(info),TG(tg_),
                SDetOp( SlaterDetOperations_shared<ComplexType>(
                //SDetOp( 
                        ((wlk!=NONCOLLINEAR)?(NMO):(2*NMO)),
                        ((wlk!=NONCOLLINEAR)?(NAEA):(NAEA+NAEB)) )),
                HamOp(std::move(hop_)),
                acta2mo(std::move(acta2mo_)),
                actb2mo(std::move(actb2mo_)),
                abij(std::move(abij_)),
                OrbMats(std::move(orbs_)),
                walker_type(wlk),NuclearCoulombEnergy(nce),
                shmbuff_for_E(nullptr),
                mutex(std::make_unique<shared_mutex>(TG.TG_local())),
                last_number_extra_tasks(-1),last_task_index(-1),
                local_group_comm(),
                shmbuff_for_G(nullptr),
                req_Gsend(MPI_REQUEST_NULL),
                req_Grecv(MPI_REQUEST_NULL),
                req_SMsend(MPI_REQUEST_NULL),
                req_SMrecv(MPI_REQUEST_NULL),
                maxnactive(std::max(OrbMats[0].size(0),OrbMats.back().size(0))),
                max_exct_n(std::max(abij.maximum_excitation_number()[0],
                                    abij.maximum_excitation_number()[1])),
                maxn_unique_confg(    
                    std::max(abij.number_of_unique_excitations()[0],
                             abij.number_of_unique_excitations()[1])),
                unique_overlaps({2,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                unique_Etot({2,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                QQ0inv0({1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                QQ0inv1({1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                GA2D0_shm({1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                GB2D0_shm({1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                local_ov({2,maxn_unique_confg}),
                local_etot({2,maxn_unique_confg}),
                local_QQ0inv0({OrbMats[0].size(0),NAEA}),
                local_QQ0inv1({OrbMats.back().size(0),NAEB}),
                Qwork({2*max_exct_n,max_exct_n}),
                Gwork({NAEA,maxnactive}),
                Ovmsd({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                Emsd({1,1,1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                QQ0A({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                QQ0B({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                GrefA({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}),
                GrefB({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                KEright({1,1,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                KEleft({1,1},shared_allocator<ComplexType>{TG.TG_local()}), 
                det_couplings{std::move(beta_coupled_to_unique_alpha__),
                              std::move(alpha_coupled_to_unique_beta__)}
    {
      /* To me, PHMSD is not compatible with walker_type=CLOSED unless
       * the MSD expansion is symmetric with respect to spin. For this, 
       * it is better to write a specialized class that assumes either spin symmetry
       * or e.g. Perfect Pairing.
       */
      if(walker_type == CLOSED) 
        APP_ABORT("Error: PHMSD requires walker_type != CLOSED.\n");

      compact_G_for_vbias = true; 
      transposed_G_for_vbias_ = HamOp.transposed_G_for_vbias();  
      transposed_G_for_E_ = HamOp.transposed_G_for_E();  
      transposed_vHS_ = HamOp.transposed_vHS();  
      fast_ph_energy = HamOp.fast_ph_energy();

      excitedState = false;  
      std::string excited_file("");  
      int i_=-1,a_=-1;  
      std::string recompute_ci("");
      recomputeCI = false;
      ParameterSet m_param;
      m_param.add(excited_file,"excited","std::string");
      // generalize this to multi-particle excitations, how do I read a list of integers???
      m_param.add(i_,"i","int");
      m_param.add(a_,"a","int");
      m_param.add(recompute_ci,"rediag","std::string");
      m_param.put(cur);
      std::transform(recompute_ci.begin(),recompute_ci.end(),recompute_ci.begin(),(int (*)(int)) tolower);
      if(recompute_ci=="yes" || recompute_ci=="true") recomputeCI=true;

      if(excited_file != "" && 
         i_ >= 0 &&
         a_ >= 0) {
        if(i_ < NMO && a_ < NMO) { 
          if(i_ >= NAEA || a_ < NAEA)
            APP_ABORT(" Errors: Inconsistent excited orbitals for alpha electrons. \n");
          excitedState=true;
          maxOccupExtendedMat = {a_,NAEB};
          numExcitations = {1,0};
          excitations.push_back({i_,a_});
        } else if(i_ >= NMO && a_ >= NMO) {
          if(i_ >= NMO+NAEB || a_ < NMO+NAEB)
            APP_ABORT(" Errors: Inconsistent excited orbitals for beta electrons. \n");
          excitedState=true;
          maxOccupExtendedMat = {NAEA,a_-NMO};
          numExcitations = {0,1};
          excitations.push_back({i_-NMO,a_-NMO});
        } else {
          APP_ABORT(" Errors: Inconsistent excited orbitals. \n");
        }    
        readWfn(excited_file,excitedOrbMat,NMO,maxOccupExtendedMat.first,maxOccupExtendedMat.second);
      }    
    }

    ~PHMSD() {
        if(req_SMrecv!=MPI_REQUEST_NULL)
            MPI_Request_free(&req_SMrecv);
        if(req_SMsend!=MPI_REQUEST_NULL)
            MPI_Request_free(&req_SMsend);
        if(req_Grecv!=MPI_REQUEST_NULL)
            MPI_Request_free(&req_Grecv);
        if(req_Gsend!=MPI_REQUEST_NULL)
            MPI_Request_free(&req_Gsend);
    }

    PHMSD(PHMSD const& other) = delete;
    PHMSD& operator=(PHMSD const& other) = delete;
    PHMSD(PHMSD&& other) = default;
    PHMSD& operator=(PHMSD&& other) = default;

    int local_number_of_cholesky_vectors() const 
    { return HamOp.local_number_of_cholesky_vectors(); }
    int global_number_of_cholesky_vectors() const 
    { return HamOp.global_number_of_cholesky_vectors(); }
    int global_origin_cholesky_vector() const
    { return HamOp.global_origin_cholesky_vector(); }
    bool distribution_over_cholesky_vectors() const 
    { return HamOp.distribution_over_cholesky_vectors(); }

    int size_of_G_for_vbias() const 
    {  return dm_size(!compact_G_for_vbias);  }

    bool transposed_G_for_vbias() const { return transposed_G_for_vbias_; }
    bool transposed_G_for_E() const { return transposed_G_for_E_; }
    bool transposed_vHS() const { return transposed_vHS_; }
    WALKER_TYPES getWalkerType() const {return walker_type; }

    template<class Vec>
    void vMF(Vec&& v);

    CMatrix getOneBodyPropagatorMatrix(TaskGroup_& TG, CVector const& vMF)
    { return HamOp.getOneBodyPropagatorMatrix(TG,vMF); }

    SlaterDetOperations* getSlaterDetOperations() {return std::addressof(SDetOp);} 

    /*
     * local contribution to vbias for the Green functions in G 
     * G: [size_of_G_for_vbias()][nW]
     * v: [local # Chol. Vectors][nW]
     */
    template<class MatG, class MatA>
    void vbias(const MatG& G, MatA&& v, double a=1.0) {
      assert( v.size(0) == HamOp.local_number_of_cholesky_vectors());
      double scl = (walker_type==COLLINEAR)?0.5:1.0;
      if(transposed_G_for_vbias_) {
        assert( G.size(0) == v.size(1) );
        assert( G.size(1) == size_of_G_for_vbias() );
        HamOp.vbias(G(G.extension(0),{0,long(OrbMats[0].size(0)*NMO)}),
                    std::forward<MatA>(v),scl*a,0.0);
        if(walker_type==COLLINEAR) 
          HamOp.vbias(G(G.extension(0),{long(OrbMats[0].size(0)*NMO),G.size(1)}),
                      std::forward<MatA>(v),scl*a,1.0);
      } else {  
        assert( G.size(0) == size_of_G_for_vbias() );
        assert( G.size(1) == v.size(1) );
        HamOp.vbias(G.sliced(0,OrbMats[0].size(0)*NMO),
                    std::forward<MatA>(v),scl*a,0.0);
        if(walker_type==COLLINEAR) 
          HamOp.vbias(G.sliced(OrbMats[0].size(0)*NMO,G.size(0)),
                      std::forward<MatA>(v),scl*a,1.0);
      }  
      TG.local_barrier();    
    }

    /*
     * local contribution to vHS for the Green functions in G 
     * X: [# chol vecs][nW]
     * v: [NMO^2][nW]
     */
    template<class MatX, class MatA>
    void vHS(MatX&& X, MatA&& v, double a=1.0) {
      assert( X.size(0) == HamOp.local_number_of_cholesky_vectors() );
      if(transposed_vHS_)
        assert( X.size(1) == v.size(0) );
      else    
        assert( X.size(1) == v.size(1) );
      HamOp.vHS(std::forward<MatX>(X),std::forward<MatA>(v),a);
      TG.local_barrier();    
    }

    /*
     * Calculates the local energy and overlaps of all the walkers in the set and stores
     * them in the wset data
     */
    template<class WlkSet>
    void Energy(WlkSet& wset) {
      int nw = wset.size();
      if(ovlp.num_elements() != nw)
        ovlp.reextent(iextensions<1u>{nw});
      if(eloc.size(0) != nw || eloc.size(1) != 3)
        eloc.reextent({nw,3});
      Energy(wset,eloc,ovlp);
      TG.local_barrier();
      if(TG.getLocalTGRank()==0) {
	int p=0;
	for(typename WlkSet::iterator it=wset.begin(); it!=wset.end(); ++it, ++p) {
	  *it->overlap() = ovlp[p];
	  *it->E1() = eloc[p][0];		
	  *it->EXX() = eloc[p][1];		
	  *it->EJ() = eloc[p][2];		
	}
      }  
      TG.local_barrier();
    }

    /*
     * Calculates the local energy and overlaps of all the walkers in the set and 
     * returns them in the appropriate data structures
     */
    template<class WlkSet, class Mat, class TVec> 
    void Energy(const WlkSet& wset, Mat&& E, TVec&& Ov) {
      if(TG.getNGroupsPerTG() > 1)
        Energy_distributed(wset,std::forward<Mat>(E),std::forward<TVec>(Ov));
      else
        Energy_shared(wset,std::forward<Mat>(E),std::forward<TVec>(Ov));
    }

    /*
     * Calculates the mixed density matrix for all walkers in the walker set. 
     * Options:
     *  - compact:   If true (default), returns compact form with Dim: [NEL*NMO], 
     *                 otherwise returns full form with Dim: [NMO*NMO]. 
     *  - transpose: If false (default), returns standard form with Dim: [XXX][nW]
     *                 otherwise returns the transpose with Dim: [nW][XXX}
     */
    template<class WlkSet, class MatG>
    void MixedDensityMatrix(const WlkSet& wset, MatG&& G, bool compact=true, bool transpose=false) {
      int nw = wset.size();
      if(ovlp.num_elements() != nw)
        ovlp.reextent(iextensions<1u>{nw});
      MixedDensityMatrix(wset,std::forward<MatG>(G),ovlp,compact,transpose);
    }

    template<class WlkSet, class MatG, class TVec>
    void MixedDensityMatrix(const WlkSet& wset, MatG&& G, TVec&& Ov, bool compact=true, bool transpose=false);

    template<class WlkSet, class MatG, class CVec1, class CVec2, class Mat1, class Mat2>
    void WalkerAveragedDensityMatrix(const WlkSet& wset, CVec1& wgt, MatG& G, CVec2& denom, Mat1 &&Ovlp, Mat2&& DMsum, bool free_projection=false, boost::multi::array_ref<ComplexType,3>* Refs=nullptr, boost::multi::array<ComplexType,2>* detR=nullptr); 

    /*
     * Calculates the mixed density matrix for all walkers in the walker set
     *   with a format consistent with (and expected by) the vbias routine.
     * This is implementation dependent, so this density matrix should ONLY be used
     * in conjunction with vbias. 
     */
    template<class WlkSet, class MatG>
    void MixedDensityMatrix_for_vbias(const WlkSet& wset, MatG&& G) {
      int nw = wset.size();
      if(ovlp.num_elements() != nw)
        ovlp.reextent(iextensions<1u>{nw});	
      MixedDensityMatrix(wset,std::forward<MatG>(G),ovlp,compact_G_for_vbias,transposed_G_for_vbias_);
    }

    /*
     * Calculates the overlaps of all walkers in the set. Returns values in arrays. 
     */
    template<class WlkSet, class TVec>
    void Overlap(const WlkSet& wset, TVec&& Ov);

    /*
     * Calculates the overlaps of all walkers in the set. Updates values in wset. 
     */
    template<class WlkSet>
    void Overlap(WlkSet& wset)
    {
      int nw = wset.size();
      if(ovlp.num_elements() != nw)
        ovlp.reextent(iextensions<1u>{nw});
      Overlap(wset,ovlp);
      TG.local_barrier();
      if(TG.getLocalTGRank()==0) {
        int p=0;
        for(typename WlkSet::iterator it=wset.begin(); it!=wset.end(); ++it, ++p) 
          *it->overlap() = ovlp[p];
      }	 
      TG.local_barrier();
    }


    /*
     * Orthogonalizes the Slater matrices of all walkers in the set.  
     * Options:
     *  - bool importanceSamplingt(default=true): use algorithm appropriate for importance sampling. 
     *         This means that the determinant of the R matrix in the QR decomposition is ignored.
     *         If false, add the determinant of R to the weight of the walker. 
     */
    template<class WlkSet>
    void Orthogonalize(WlkSet& wset, bool impSamp); 

    /*
     * Orthogonalizes the Slater matrix of a walker in an excited state calculation.
     */
    template<class Mat>
    void OrthogonalizeExcited(Mat&& A, SpinTypes spin, double LogOverlapFactor);

    /*
     * Returns the number of reference Slater Matrices needed for back propagation.  
     */
    int number_of_references_for_back_propagation() const {
      return 0; 
    }

    /*
     * Returns the reference Slater Matrices needed for back propagation.  
     */
    template<class Mat>
    void getReferencesForBackPropagation(Mat&& A) {
      APP_ABORT(" Error: getReferencesForBackPropagation not implemented with PHMSD wavefunctions.\n"); 
    }

    void computeVariationalEnergy(Hamiltonian& ham)
    {
      int ndets = abij.number_of_configurations();
      CMatrix H({ndets,ndets});
      ComplexType numer = ComplexType(0.0);
      ComplexType denom = ComplexType(0.0);
      using std::get;
      auto cnfg_it = abij.configurations_begin();
      for(int idet = 0; idet < ndets; idet++) {
        std::vector<int> deti;
        createDeterminant(idet, deti);
        ComplexType cidet = get<2>(*(cnfg_it+idet));
        for(int jdet = 0; jdet < ndets; jdet++) {
          std::vector<int> detj;
          ComplexType cjdet = get<2>(*(cnfg_it+jdet));
          createDeterminant(jdet, detj);
          int perm = 1;
          std::vector<int> excit;
          int nexcit = getExcitation(deti, detj, excit, perm);
          // Compute <Di|H|Dj>
          if(nexcit == 0) {
            H[idet][jdet] = ComplexType(perm)*slaterCondon0(ham, detj);
          } else if(nexcit == 1) {
            H[idet][jdet] = ComplexType(perm)*slaterCondon1(ham, excit, detj);
          } else if(nexcit == 2) {
            H[idet][jdet] = ComplexType(perm)*slaterCondon2(ham, excit);
          } else {
            H[idet][jdet] = ComplexType(0.0);
          }
          numer += ma::conj(cidet)*cjdet*H[idet][jdet];
        }
        denom += ma::conj(cidet)*cidet;
      }
      app_log() << " Variational energy of trial wavefunction: " << numer / denom << "\n";
      if(recomputeCI) {
        app_log() << " Recomputing CI coefficients.\n";
        std::pair<RVector,CMatrix> Sol = ma::symEig<RVector,CMatrix>(H);
        using std::get;
        app_log() << " Resetting CI coefficients. \n";
        auto cnfg_it = abij.configurations_begin();
        for(int idet=0; idet < ndets; idet++) {
          ComplexType ci = Sol.second[idet][0];
          abij.set_ci_coefficient(idet, ci);
          ComplexType cidet = get<2>(*(cnfg_it+idet));
        }
        app_log() << " Recomputed variational energy of trial wavefunction: " << Sol.first[0] << "\n";
      }

    }

  protected: 

    TaskGroup_& TG;
 
    //SlaterDetOperations_shared<ComplexType> SDetOp;
    SlaterDetOperations SDetOp;
  
    HamiltonianOperations HamOp;

    std::map<int,int> acta2mo;
    std::map<int,int> actb2mo;

    ph_excitations<int,ComplexType> abij;

    // eventually switched from CMatrix to SMHSparseMatrix(node)
    std::vector<PsiT_Matrix> OrbMats;

    std::unique_ptr<shmCVector> shmbuff_for_E;

    std::unique_ptr<shared_mutex> mutex;

    // in both cases below: closed_shell=0, UHF/ROHF=1, GHF=2
    WALKER_TYPES walker_type;

    bool compact_G_for_vbias;

    // in the 3 cases, true means [nwalk][...], false means [...][nwalk]
    bool transposed_G_for_vbias_;
    bool transposed_G_for_E_;
    bool transposed_vHS_;

    ValueType NuclearCoulombEnergy; 

    // not elegant, but reasonable for now
    int last_number_extra_tasks;
    int last_task_index;

    // shared_communicator for parallel work within TG_local()
    //std::unique_ptr<shared_communicator> local_group_comm; 
    shared_communicator local_group_comm; 
    std::unique_ptr<shmCVector> shmbuff_for_G;

    // shared memory arrays for temporary calculations
    bool fast_ph_energy;
    size_t maxn_unique_confg; // maximum number of unque configurations 
    size_t maxnactive;   // maximum number of states in active space
    size_t max_exct_n;   // maximum excitation number (number of electrons excited simultaneously)
    // used by OVerlap and MixedDensityMatrix
    shmCMatrix unique_overlaps;
    shmCMatrix unique_Etot;
    shmCMatrix QQ0inv0;  // Q * inv(Q0) 
    shmCMatrix QQ0inv1;  // Q * inv(Q0) 
    shmCMatrix GA2D0_shm;  
    shmCMatrix GB2D0_shm; 
    boost::multi::array<ComplexType,2> local_ov;
    boost::multi::array<ComplexType,2> local_etot;
    boost::multi::array<ComplexType,2> local_QQ0inv0;
    boost::multi::array<ComplexType,2> local_QQ0inv1;
    boost::multi::array<ComplexType,2> Qwork;     
    boost::multi::array<ComplexType,2> Gwork; 
    // used by Energy_shared 
    boost::multi::array<ComplexType,1> wgt; 
    boost::multi::array<ComplexType,1> opSpinEJ; 
    shmC3Tensor Ovmsd;   // [nspins][maxn_unique_confg][nwalk]
    shmC4Tensor Emsd;    // [nspins][maxn_unique_confg][nwalk][3]
    shmC3Tensor QQ0A;    // [nwalk][NAOA][NAEA]
    shmC3Tensor QQ0B;    // [nwalk][NAOB][NAEB]
    shmC3Tensor GrefA;     // [nwalk][NAOA][NMO]
    shmC3Tensor GrefB;     // [nwalk][NAOB][NMO]
    shmC3Tensor KEright;   
    shmCMatrix KEleft;     
     

    // array of sequence structure storing the list of connected alpha/beta configurations
    std::array<index_aos,2> det_couplings; 

    // excited states
    bool excitedState;
    std::vector<std::pair<int,int>> excitations;
    boost::multi::array<ComplexType,3> excitedOrbMat; 
    CMatrix extendedMatAlpha;
    CMatrix extendedMatBeta;
    std::pair<int,int> maxOccupExtendedMat;
    std::pair<int,int> numExcitations; 

    // Controls whether we recalculate the CI coefficients of the trial wavefunction
    // expansion.
    bool recomputeCI;

    // buffers and work arrays, careful here!!!
    CVector ovlp, localGbuff, ovlp2;
    CMatrix eloc, eloc2, eloc3;

    MPI_Request req_Gsend, req_Grecv;
    MPI_Request req_SMsend, req_SMrecv;

    /*
     * Calculates the local energy and overlaps of all the walkers in the set and 
     * returns them in the appropriate data structures
     */
    template<class WlkSet, class Mat, class TVec>
    void Energy_shared(const WlkSet& wset, Mat&& E, TVec&& Ov);

    /*
     * Calculates the local energy and overlaps of all the walkers in the set and 
     * returns them in the appropriate data structures
     */
    template<class WlkSet, class Mat, class TVec>
    void Energy_distributed(const WlkSet& wset, Mat&& E, TVec&& Ov); 

    int dm_size(bool full) const {
      switch(walker_type) {
        case CLOSED: // closed-shell RHF
          return (full)?(NMO*NMO):(OrbMats[0].size(0)*NMO);
          break;
        case COLLINEAR:
          return (full)?(2*NMO*NMO):((OrbMats[0].size(0)+OrbMats.back().size(0))*NMO);
          break;
        case NONCOLLINEAR:
          return (full)?(4*NMO*NMO):((OrbMats[0].size(0))*2*NMO);
          break;
        default:
          APP_ABORT(" Error: Unknown walker_type in dm_size. \n");
          return -1;
      }
    }
    // dimensions for each component of the DM. 
    std::pair<int,int> dm_dims(bool full, SpinTypes sp=Alpha) const {
      using arr = std::pair<int,int>;
      switch(walker_type) {
        case CLOSED: // closed-shell RHF
          return (full)?(arr{NMO,NMO}):(arr{OrbMats[0].size(0),NMO});
          break;
        case COLLINEAR:
          return (full)?(arr{NMO,NMO}):((sp==Alpha)?(arr{OrbMats[0].size(0),NMO}):(arr{OrbMats.back().size(0),NMO}));
          break;
        case NONCOLLINEAR:
          return (full)?(arr{2*NMO,2*NMO}):(arr{OrbMats[0].size(0),2*NMO});
          break;
        default:
          APP_ABORT(" Error: Unknown walker_type in dm_size. \n");
          return arr{-1,-1};
      }
    }
    std::pair<int,int> dm_dims_ref(bool full, SpinTypes sp=Alpha) const {
      using arr = std::pair<int,int>;
      switch(walker_type) {
        case CLOSED: // closed-shell RHF
          return (full)?(arr{NMO,NMO}):(arr{NAEA,NMO});
          break;
        case COLLINEAR:
          return (full)?(arr{NMO,NMO}):((sp==Alpha)?(arr{NAEA,NMO}):(arr{NAEB,NMO}));
          break;
        case NONCOLLINEAR:
          return (full)?(arr{2*NMO,2*NMO}):(arr{NAEA,2*NMO});
          break;
        default:
          APP_ABORT(" Error: Unknown walker_type in dm_size. \n");
          return arr{-1,-1};
      }
    }

    /**
     * Compute the excitation level between two determinants.
     */
    inline int getExcitation(std::vector<int>& deti, std::vector<int>& detj, std::vector<int>& excit, int& perm)
    {
      std::vector<int> from_orb, to_orb;
      // Work out which orbitals are excited from / to.
      std::set_difference(detj.begin(), detj.end(),
                          deti.begin(), deti.end(),
                          std::inserter(from_orb, from_orb.begin()));
      std::set_difference(deti.begin(), deti.end(),
                          detj.begin(), detj.end(),
                          std::inserter(to_orb, to_orb.begin()));
      int nexcit = from_orb.size();
      if(nexcit <= 2) {
        for(int i = 0; i < from_orb.size(); i++)
          excit.push_back(from_orb[i]);
        for(int i = 0; i < to_orb.size(); i++)
          excit.push_back(to_orb[i]);
        int nperm = 0;
        int nmove = 0;
        for(auto o : from_orb) {
          auto it = std::find(detj.begin(), detj.end(), o);
          int loc = std::distance(detj.begin(), it);
          nperm += detj.size() - loc - 1 + nmove;
          nmove += 1;
        }
        nmove = 0;
        for(auto o : to_orb) {
          auto it = std::find(deti.begin(), deti.end(), o);
          int loc = std::distance(deti.begin(), it);
          nperm += deti.size() - loc - 1 + nmove;
          nmove += 1;
        }
        perm = nperm%2 == 1 ? -1 : 1;
      }
      return nexcit;
    }

    inline int decodeSpinOrbital(int spinOrb, int& spin)
    {
      spin = spinOrb%2==0 ? 0 : 1;
      return spin ? (spinOrb-1) / 2 : spinOrb / 2;
    }

    inline ComplexType slaterCondon0(Hamiltonian& ham, std::vector<int>& det)
    {
      ComplexType oneBody = ComplexType(0.0), twoBody = ComplexType(0.0);
      auto H1 = ham.getH1();
      int spini, spinj;
      for(auto i : det) {
        int oi = decodeSpinOrbital(i, spini);
        oneBody += H1[oi][oi];
        for(auto j : det) {
          int oj = decodeSpinOrbital(j, spinj);
          twoBody += ham.H(oi,oj,oi,oj);
          if(spini == spinj) twoBody -= ham.H(oi,oj,oj,oi);
        }
      }
      return oneBody + 0.5 * twoBody;
    }

    inline ComplexType slaterCondon1(Hamiltonian& ham, std::vector<int>& excit, std::vector<int>& det)
    {
      int spini, spina;
      int oi = decodeSpinOrbital(excit[0], spini);
      int oa = decodeSpinOrbital(excit[1], spina);
      auto H1 = ham.getH1();
      ComplexType oneBody = H1[oi][oa];
      ComplexType twoBody = ComplexType(0.0);
      for(auto j : det) {
        int spinj;
        int oj = decodeSpinOrbital(j, spinj);
        if(j != excit[0]) {
          twoBody += ham.H(oi,oj,oa,oj);
          if(spini == spinj)
            twoBody -= ham.H(oi,oj,oj,oa);
        }
      }
      return oneBody + twoBody;
    }

    inline ComplexType slaterCondon2(Hamiltonian& ham, std::vector<int>& excit)
    {
      ComplexType twoBody = ComplexType(0.0);
      int spini, spinj, spina, spinb;
      int oi = decodeSpinOrbital(excit[0], spini);
      int oj = decodeSpinOrbital(excit[1], spinj);
      int oa = decodeSpinOrbital(excit[2], spina);
      int ob = decodeSpinOrbital(excit[3], spinb);
      if(spini == spina)
        twoBody = ham.H(oi,oj,oa,ob);
      if(spini == spinb)
        twoBody -= ham.H(oi,oj,ob,oa);
      return twoBody;
    }

    inline void createDeterminant(int idet, std::vector<int>& det)
    {
      using std::get;
      auto cit = abij.configurations_begin() + idet;
      int alpha_ix = get<0>(*cit);
      int beta_ix = get<1>(*cit);
      auto ci = get<2>(*cit);
      std::vector<int> occa(NAEA), occb(NAEB);
      abij.get_configuration(0, alpha_ix, occa);
      abij.get_configuration(1, beta_ix, occb);
      for(auto i : occa)
        det.push_back(2*i);
      for(auto i : occb)
        det.push_back(2*i+1);
      std::sort(det.begin(), det.end());
    }

    ComplexType contractOneBody(std::vector<int>& det, std::vector<int>& excit, CMatrix_ref& HSPot)
    {
      ComplexType oneBody = ComplexType(0.0);
      int spini, spina;
      if(excit.size()==0) {
        for(auto i : det) {
          int oi = decodeSpinOrbital(i, spini);
          oneBody += HSPot[oi][oi];
        }
      } else {
        int oi = decodeSpinOrbital(excit[0], spini);
        int oa = decodeSpinOrbital(excit[1], spina);
        oneBody = HSPot[oi][oa];
      }
      return oneBody;
    }

};

}

}

#include "AFQMC/Wavefunctions/PHMSD.icc"

#endif

