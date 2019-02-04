// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/// duplicate ceph::buffer implementation within this unit
#include "common/buffer.cc"
#include "librados/librados_cxx.h"

/// symbol versioning helpers for ceph::buffer
#define CEPH_BUFFER_NS_API(fn)                            \
  LIBRADOS_CXX_API(_ZN4ceph6buffer, fn)
#define CEPH_BUFFER_NS_API_CONST(fn)                      \
  LIBRADOS_CXX_API(_ZNK4ceph6buffer, fn)

CEPH_BUFFER_NS_API(12claim_bufferEjPc7deleter);
CEPH_BUFFER_NS_API(10claim_charEjPc);
CEPH_BUFFER_NS_API(12claim_mallocEjPc);
CEPH_BUFFER_NS_API(4copyEPKcj);
CEPH_BUFFER_NS_API(25create_aligned_in_mempoolEjji);
CEPH_BUFFER_NS_API(14create_alignedEjj);
CEPH_BUFFER_NS_API(17create_in_mempoolEji);
CEPH_BUFFER_NS_API(13create_mallocEj);
CEPH_BUFFER_NS_API(19create_page_alignedEj);
CEPH_BUFFER_NS_API(13create_staticEjPc);
CEPH_BUFFER_NS_API(18create_unshareableEj);
CEPH_BUFFER_NS_API(6createEj);
CEPH_BUFFER_NS_API(16create_zero_copyEjiPl);
CEPH_BUFFER_NS_API(23get_cached_crc_adjustedEv);
CEPH_BUFFER_NS_API(14get_cached_crcEv);
CEPH_BUFFER_NS_API(18get_c_str_accessesEv);
CEPH_BUFFER_NS_API(23get_history_alloc_bytesEv);
CEPH_BUFFER_NS_API(21get_history_alloc_numEv);
CEPH_BUFFER_NS_API(14get_missed_crcEv);
CEPH_BUFFER_NS_API(15get_total_allocEv);
CEPH_BUFFER_NS_API(16track_cached_crcEb);
CEPH_BUFFER_NS_API(11track_c_strEb);
CEPH_BUFFER_NS_API(lsERSoRKNS0_5errorE);
CEPH_BUFFER_NS_API(lsERSoRKNS0_4listE);
CEPH_BUFFER_NS_API(lsERSoRKNS0_3ptrE);
CEPH_BUFFER_NS_API(lsERSoRKNS0_3rawE);
CEPH_BUFFER_NS_API_CONST(9bad_alloc4whatEv);
CEPH_BUFFER_NS_API_CONST(13end_of_buffer4whatEv);
CEPH_BUFFER_NS_API(10error_codeC1Ei);
CEPH_BUFFER_NS_API(10error_codeC2Ei);
CEPH_BUFFER_NS_API_CONST(5error4whatEv);
CEPH_BUFFER_NS_API_CONST(15malformed_input4whatEv);


/// symbol versioning helpers for ceph::buffer::ptr
#define CEPH_BUFFER_PTR_API(fn)                           \
  LIBRADOS_CXX_API(_ZN4ceph6buffer3ptr, fn)
#define CEPH_BUFFER_PTR_API_CONST(fn)                     \
  LIBRADOS_CXX_API(_ZNK4ceph6buffer3ptr, fn)

CEPH_BUFFER_PTR_API(3ptrC1ERKS1_jj);
CEPH_BUFFER_PTR_API(3ptrC2ERKS1_jj);
CEPH_BUFFER_PTR_API(3ptrC1ERKS1_);
CEPH_BUFFER_PTR_API(3ptrC2ERKS1_);
CEPH_BUFFER_PTR_API(3ptrC1EOS1_);
CEPH_BUFFER_PTR_API(3ptrC2EOS1_);
CEPH_BUFFER_PTR_API(3ptrC1EPNS0_3rawE);
CEPH_BUFFER_PTR_API(3ptrC2EPNS0_3rawE);
CEPH_BUFFER_PTR_API(3ptrC1EPKcj);
CEPH_BUFFER_PTR_API(3ptrC2EPKcj);
CEPH_BUFFER_PTR_API(3ptrC1Ej);
CEPH_BUFFER_PTR_API(3ptrC2Ej);
CEPH_BUFFER_PTR_API(3ptr6appendEPKcj);
CEPH_BUFFER_PTR_API(3ptr6appendEc);
CEPH_BUFFER_PTR_API_CONST(14at_buffer_tailEv);
CEPH_BUFFER_PTR_API_CONST(13can_zero_copyEv);
CEPH_BUFFER_PTR_API(3ptr5cloneEv);
CEPH_BUFFER_PTR_API_CONST(3cmpERKS1_);
CEPH_BUFFER_PTR_API(3ptr7copy_inEjjPKcb);
CEPH_BUFFER_PTR_API(3ptr7copy_inEjjPKc);
CEPH_BUFFER_PTR_API_CONST(8copy_outEjjPc);
CEPH_BUFFER_PTR_API_CONST(5c_strEv);
CEPH_BUFFER_PTR_API(3ptr5c_strEv);
CEPH_BUFFER_PTR_API_CONST(9end_c_strEv);
CEPH_BUFFER_PTR_API(3ptr9end_c_strEv);
CEPH_BUFFER_PTR_API_CONST(11get_mempoolEv);
CEPH_BUFFER_PTR_API_CONST(7is_zeroEv);
CEPH_BUFFER_PTR_API(3ptr14make_shareableEv);
CEPH_BUFFER_PTR_API(3ptraSERKS1_);
CEPH_BUFFER_PTR_API(3ptraSEOS1_);
CEPH_BUFFER_PTR_API_CONST(ixEj);
CEPH_BUFFER_PTR_API(3ptrixEj);
CEPH_BUFFER_PTR_API_CONST(9raw_c_strEv);
CEPH_BUFFER_PTR_API_CONST(10raw_lengthEv);
CEPH_BUFFER_PTR_API_CONST(8raw_nrefEv);
CEPH_BUFFER_PTR_API(3ptr19reassign_to_mempoolEi);
CEPH_BUFFER_PTR_API(3ptr7releaseEv);
CEPH_BUFFER_PTR_API(3ptr4swapERS1_);
CEPH_BUFFER_PTR_API(3ptr21try_assign_to_mempoolEi);
CEPH_BUFFER_PTR_API_CONST(18unused_tail_lengthEv);
CEPH_BUFFER_PTR_API_CONST(6wastedEv);
CEPH_BUFFER_PTR_API(3ptr4zeroEb);
CEPH_BUFFER_PTR_API(3ptr4zeroEjjb);
CEPH_BUFFER_PTR_API(3ptr4zeroEjj);
CEPH_BUFFER_PTR_API(3ptr4zeroEv);
CEPH_BUFFER_PTR_API_CONST(15zero_copy_to_fdEiPl);


/// symbol versioning helpers for ceph::buffer::list
#define CEPH_BUFFER_LIST_API(fn)                          \
  LIBRADOS_CXX_API(_ZN4ceph6buffer4list, fn)
#define CEPH_BUFFER_LIST_API_CONST(fn)                    \
  LIBRADOS_CXX_API(_ZNK4ceph6buffer4list, fn)

CEPH_BUFFER_LIST_API(C1EOS1_);
CEPH_BUFFER_LIST_API(C2EOS1_);
CEPH_BUFFER_LIST_API(6appendERKS1_);
CEPH_BUFFER_LIST_API(6appendERKNS0_3ptrEjj);
CEPH_BUFFER_LIST_API(6appendERKNS0_3ptrE);
CEPH_BUFFER_LIST_API(6appendEONS0_3ptrE);
CEPH_BUFFER_LIST_API(6appendEPKcj);
CEPH_BUFFER_LIST_API(6appendEc);
CEPH_BUFFER_LIST_API(6appendERSi);
CEPH_BUFFER_LIST_API(11append_zeroEj);
CEPH_BUFFER_LIST_API_CONST(13can_zero_copyEv);
CEPH_BUFFER_LIST_API_CONST(13can_zero_copyEv);
CEPH_BUFFER_LIST_API(12claim_appendERS1_j);
CEPH_BUFFER_LIST_API(22claim_append_piecewiseERS1_);
CEPH_BUFFER_LIST_API(5claimERS1_j);
CEPH_BUFFER_LIST_API(13claim_prependERS1_j);
CEPH_BUFFER_LIST_API(13claim_prependERS1_j);
CEPH_BUFFER_LIST_API_CONST(14contents_equalERKS1_);
CEPH_BUFFER_LIST_API(14contents_equalERS1_);
CEPH_BUFFER_LIST_API(14contents_equalERS1_);
CEPH_BUFFER_LIST_API(7copy_inEjjRKS1_);
CEPH_BUFFER_LIST_API(7copy_inEjjPKcb);
CEPH_BUFFER_LIST_API(7copy_inEjjPKc);
CEPH_BUFFER_LIST_API_CONST(4copyEjjRS1_);
CEPH_BUFFER_LIST_API_CONST(4copyEjjPc);
CEPH_BUFFER_LIST_API_CONST(4copyEjjRNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
CEPH_BUFFER_LIST_API_CONST(4copyEjjRSs);
CEPH_BUFFER_LIST_API_CONST(6crc32cEj);
CEPH_BUFFER_LIST_API(5c_strEv);
CEPH_BUFFER_LIST_API(13decode_base64ERS1_);
CEPH_BUFFER_LIST_API(13encode_base64ERS1_);
CEPH_BUFFER_LIST_API(14get_contiguousEjj);
CEPH_BUFFER_LIST_API(14get_contiguousEjj);
CEPH_BUFFER_LIST_API_CONST(11get_mempoolEv);
CEPH_BUFFER_LIST_API_CONST(16get_wasted_spaceEv);
CEPH_BUFFER_LIST_API_CONST(7hexdumpERSob);
CEPH_BUFFER_LIST_API(14invalidate_crcEv);
CEPH_BUFFER_LIST_API_CONST(26is_aligned_size_and_memoryEjj);
CEPH_BUFFER_LIST_API_CONST(10is_alignedEj);
CEPH_BUFFER_LIST_API_CONST(13is_contiguousEv);
CEPH_BUFFER_LIST_API_CONST(16is_n_align_sizedEj);
CEPH_BUFFER_LIST_API_CONST(15is_n_page_sizedEv);
CEPH_BUFFER_LIST_API_CONST(15is_page_alignedEv);
CEPH_BUFFER_LIST_API_CONST(18is_provided_bufferEPKc);
CEPH_BUFFER_LIST_API_CONST(7is_zeroEv);
CEPH_BUFFER_LIST_API_CONST(ixEj);
CEPH_BUFFER_LIST_API(12prepend_zeroEj);
CEPH_BUFFER_LIST_API(7read_fdEim);
CEPH_BUFFER_LIST_API(17read_fd_zero_copyEim);
CEPH_BUFFER_LIST_API(9read_fileEPKcPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
CEPH_BUFFER_LIST_API(9read_fileEPKcPSs);
CEPH_BUFFER_LIST_API(19reassign_to_mempoolEi);
CEPH_BUFFER_LIST_API(31rebuild_aligned_size_and_memoryEjjj);
CEPH_BUFFER_LIST_API(15rebuild_alignedEj);
CEPH_BUFFER_LIST_API(7rebuildERNS0_3ptrE);
CEPH_BUFFER_LIST_API(20rebuild_page_alignedEv);
CEPH_BUFFER_LIST_API(7rebuildEv);
CEPH_BUFFER_LIST_API(7reserveEm);
CEPH_BUFFER_LIST_API(6spliceEjjPS1_);
CEPH_BUFFER_LIST_API(19static_from_cstringEPc);
CEPH_BUFFER_LIST_API(15static_from_memEPcm);
CEPH_BUFFER_LIST_API(18static_from_stringERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
CEPH_BUFFER_LIST_API(18static_from_stringERSs);
CEPH_BUFFER_LIST_API(9substr_ofERKS1_jj);
CEPH_BUFFER_LIST_API(4swapERS1_);
CEPH_BUFFER_LIST_API_CONST(6to_strB5cxx11Ev);
CEPH_BUFFER_LIST_API(21try_assign_to_mempoolEi);
CEPH_BUFFER_LIST_API_CONST(8write_fdEi);
CEPH_BUFFER_LIST_API_CONST(8write_fdEim);
CEPH_BUFFER_LIST_API_CONST(18write_fd_zero_copyEi);
CEPH_BUFFER_LIST_API(10write_fileEPKci);
CEPH_BUFFER_LIST_API_CONST(5writeEiiRSo);
CEPH_BUFFER_LIST_API_CONST(12write_streamERSo);
CEPH_BUFFER_LIST_API(4zeroEjj);
CEPH_BUFFER_LIST_API(4zeroEv);
CEPH_BUFFER_LIST_API(8iteratorC1EPS1_jSt14_List_iteratorINS0_3ptrEEj);
CEPH_BUFFER_LIST_API(8iteratorC2EPS1_jSt14_List_iteratorINS0_3ptrEEj);
CEPH_BUFFER_LIST_API(8iteratorC1EPS1_j);
CEPH_BUFFER_LIST_API(8iteratorC2EPS1_j);
CEPH_BUFFER_LIST_API(8iterator7advanceEi);
CEPH_BUFFER_LIST_API(8iterator7advanceEi);
CEPH_BUFFER_LIST_API(8iterator8copy_allERS1_);
CEPH_BUFFER_LIST_API(8iterator9copy_deepEjRNS0_3ptrE);
CEPH_BUFFER_LIST_API(8iterator7copy_inEjRS1_);
CEPH_BUFFER_LIST_API(8iterator7copy_inEjRKS1_);
CEPH_BUFFER_LIST_API(8iterator7copy_inEjPKcb);
CEPH_BUFFER_LIST_API(8iterator7copy_inEjPKc);
CEPH_BUFFER_LIST_API(8iterator12copy_shallowEjRNS0_3ptrE);
CEPH_BUFFER_LIST_API(8iterator4copyEjRS1_);
CEPH_BUFFER_LIST_API(8iterator4copyEjRNS0_3ptrE);
CEPH_BUFFER_LIST_API(8iterator4copyEjPc);
CEPH_BUFFER_LIST_API(8iterator4copyEjRNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
CEPH_BUFFER_LIST_API(8iterator4copyEjRSs);
CEPH_BUFFER_LIST_API(8iterator15get_current_ptrEv);
CEPH_BUFFER_LIST_API(8iteratordeEv);
CEPH_BUFFER_LIST_API(8iteratorppEv);
CEPH_BUFFER_LIST_API(8iterator4seekEj);
