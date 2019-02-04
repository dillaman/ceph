// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <limits.h>

#include "common/config.h"
#include "common/errno.h"
#include "common/ceph_argparse.h"
#include "common/ceph_json.h"
#include "common/common_init.h"
#include "common/TracepointProvider.h"
#include "common/hobject.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "include/types.h"
#include <include/stringify.h>

#include "librados/librados_c.h"
#include "librados/librados_cxx.h"
#include "librados/AioCompletionImpl.h"
#include "librados/IoCtxImpl.h"
#include "librados/PoolAsyncCompletionImpl.h"
#include "librados/RadosClient.h"
#include "librados/RadosXattrIter.h"
#include "librados/ListObjectImpl.h"
#include "librados/librados_util.h"
#include "cls/lock/cls_lock_client.h"

#include <string>
#include <map>
#include <set>
#include <vector>
#include <list>
#include <stdexcept>
#include <system_error>

#ifdef WITH_LTTNG
#define TRACEPOINT_DEFINE
#define TRACEPOINT_PROBE_DYNAMIC_LINKAGE
#include "tracing/librados.h"
#undef TRACEPOINT_PROBE_DYNAMIC_LINKAGE
#undef TRACEPOINT_DEFINE
#else
#define tracepoint(...)
#endif

using std::string;
using std::map;
using std::set;
using std::vector;
using std::list;

#define dout_subsys ceph_subsys_rados
#undef dout_prefix
#define dout_prefix *_dout << "librados: "

static TracepointProvider::Traits tracepoint_traits("librados_tp.so", "rados_tracing");

/*
 * Structure of this file
 *
 * RadosClient and the related classes are the internal implementation of librados.
 * Above that layer sits the C API, found in include/rados/librados.h, and
 * the C++ API, found in include/rados/librados.hpp
 *
 * The C++ API sometimes implements things in terms of the C API.
 * Both the C++ and C API rely on RadosClient.
 *
 * Visually:
 * +--------------------------------------+
 * |             C++ API                  |
 * +--------------------+                 |
 * |       C API        |                 |
 * +--------------------+-----------------+
 * |          RadosClient                 |
 * +--------------------------------------+
 */

namespace librados {

struct ObjectOperationImpl {
  ::ObjectOperation o;
  real_time rt;
  real_time *prt;

  ObjectOperationImpl() : prt(NULL) {}
};

}

/// symbol versioning helpers for librados::ObjectOperation
#define LIBRADOS_OBJECT_OPERATION_API(fn)                 \
  LIBRADOS_CXX_API(_ZN8librados15ObjectOperation, fn)
size_t librados::ObjectOperation::size()
{
  ::ObjectOperation *o = &impl->o;
  return o->size();
}
LIBRADOS_OBJECT_OPERATION_API(4sizeEv);

//deprcated
void librados::ObjectOperation::set_op_flags(ObjectOperationFlags flags)
{
  set_op_flags2((int)flags);
}
LIBRADOS_OBJECT_OPERATION_API(12set_op_flagsENS_20ObjectOperationFlagsE);

void librados::ObjectOperation::set_op_flags2(int flags)
{
  impl->o.set_last_op_flags(get_op_flags(flags));
}
LIBRADOS_OBJECT_OPERATION_API(13set_op_flags2Ei);

void librados::ObjectOperation::cmpext(uint64_t off,
                                       bufferlist &cmp_bl,
                                       int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->cmpext(off, cmp_bl, prval);
}
LIBRADOS_OBJECT_OPERATION_API(6cmpextEmRN4ceph6buffer4listEPi);

void librados::ObjectOperation::cmpxattr(const char *name, uint8_t op, const bufferlist& v)
{
  ::ObjectOperation *o = &impl->o;
  o->cmpxattr(name, op, CEPH_OSD_CMPXATTR_MODE_STRING, v);
}
LIBRADOS_OBJECT_OPERATION_API(8cmpxattrEPKchRKN4ceph6buffer4listE);

void librados::ObjectOperation::cmpxattr(const char *name, uint8_t op, uint64_t v)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist bl;
  encode(v, bl);
  o->cmpxattr(name, op, CEPH_OSD_CMPXATTR_MODE_U64, bl);
}
LIBRADOS_OBJECT_OPERATION_API(8cmpxattrEPKchm);

void librados::ObjectOperation::assert_version(uint64_t ver)
{
  ::ObjectOperation *o = &impl->o;
  o->assert_version(ver);
}
LIBRADOS_OBJECT_OPERATION_API(14assert_versionEm);

void librados::ObjectOperation::assert_exists()
{
  ::ObjectOperation *o = &impl->o;
  o->stat(NULL, (ceph::real_time*) NULL, NULL);
}
LIBRADOS_OBJECT_OPERATION_API(13assert_existsEv);

void librados::ObjectOperation::exec(const char *cls, const char *method, bufferlist& inbl)
{
  ::ObjectOperation *o = &impl->o;
  o->call(cls, method, inbl);
}
LIBRADOS_OBJECT_OPERATION_API(4execEPKcS2_RN4ceph6buffer4listE);

void librados::ObjectOperation::exec(const char *cls, const char *method, bufferlist& inbl, bufferlist *outbl, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->call(cls, method, inbl, outbl, NULL, prval);
}
LIBRADOS_OBJECT_OPERATION_API(4execEPKcS2_RN4ceph6buffer4listEPS5_Pi);

class ObjectOpCompletionCtx : public Context {
  librados::ObjectOperationCompletion *completion;
  bufferlist bl;
public:
  explicit ObjectOpCompletionCtx(librados::ObjectOperationCompletion *c) : completion(c) {}
  void finish(int r) override {
    completion->handle_completion(r, bl);
    delete completion;
  }

  bufferlist *outbl() {
    return &bl;
  }
};

void librados::ObjectOperation::exec(const char *cls, const char *method, bufferlist& inbl, librados::ObjectOperationCompletion *completion)
{
  ::ObjectOperation *o = &impl->o;

  ObjectOpCompletionCtx *ctx = new ObjectOpCompletionCtx(completion);

  o->call(cls, method, inbl, ctx->outbl(), ctx, NULL);
}
LIBRADOS_OBJECT_OPERATION_API(4execEPKcS2_RN4ceph6buffer4listEPNS_25ObjectOperationCompletionE);

/// symbol versioning helpers for librados::ObjectReadOperation
#define LIBRADOS_OBJECT_READ_OPERATION_API(fn)          \
  LIBRADOS_CXX_API(_ZN8librados19ObjectReadOperation, fn)

void librados::ObjectReadOperation::stat(uint64_t *psize, time_t *pmtime, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->stat(psize, pmtime, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(4statEPmPlPi);

void librados::ObjectReadOperation::stat2(uint64_t *psize, struct timespec *pts, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->stat(psize, pts, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(5stat2EPmP8timespecPi);

void librados::ObjectReadOperation::read(size_t off, uint64_t len, bufferlist *pbl, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->read(off, len, pbl, prval, NULL);
}
LIBRADOS_OBJECT_READ_OPERATION_API(4readEmmPN4ceph6buffer4listEPi);

void librados::ObjectReadOperation::sparse_read(uint64_t off, uint64_t len,
						std::map<uint64_t,uint64_t> *m,
						bufferlist *data_bl, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->sparse_read(off, len, m, data_bl, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(11sparse_readEmmPSt3mapImmSt4lessImESaISt4pairIKmmEEEPN4ceph6buffer4listEPi);

void librados::ObjectReadOperation::checksum(rados_checksum_type_t type,
					     const bufferlist &init_value_bl,
					     uint64_t off, size_t len,
					     size_t chunk_size, bufferlist *pbl,
					     int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->checksum(get_checksum_op_type(type), init_value_bl, off, len, chunk_size,
	      pbl, prval, nullptr);
}
LIBRADOS_OBJECT_READ_OPERATION_API(8checksumE21rados_checksum_type_tRKN4ceph6buffer4listEmmmPS4_Pi);

void librados::ObjectReadOperation::getxattr(const char *name, bufferlist *pbl, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->getxattr(name, pbl, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(8getxattrEPKcPN4ceph6buffer4listEPi);

void librados::ObjectReadOperation::omap_get_vals(
  const std::string &start_after,
  const std::string &filter_prefix,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_vals(start_after, filter_prefix, max_return, out_vals, nullptr,
		   prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_valsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_valsERKSsS2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPi);

void librados::ObjectReadOperation::omap_get_vals2(
  const std::string &start_after,
  const std::string &filter_prefix,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  bool *pmore,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_vals(start_after, filter_prefix, max_return, out_vals, pmore,
		   prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_vals2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPbPi);
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_vals2ERKSsS2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPbPi);

void librados::ObjectReadOperation::omap_get_vals(
  const std::string &start_after,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_vals(start_after, "", max_return, out_vals, nullptr, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_valsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_valsERKSsmPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPi);

void librados::ObjectReadOperation::omap_get_vals2(
  const std::string &start_after,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  bool *pmore,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_vals(start_after, "", max_return, out_vals, pmore, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_vals2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPbPi);
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_vals2ERKSsmPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPbPi);

void librados::ObjectReadOperation::omap_get_keys(
  const std::string &start_after,
  uint64_t max_return,
  std::set<std::string> *out_keys,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_keys(start_after, max_return, out_keys, nullptr, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_keysERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmPSt3setIS6_St4lessIS6_ESaIS6_EEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(13omap_get_keysERKSsmPSt3setISsSt4lessISsESaISsEEPi);

void librados::ObjectReadOperation::omap_get_keys2(
  const std::string &start_after,
  uint64_t max_return,
  std::set<std::string> *out_keys,
  bool *pmore,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_keys(start_after, max_return, out_keys, pmore, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_keys2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmPSt3setIS6_St4lessIS6_ESaIS6_EEPbPi);
LIBRADOS_OBJECT_READ_OPERATION_API(14omap_get_keys2ERKSsmPSt3setISsSt4lessISsESaISsEEPbPi);

void librados::ObjectReadOperation::omap_get_header(bufferlist *bl, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_header(bl, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(15omap_get_headerEPN4ceph6buffer4listEPi);

void librados::ObjectReadOperation::omap_get_vals_by_keys(
  const std::set<std::string> &keys,
  std::map<std::string, bufferlist> *map,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_get_vals_by_keys(keys, map, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(21omap_get_vals_by_keysERKSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS7_ESaIS7_EEPSt3mapIS7_N4ceph6buffer4listES9_SaISt4pairIKS7_SH_EEEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(21omap_get_vals_by_keysERKSt3setISsSt4lessISsESaISsEEPSt3mapISsN4ceph6buffer4listES3_SaISt4pairIKSsSB_EEEPi);

void librados::ObjectOperation::omap_cmp(
  const std::map<std::string, pair<bufferlist, int> > &assertions,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_cmp(assertions, prval);
}
LIBRADOS_OBJECT_OPERATION_API(8omap_cmpERKSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4pairIN4ceph6buffer4listEiESt4lessIS7_ESaIS8_IKS7_SC_EEEPi);
LIBRADOS_OBJECT_OPERATION_API(8omap_cmpERKSt3mapISsSt4pairIN4ceph6buffer4listEiESt4lessISsESaIS2_IKSsS6_EEEPi);

void librados::ObjectReadOperation::list_watchers(
  list<obj_watch_t> *out_watchers,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->list_watchers(out_watchers, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(13list_watchersEPNSt7__cxx114listI11obj_watch_tSaIS3_EEEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(13list_watchersEPSt4listI11obj_watch_tSaIS2_EEPi);

void librados::ObjectReadOperation::list_snaps(
  snap_set_t *out_snaps,
  int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->list_snaps(out_snaps, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(10list_snapsEPNS_10snap_set_tEPi);

void librados::ObjectReadOperation::is_dirty(bool *is_dirty, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->is_dirty(is_dirty, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(8is_dirtyEPbPi);

void librados::ObjectReadOperation::getxattrs(map<string, bufferlist> *pattrs, int *prval)
{
  ::ObjectOperation *o = &impl->o;
  o->getxattrs(pattrs, prval);
}
LIBRADOS_OBJECT_READ_OPERATION_API(9getxattrsEPSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEN4ceph6buffer4listESt4lessIS7_ESaISt4pairIKS7_SA_EEEPi);
LIBRADOS_OBJECT_READ_OPERATION_API(9getxattrsEPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIKSsS4_EEEPi);

/// symbol versioning helpers for librados::ObjectWriteOperation
#define LIBRADOS_OBJECT_WRITE_OPERATION_API(fn)          \
  LIBRADOS_CXX_API(_ZN8librados20ObjectWriteOperation, fn)

void librados::ObjectWriteOperation::mtime(time_t *pt)
{
  if (pt) {
    impl->rt = ceph::real_clock::from_time_t(*pt);
    impl->prt = &impl->rt;
  }
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(5mtimeEPl);

void librados::ObjectWriteOperation::mtime2(struct timespec *pts)
{
  if (pts) {
    impl->rt = ceph::real_clock::from_timespec(*pts);
    impl->prt = &impl->rt;
  }
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(6mtime2EP8timespec);

void librados::ObjectWriteOperation::create(bool exclusive)
{
  ::ObjectOperation *o = &impl->o;
  o->create(exclusive);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(6createEb);

void librados::ObjectWriteOperation::create(bool exclusive,
					    const std::string& category) // unused
{
  ::ObjectOperation *o = &impl->o;
  o->create(exclusive);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(6createEbRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_OBJECT_WRITE_OPERATION_API(6createEbRKSs);

void librados::ObjectWriteOperation::write(uint64_t off, const bufferlist& bl)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist c = bl;
  o->write(off, c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(5writeEmRKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::write_full(const bufferlist& bl)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist c = bl;
  o->write_full(c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(10write_fullERKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::writesame(uint64_t off, uint64_t write_len,
					       const bufferlist& bl)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist c = bl;
  o->writesame(off, write_len, c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(9writesameEmmRKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::append(const bufferlist& bl)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist c = bl;
  o->append(c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(6appendERKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::remove()
{
  ::ObjectOperation *o = &impl->o;
  o->remove();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(6removeEv);

void librados::ObjectWriteOperation::truncate(uint64_t off)
{
  ::ObjectOperation *o = &impl->o;
  o->truncate(off);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(8truncateEm);

void librados::ObjectWriteOperation::zero(uint64_t off, uint64_t len)
{
  ::ObjectOperation *o = &impl->o;
  o->zero(off, len);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(4zeroEmm);

void librados::ObjectWriteOperation::rmxattr(const char *name)
{
  ::ObjectOperation *o = &impl->o;
  o->rmxattr(name);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(7rmxattrEPKc);

void librados::ObjectWriteOperation::setxattr(const char *name, const bufferlist& v)
{
  ::ObjectOperation *o = &impl->o;
  o->setxattr(name, v);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(8setxattrEPKcRKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::setxattr(const char *name,
					      const buffer::list&& v)
{
  ::ObjectOperation *o = &impl->o;
  o->setxattr(name, std::move(v));
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(8setxattrEPKcOKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::omap_set(
  const map<string, bufferlist> &map)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_set(map);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(8omap_setERKSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEN4ceph6buffer4listESt4lessIS7_ESaISt4pairIKS7_SA_EEE);
LIBRADOS_OBJECT_WRITE_OPERATION_API(8omap_setERKSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIKSsS4_EEE);

void librados::ObjectWriteOperation::omap_set_header(const bufferlist &bl)
{
  bufferlist c = bl;
  ::ObjectOperation *o = &impl->o;
  o->omap_set_header(c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(15omap_set_headerERKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::omap_clear()
{
  ::ObjectOperation *o = &impl->o;
  o->omap_clear();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(10omap_clearEv);

void librados::ObjectWriteOperation::omap_rm_keys(
  const std::set<std::string> &to_rm)
{
  ::ObjectOperation *o = &impl->o;
  o->omap_rm_keys(to_rm);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(12omap_rm_keysERKSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS7_ESaIS7_EE);
LIBRADOS_OBJECT_WRITE_OPERATION_API(12omap_rm_keysERKSt3setISsSt4lessISsESaISsEE);

void librados::ObjectWriteOperation::copy_from(const std::string& src,
					       const IoCtx& src_ioctx,
					       uint64_t src_version,
					       uint32_t src_fadvise_flags)
{
  ::ObjectOperation *o = &impl->o;
  o->copy_from(object_t(src), src_ioctx.io_ctx_impl->snap_seq,
	       src_ioctx.io_ctx_impl->oloc, src_version, 0, src_fadvise_flags);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(9copy_fromERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNS_5IoCtxEmj);
LIBRADOS_OBJECT_WRITE_OPERATION_API(9copy_fromERKSsRKNS_5IoCtxEmj);

void librados::ObjectWriteOperation::undirty()
{
  ::ObjectOperation *o = &impl->o;
  o->undirty();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(7undirtyEv);

void librados::ObjectReadOperation::cache_flush()
{
  ::ObjectOperation *o = &impl->o;
  o->cache_flush();
}
LIBRADOS_OBJECT_READ_OPERATION_API(11cache_flushEv);

void librados::ObjectReadOperation::cache_try_flush()
{
  ::ObjectOperation *o = &impl->o;
  o->cache_try_flush();
}
LIBRADOS_OBJECT_READ_OPERATION_API(15cache_try_flushEv);

void librados::ObjectReadOperation::cache_evict()
{
  ::ObjectOperation *o = &impl->o;
  o->cache_evict();
}
LIBRADOS_OBJECT_READ_OPERATION_API(11cache_evictEv);

void librados::ObjectWriteOperation::set_redirect(const std::string& tgt_obj, 
						  const IoCtx& tgt_ioctx,
						  uint64_t tgt_version,
						  int flag)
{
  ::ObjectOperation *o = &impl->o;
  o->set_redirect(object_t(tgt_obj), tgt_ioctx.io_ctx_impl->snap_seq,
			  tgt_ioctx.io_ctx_impl->oloc, tgt_version, flag);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(12set_redirectERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKNS_5IoCtxEmi);
LIBRADOS_OBJECT_WRITE_OPERATION_API(12set_redirectERKSsRKNS_5IoCtxEmi);

void librados::ObjectWriteOperation::set_chunk(uint64_t src_offset,
					       uint64_t src_length,
					       const IoCtx& tgt_ioctx,
					       string tgt_oid,
					       uint64_t tgt_offset,
					       int flag)
{
  ::ObjectOperation *o = &impl->o;
  o->set_chunk(src_offset, src_length, 
	       tgt_ioctx.io_ctx_impl->oloc, object_t(tgt_oid), tgt_offset, flag);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(9set_chunkEmmRKNS_5IoCtxENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmi);
LIBRADOS_OBJECT_WRITE_OPERATION_API(9set_chunkEmmRKNS_5IoCtxESsmi);

void librados::ObjectWriteOperation::tier_promote()
{
  ::ObjectOperation *o = &impl->o;
  o->tier_promote();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(12tier_promoteEv);

void librados::ObjectWriteOperation::unset_manifest()
{
  ::ObjectOperation *o = &impl->o;
  o->unset_manifest();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(14unset_manifestEv);

void librados::ObjectWriteOperation::tmap_update(const bufferlist& cmdbl)
{
  ::ObjectOperation *o = &impl->o;
  bufferlist c = cmdbl;
  o->tmap_update(c);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(11tmap_updateERKN4ceph6buffer4listE);

void librados::ObjectWriteOperation::selfmanaged_snap_rollback(snap_t snapid)
{
  ::ObjectOperation *o = &impl->o;
  o->rollback(snapid);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(25selfmanaged_snap_rollbackEm);

// You must specify the snapid not the name normally used with pool snapshots
void librados::ObjectWriteOperation::snap_rollback(snap_t snapid)
{
  ::ObjectOperation *o = &impl->o;
  o->rollback(snapid);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(13snap_rollbackEm);

void librados::ObjectWriteOperation::set_alloc_hint(
                                            uint64_t expected_object_size,
                                            uint64_t expected_write_size)
{
  ::ObjectOperation *o = &impl->o;
  o->set_alloc_hint(expected_object_size, expected_write_size, 0);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(14set_alloc_hintEmm);
void librados::ObjectWriteOperation::set_alloc_hint2(
                                            uint64_t expected_object_size,
                                            uint64_t expected_write_size,
					    uint32_t flags)
{
  ::ObjectOperation *o = &impl->o;
  o->set_alloc_hint(expected_object_size, expected_write_size, flags);
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(15set_alloc_hint2Emmj);

void librados::ObjectWriteOperation::cache_pin()
{
  ::ObjectOperation *o = &impl->o;
  o->cache_pin();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(9cache_pinEv);

void librados::ObjectWriteOperation::cache_unpin()
{
  ::ObjectOperation *o = &impl->o;
  o->cache_unpin();
}
LIBRADOS_OBJECT_WRITE_OPERATION_API(11cache_unpinEv);

librados::WatchCtx::~WatchCtx()
{
}
LIBRADOS_CXX_API(_ZN8librados8WatchCtx, D0Ev);
LIBRADOS_CXX_API(_ZN8librados8WatchCtx, D1Ev);
LIBRADOS_CXX_API(_ZN8librados8WatchCtx, D2Ev);

librados::WatchCtx2::~WatchCtx2()
{
}
LIBRADOS_CXX_API(_ZN8librados9WatchCtx2, D0Ev);
LIBRADOS_CXX_API(_ZN8librados9WatchCtx2, D1Ev);
LIBRADOS_CXX_API(_ZN8librados9WatchCtx2, D2Ev);


///////////////////////////// NObjectIteratorImpl /////////////////////////////
librados::NObjectIteratorImpl::NObjectIteratorImpl(ObjListCtx *ctx_)
  : ctx(ctx_)
{
}

librados::NObjectIteratorImpl::~NObjectIteratorImpl()
{
  ctx.reset();
}

librados::NObjectIteratorImpl::NObjectIteratorImpl(const NObjectIteratorImpl &rhs)
{
  *this = rhs;
}

librados::NObjectIteratorImpl& librados::NObjectIteratorImpl::operator=(const librados::NObjectIteratorImpl &rhs)
{
  if (&rhs == this)
    return *this;
  if (rhs.ctx.get() == NULL) {
    ctx.reset();
    return *this;
  }
  Objecter::NListContext *list_ctx = new Objecter::NListContext(*rhs.ctx->nlc);
  ctx.reset(new ObjListCtx(rhs.ctx->ctx, list_ctx));
  cur_obj = rhs.cur_obj;
  return *this;
}

bool librados::NObjectIteratorImpl::operator==(const librados::NObjectIteratorImpl& rhs) const {

  if (ctx.get() == NULL) {
    if (rhs.ctx.get() == NULL)
      return true;
    return rhs.ctx->nlc->at_end();
  }
  if (rhs.ctx.get() == NULL) {
    // Redundant but same as ObjectIterator version
    if (ctx.get() == NULL)
      return true;
    return ctx->nlc->at_end();
  }
  return ctx.get() == rhs.ctx.get();
}

bool librados::NObjectIteratorImpl::operator!=(const librados::NObjectIteratorImpl& rhs) const {
  return !(*this == rhs);
}

const librados::ListObject& librados::NObjectIteratorImpl::operator*() const {
  return cur_obj;
}

const librados::ListObject* librados::NObjectIteratorImpl::operator->() const {
  return &cur_obj;
}

librados::NObjectIteratorImpl& librados::NObjectIteratorImpl::operator++()
{
  get_next();
  return *this;
}

librados::NObjectIteratorImpl librados::NObjectIteratorImpl::operator++(int)
{
  librados::NObjectIteratorImpl ret(*this);
  get_next();
  return ret;
}

uint32_t librados::NObjectIteratorImpl::seek(uint32_t pos)
{
  uint32_t r = rados_nobjects_list_seek(ctx.get(), pos);
  get_next();
  return r;
}

uint32_t librados::NObjectIteratorImpl::seek(const ObjectCursor& cursor)
{
  uint32_t r = rados_nobjects_list_seek_cursor(ctx.get(), (rados_object_list_cursor)cursor.c_cursor);
  get_next();
  return r;
}

librados::ObjectCursor librados::NObjectIteratorImpl::get_cursor()
{
  librados::ObjListCtx *lh = (librados::ObjListCtx *)ctx.get();
  librados::ObjectCursor oc;
  oc.set(lh->ctx->nlist_get_cursor(lh->nlc));
  return oc;
}

void librados::NObjectIteratorImpl::set_filter(const bufferlist &bl)
{
  ceph_assert(ctx);
  ctx->nlc->filter = bl;
}

void librados::NObjectIteratorImpl::get_next()
{
  const char *entry, *key, *nspace;
  if (ctx->nlc->at_end())
    return;
  int ret = rados_nobjects_list_next(ctx.get(), &entry, &key, &nspace);
  if (ret == -ENOENT) {
    return;
  }
  else if (ret) {
    throw std::system_error(-ret, std::system_category(),
                            "rados_nobjects_list_next");
  }

  if (cur_obj.impl == NULL)
    cur_obj.impl = new ListObjectImpl();
  cur_obj.impl->nspace = nspace;
  cur_obj.impl->oid = entry;
  cur_obj.impl->locator = key ? key : string();
}

uint32_t librados::NObjectIteratorImpl::get_pg_hash_position() const
{
  return ctx->nlc->get_pg_hash_position();
}

///////////////////////////// NObjectIterator /////////////////////////////

/// symbol versioning helpers for librados::NObjectIterator
#define LIBRADOS_NOBJECT_ITERATOR_API(fn)                 \
  LIBRADOS_CXX_API(_ZN8librados15NObjectIterator, fn)
#define LIBRADOS_NOBJECT_ITERATOR_API_CONST(fn)           \
  LIBRADOS_CXX_API(_ZNK8librados15NObjectIterator, fn)

librados::NObjectIterator::NObjectIterator(ObjListCtx *ctx_)
{
  impl = new NObjectIteratorImpl(ctx_);
}
LIBRADOS_NOBJECT_ITERATOR_API(C1EPNS_10ObjListCtxE);
LIBRADOS_NOBJECT_ITERATOR_API(C2EPNS_10ObjListCtxE);

librados::NObjectIterator::~NObjectIterator()
{
  delete impl;
}
LIBRADOS_NOBJECT_ITERATOR_API(D1Ev);
LIBRADOS_NOBJECT_ITERATOR_API(D2Ev);

librados::NObjectIterator::NObjectIterator(const NObjectIterator &rhs)
{
  if (rhs.impl == NULL) {
    impl = NULL;
    return;
  }
  impl = new NObjectIteratorImpl();
  *impl = *(rhs.impl);
}
LIBRADOS_NOBJECT_ITERATOR_API(C1ERKS0_);
LIBRADOS_NOBJECT_ITERATOR_API(C2ERKS0_);

librados::NObjectIterator& librados::NObjectIterator::operator=(const librados::NObjectIterator &rhs)
{
  if (rhs.impl == NULL) {
    delete impl;
    impl = NULL;
    return *this;
  }
  if (impl == NULL)
    impl = new NObjectIteratorImpl();
  *impl = *(rhs.impl);
  return *this;
}
LIBRADOS_NOBJECT_ITERATOR_API(aSERKS0_);

bool librados::NObjectIterator::operator==(const librados::NObjectIterator& rhs) const 
{
  if (impl && rhs.impl) {
    return *impl == *(rhs.impl);
  } else {
    return impl == rhs.impl;
  }
}
LIBRADOS_NOBJECT_ITERATOR_API_CONST(eqERKS0_);

bool librados::NObjectIterator::operator!=(const librados::NObjectIterator& rhs) const
{
  return !(*this == rhs);
}
LIBRADOS_NOBJECT_ITERATOR_API_CONST(neERKS0_);

const librados::ListObject& librados::NObjectIterator::operator*() const {
  ceph_assert(impl);
  return *(impl->get_listobjectp());
}
LIBRADOS_NOBJECT_ITERATOR_API_CONST(deEv);

const librados::ListObject* librados::NObjectIterator::operator->() const {
  ceph_assert(impl);
  return impl->get_listobjectp();
}
LIBRADOS_NOBJECT_ITERATOR_API_CONST(ptEv);

librados::NObjectIterator& librados::NObjectIterator::operator++()
{
  ceph_assert(impl);
  impl->get_next();
  return *this;
}
LIBRADOS_NOBJECT_ITERATOR_API(ppEv);

librados::NObjectIterator librados::NObjectIterator::operator++(int)
{
  librados::NObjectIterator ret(*this);
  impl->get_next();
  return ret;
}
LIBRADOS_NOBJECT_ITERATOR_API(ppEi);

uint32_t librados::NObjectIterator::seek(uint32_t pos)
{
  ceph_assert(impl);
  return impl->seek(pos);
}
LIBRADOS_NOBJECT_ITERATOR_API(4seekEj);

uint32_t librados::NObjectIterator::seek(const ObjectCursor& cursor)
{
  ceph_assert(impl);
  return impl->seek(cursor);
}
LIBRADOS_NOBJECT_ITERATOR_API(4seekERKNS_12ObjectCursorE);

librados::ObjectCursor librados::NObjectIterator::get_cursor()
{
  ceph_assert(impl);
  return impl->get_cursor();
}
LIBRADOS_NOBJECT_ITERATOR_API(10get_cursorEv);

void librados::NObjectIterator::set_filter(const bufferlist &bl)
{
  impl->set_filter(bl);
}
LIBRADOS_NOBJECT_ITERATOR_API(10set_filterERKN4ceph6buffer4listE);

void librados::NObjectIterator::get_next()
{
  ceph_assert(impl);
  impl->get_next();
}
LIBRADOS_NOBJECT_ITERATOR_API(8get_nextEv);

uint32_t librados::NObjectIterator::get_pg_hash_position() const
{
  ceph_assert(impl);
  return impl->get_pg_hash_position();
}
LIBRADOS_NOBJECT_ITERATOR_API(20get_pg_hash_positionEv);

const librados::NObjectIterator librados::NObjectIterator::__EndObjectIterator(NULL);
LIBRADOS_NOBJECT_ITERATOR_API(19__EndObjectIteratorE);

///////////////////////////// PoolAsyncCompletion //////////////////////////////

/// symbol versioning helpers for librados::PoolAsyncCompletion
#define LIBRADOS_POOL_ASYNC_COMPLETION_API(fn)          \
  LIBRADOS_CXX_API(_ZN8librados19PoolAsyncCompletion, fn)

int librados::PoolAsyncCompletion::PoolAsyncCompletion::set_callback(void *cb_arg,
								     rados_callback_t cb)
{
  PoolAsyncCompletionImpl *c = (PoolAsyncCompletionImpl *)pc;
  return c->set_callback(cb_arg, cb);
}
LIBRADOS_POOL_ASYNC_COMPLETION_API(12set_callbackEPvPFvS1_S1_E);

int librados::PoolAsyncCompletion::PoolAsyncCompletion::wait()
{
  PoolAsyncCompletionImpl *c = (PoolAsyncCompletionImpl *)pc;
  return c->wait();
}
LIBRADOS_POOL_ASYNC_COMPLETION_API(4waitEv);

bool librados::PoolAsyncCompletion::PoolAsyncCompletion::is_complete()
{
  PoolAsyncCompletionImpl *c = (PoolAsyncCompletionImpl *)pc;
  return c->is_complete();
}
LIBRADOS_POOL_ASYNC_COMPLETION_API(11is_completeEv);

int librados::PoolAsyncCompletion::PoolAsyncCompletion::get_return_value()
{
  PoolAsyncCompletionImpl *c = (PoolAsyncCompletionImpl *)pc;
  return c->get_return_value();
}
LIBRADOS_POOL_ASYNC_COMPLETION_API(16get_return_valueEv);

void librados::PoolAsyncCompletion::PoolAsyncCompletion::release()
{
  PoolAsyncCompletionImpl *c = (PoolAsyncCompletionImpl *)pc;
  c->release();
  delete this;
}
LIBRADOS_POOL_ASYNC_COMPLETION_API(7releaseEv);

///////////////////////////// AioCompletion //////////////////////////////

/// symbol versioning helpers for librados::AioCompletion
#define LIBRADOS_AIO_COMPLETION_API(fn)               \
  LIBRADOS_CXX_API(_ZN8librados13AioCompletion, fn)

int librados::AioCompletion::AioCompletion::set_complete_callback(void *cb_arg, rados_callback_t cb)
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->set_complete_callback(cb_arg, cb);
}
LIBRADOS_AIO_COMPLETION_API(21set_complete_callbackEPvPFvS1_S1_E);

int librados::AioCompletion::AioCompletion::set_safe_callback(void *cb_arg, rados_callback_t cb)
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->set_safe_callback(cb_arg, cb);
}
LIBRADOS_AIO_COMPLETION_API(17set_safe_callbackEPvPFvS1_S1_E);

int librados::AioCompletion::AioCompletion::wait_for_complete()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->wait_for_complete();
}
LIBRADOS_AIO_COMPLETION_API(17wait_for_completeEv);

int librados::AioCompletion::AioCompletion::wait_for_safe()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->wait_for_safe();
}
LIBRADOS_AIO_COMPLETION_API(13wait_for_safeEv);

bool librados::AioCompletion::AioCompletion::is_complete()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->is_complete();
}
LIBRADOS_AIO_COMPLETION_API(11is_completeEv);

bool librados::AioCompletion::AioCompletion::is_safe()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->is_safe();
}
LIBRADOS_AIO_COMPLETION_API(7is_safeEv);

int librados::AioCompletion::AioCompletion::wait_for_complete_and_cb()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->wait_for_complete_and_cb();
}
LIBRADOS_AIO_COMPLETION_API(24wait_for_complete_and_cbEv);

int librados::AioCompletion::AioCompletion::wait_for_safe_and_cb()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->wait_for_safe_and_cb();
}
LIBRADOS_AIO_COMPLETION_API(20wait_for_safe_and_cbEv);

bool librados::AioCompletion::AioCompletion::is_complete_and_cb()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->is_complete_and_cb();
}
LIBRADOS_AIO_COMPLETION_API(18is_complete_and_cbEv);

bool librados::AioCompletion::AioCompletion::is_safe_and_cb()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->is_safe_and_cb();
}
LIBRADOS_AIO_COMPLETION_API(14is_safe_and_cbEv);

int librados::AioCompletion::AioCompletion::get_return_value()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->get_return_value();
}
LIBRADOS_AIO_COMPLETION_API(16get_return_valueEv);

int librados::AioCompletion::AioCompletion::get_version()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->get_version();
}
LIBRADOS_AIO_COMPLETION_API(11get_versionEv);

uint64_t librados::AioCompletion::AioCompletion::get_version64()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  return c->get_version();
}
LIBRADOS_AIO_COMPLETION_API(13get_version64Ev);

void librados::AioCompletion::AioCompletion::release()
{
  AioCompletionImpl *c = (AioCompletionImpl *)pc;
  c->release();
  delete this;
}
LIBRADOS_AIO_COMPLETION_API(7releaseEv);

///////////////////////////// IoCtx //////////////////////////////

/// symbol versioning helpers for librados::AioCompletion
#define LIBRADOS_IOCTX_API(fn)                  \
  LIBRADOS_CXX_API(_ZN8librados5IoCtx, fn)
#define LIBRADOS_IOCTX_API_CONST(fn)            \
  LIBRADOS_CXX_API(_ZNK8librados5IoCtx, fn)

librados::IoCtx::IoCtx() : io_ctx_impl(NULL)
{
}
LIBRADOS_IOCTX_API(C1Ev);
LIBRADOS_IOCTX_API(C2Ev);

void librados::IoCtx::from_rados_ioctx_t(rados_ioctx_t p, IoCtx &io)
{
  IoCtxImpl *io_ctx_impl = (IoCtxImpl*)p;

  io.io_ctx_impl = io_ctx_impl;
  if (io_ctx_impl) {
    io_ctx_impl->get();
  }
}
LIBRADOS_IOCTX_API(18from_rados_ioctx_tEPvRS0_);

librados::IoCtx::IoCtx(const IoCtx& rhs)
{
  io_ctx_impl = rhs.io_ctx_impl;
  if (io_ctx_impl) {
    io_ctx_impl->get();
  }
}
LIBRADOS_IOCTX_API(C1ERKS0_);
LIBRADOS_IOCTX_API(C1ERKS1_);

librados::IoCtx& librados::IoCtx::operator=(const IoCtx& rhs)
{
  if (io_ctx_impl)
    io_ctx_impl->put();
  io_ctx_impl = rhs.io_ctx_impl;
  io_ctx_impl->get();
  return *this;
}
LIBRADOS_IOCTX_API(aSERKS0_);

librados::IoCtx::IoCtx(IoCtx&& rhs) noexcept
  : io_ctx_impl(std::exchange(rhs.io_ctx_impl, nullptr))
{
}
LIBRADOS_IOCTX_API(C1EOS0_);
LIBRADOS_IOCTX_API(C2EOS1_);

librados::IoCtx& librados::IoCtx::operator=(IoCtx&& rhs) noexcept
{
  if (io_ctx_impl)
    io_ctx_impl->put();
  io_ctx_impl = std::exchange(rhs.io_ctx_impl, nullptr);
  return *this;
}
LIBRADOS_IOCTX_API(aSEOS0_);

librados::IoCtx::~IoCtx()
{
  close();
}
LIBRADOS_IOCTX_API(D1Ev);
LIBRADOS_IOCTX_API(D2Ev);

void librados::IoCtx::close()
{
  if (io_ctx_impl)
    io_ctx_impl->put();
  io_ctx_impl = 0;
}
LIBRADOS_IOCTX_API(5closeEv);

void librados::IoCtx::dup(const IoCtx& rhs)
{
  if (io_ctx_impl)
    io_ctx_impl->put();
  io_ctx_impl = new IoCtxImpl();
  io_ctx_impl->get();
  io_ctx_impl->dup(*rhs.io_ctx_impl);
}
LIBRADOS_IOCTX_API(3dupERKS0_);

int librados::IoCtx::set_auid(uint64_t auid_)
{
  return -EOPNOTSUPP;
}
LIBRADOS_IOCTX_API(8set_auidEm);

int librados::IoCtx::set_auid_async(uint64_t auid_, PoolAsyncCompletion *c)
{
  return -EOPNOTSUPP;
}
LIBRADOS_IOCTX_API(14set_auid_asyncEmPNS_19PoolAsyncCompletionE);

int librados::IoCtx::get_auid(uint64_t *auid_)
{
  return -EOPNOTSUPP;
}
LIBRADOS_IOCTX_API(8get_auidEPm);

bool librados::IoCtx::pool_requires_alignment()
{
  return io_ctx_impl->client->pool_requires_alignment(get_id());
}
LIBRADOS_IOCTX_API(23pool_requires_alignmentEv);

int librados::IoCtx::pool_requires_alignment2(bool *requires)
{
  return io_ctx_impl->client->pool_requires_alignment2(get_id(), requires);
}
LIBRADOS_IOCTX_API(24pool_requires_alignment2EPb);

uint64_t librados::IoCtx::pool_required_alignment()
{
  return io_ctx_impl->client->pool_required_alignment(get_id());
}
LIBRADOS_IOCTX_API(23pool_required_alignmentEv);

int librados::IoCtx::pool_required_alignment2(uint64_t *alignment)
{
  return io_ctx_impl->client->pool_required_alignment2(get_id(), alignment);
}
LIBRADOS_IOCTX_API(24pool_required_alignment2EPm);

std::string librados::IoCtx::get_pool_name()
{
  std::string s;
  io_ctx_impl->client->pool_get_name(get_id(), &s);
  return s;
}
LIBRADOS_IOCTX_API(13get_pool_nameB5cxx11Ev);
LIBRADOS_IOCTX_API(13get_pool_nameEv);

std::string librados::IoCtx::get_pool_name() const
{
  return io_ctx_impl->get_cached_pool_name();
}
LIBRADOS_IOCTX_API_CONST(13get_pool_nameB5cxx11Ev);
LIBRADOS_IOCTX_API_CONST(13get_pool_nameEv);

uint64_t librados::IoCtx::get_instance_id() const
{
  return io_ctx_impl->client->get_instance_id();
}
LIBRADOS_IOCTX_API_CONST(15get_instance_idEv);

int librados::IoCtx::create(const std::string& oid, bool exclusive)
{
  object_t obj(oid);
  return io_ctx_impl->create(obj, exclusive);
}
LIBRADOS_IOCTX_API(6createERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb);
LIBRADOS_IOCTX_API(6createERKSsb);

int librados::IoCtx::create(const std::string& oid, bool exclusive,
			    const std::string& category) // unused
{
  object_t obj(oid);
  return io_ctx_impl->create(obj, exclusive);
}
LIBRADOS_IOCTX_API(6createERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEbS8_);
LIBRADOS_IOCTX_API(6createERKSsbS2_);

int librados::IoCtx::write(const std::string& oid, bufferlist& bl, size_t len, uint64_t off)
{
  object_t obj(oid);
  return io_ctx_impl->write(obj, bl, len, off);
}
LIBRADOS_IOCTX_API(5writeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(5writeERKSsRN4ceph6buffer4listEmm);

int librados::IoCtx::append(const std::string& oid, bufferlist& bl, size_t len)
{
  object_t obj(oid);
  return io_ctx_impl->append(obj, bl, len);
}
LIBRADOS_IOCTX_API(6appendERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listEm);
LIBRADOS_IOCTX_API(6appendERKSsRN4ceph6buffer4listEm);

int librados::IoCtx::write_full(const std::string& oid, bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->write_full(obj, bl);
}
LIBRADOS_IOCTX_API(10write_fullERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(10write_fullERKSsRN4ceph6buffer4listE);

int librados::IoCtx::writesame(const std::string& oid, bufferlist& bl,
			       size_t write_len, uint64_t off)
{
  object_t obj(oid);
  return io_ctx_impl->writesame(obj, bl, write_len, off);
}
LIBRADOS_IOCTX_API(9writesameERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(9writesameERKSsRN4ceph6buffer4listEmm);

int librados::IoCtx::read(const std::string& oid, bufferlist& bl, size_t len, uint64_t off)
{
  object_t obj(oid);
  return io_ctx_impl->read(obj, bl, len, off);
}
LIBRADOS_IOCTX_API(4readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(4readERKSsRN4ceph6buffer4listEmm);

int librados::IoCtx::checksum(const std::string& oid,
			      rados_checksum_type_t type,
			      const bufferlist &init_value_bl, size_t len,
			      uint64_t off, size_t chunk_size, bufferlist *pbl)
{
  object_t obj(oid);
  return io_ctx_impl->checksum(obj, get_checksum_op_type(type), init_value_bl,
			       len, off, chunk_size, pbl);
}
LIBRADOS_IOCTX_API(8checksumERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE21rados_checksum_type_tRKN4ceph6buffer4listEmmmPSC_);
LIBRADOS_IOCTX_API(8checksumERKSs21rados_checksum_type_tRKN4ceph6buffer4listEmmmPS6_);

int librados::IoCtx::remove(const std::string& oid)
{
  object_t obj(oid);
  return io_ctx_impl->remove(obj);
}
LIBRADOS_IOCTX_API(6removeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(6removeERKSs);

int librados::IoCtx::remove(const std::string& oid, int flags)
{
  object_t obj(oid);
  return io_ctx_impl->remove(obj, flags); 
}
LIBRADOS_IOCTX_API(6removeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEi);
LIBRADOS_IOCTX_API(6removeERKSsi);

int librados::IoCtx::trunc(const std::string& oid, uint64_t size)
{
  object_t obj(oid);
  return io_ctx_impl->trunc(obj, size);
}
LIBRADOS_IOCTX_API(5truncERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm);
LIBRADOS_IOCTX_API(5truncERKSsm);

int librados::IoCtx::mapext(const std::string& oid, uint64_t off, size_t len,
			    std::map<uint64_t,uint64_t>& m)
{
  object_t obj(oid);
  return io_ctx_impl->mapext(obj, off, len, m);
}
LIBRADOS_IOCTX_API(6mapextERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmmRSt3mapImmSt4lessImESaISt4pairIKmmEEE);
LIBRADOS_IOCTX_API(6mapextERKSsmmRSt3mapImmSt4lessImESaISt4pairIKmmEEE);

int librados::IoCtx::cmpext(const std::string& oid, uint64_t off, bufferlist& cmp_bl)
{
  object_t obj(oid);
  return io_ctx_impl->cmpext(obj, off, cmp_bl);
}
LIBRADOS_IOCTX_API(6cmpextERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(6cmpextERKSsmRN4ceph6buffer4listE);

int librados::IoCtx::sparse_read(const std::string& oid, std::map<uint64_t,uint64_t>& m,
				 bufferlist& bl, size_t len, uint64_t off)
{
  object_t obj(oid);
  return io_ctx_impl->sparse_read(obj, m, bl, len, off);
}
LIBRADOS_IOCTX_API(11sparse_readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERSt3mapImmSt4lessImESaISt4pairIKmmEEERN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(11sparse_readERKSsRSt3mapImmSt4lessImESaISt4pairIKmmEEERN4ceph6buffer4listEmm);

int librados::IoCtx::getxattr(const std::string& oid, const char *name, bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->getxattr(obj, name, bl);
}
LIBRADOS_IOCTX_API(8getxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKcRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(8getxattrERKSsPKcRN4ceph6buffer4listE);

int librados::IoCtx::getxattrs(const std::string& oid, map<std::string, bufferlist>& attrset)
{
  object_t obj(oid);
  return io_ctx_impl->getxattrs(obj, attrset);
}
LIBRADOS_IOCTX_API(9getxattrsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEE);
LIBRADOS_IOCTX_API(9getxattrsERKSsRSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEE);

int librados::IoCtx::setxattr(const std::string& oid, const char *name, bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->setxattr(obj, name, bl);
}
LIBRADOS_IOCTX_API(8setxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKcRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(8setxattrERKSsPKcRN4ceph6buffer4listE);

int librados::IoCtx::rmxattr(const std::string& oid, const char *name)
{
  object_t obj(oid);
  return io_ctx_impl->rmxattr(obj, name);
}
LIBRADOS_IOCTX_API(7rmxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKc);
LIBRADOS_IOCTX_API(7rmxattrERKSsPKc);

int librados::IoCtx::stat(const std::string& oid, uint64_t *psize, time_t *pmtime)
{
  object_t obj(oid);
  return io_ctx_impl->stat(obj, psize, pmtime);
}
LIBRADOS_IOCTX_API(4statERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPmPl);
LIBRADOS_IOCTX_API(4statERKSsPmPl);

int librados::IoCtx::stat2(const std::string& oid, uint64_t *psize, struct timespec *pts)
{
  object_t obj(oid);
  return io_ctx_impl->stat2(obj, psize, pts);
}
LIBRADOS_IOCTX_API(5stat2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPmP8timespec);
LIBRADOS_IOCTX_API(5stat2ERKSsPmP8timespec);

int librados::IoCtx::exec(const std::string& oid, const char *cls, const char *method,
			  bufferlist& inbl, bufferlist& outbl)
{
  object_t obj(oid);
  return io_ctx_impl->exec(obj, cls, method, inbl, outbl);
}
LIBRADOS_IOCTX_API(4execERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKcSA_RN4ceph6buffer4listESE_);
LIBRADOS_IOCTX_API(4execERKSsPKcS4_RN4ceph6buffer4listES8_);

int librados::IoCtx::tmap_update(const std::string& oid, bufferlist& cmdbl)
{
  object_t obj(oid);
  return io_ctx_impl->tmap_update(obj, cmdbl);
}
LIBRADOS_IOCTX_API(11tmap_updateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(11tmap_updateERKSsRN4ceph6buffer4listE);

int librados::IoCtx::omap_get_vals(const std::string& oid,
                                   const std::string& orig_start_after,
                                   const std::string& filter_prefix,
                                   uint64_t max_return,
                                   std::map<std::string, bufferlist> *out_vals)
{
  bool first = true;
  string start_after = orig_start_after;
  bool more = true;
  while (max_return > 0 && more) {
    std::map<std::string,bufferlist> out;
    ObjectReadOperation op;
    op.omap_get_vals2(start_after, filter_prefix, max_return, &out, &more,
		      nullptr);
    bufferlist bl;
    int ret = operate(oid, &op, &bl);
    if (ret < 0) {
      return ret;
    }
    if (more) {
      if (out.empty()) {
	return -EINVAL;  // wth
      }
      start_after = out.rbegin()->first;
    }
    if (out.size() <= max_return) {
      max_return -= out.size();
    } else {
      max_return = 0;
    }
    if (first) {
      out_vals->swap(out);
      first = false;
    } else {
      out_vals->insert(out.begin(), out.end());
      out.clear();
    }
  }
  return 0;
}
LIBRADOS_IOCTX_API(13omap_get_valsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEE);
LIBRADOS_IOCTX_API(13omap_get_valsERKSsS2_S2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEE);

int librados::IoCtx::omap_get_vals2(
  const std::string& oid,
  const std::string& start_after,
  const std::string& filter_prefix,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  bool *pmore)
{
  ObjectReadOperation op;
  int r;
  op.omap_get_vals2(start_after, filter_prefix, max_return, out_vals, pmore, &r);
  bufferlist bl;
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;
  return r;
}
LIBRADOS_IOCTX_API(14omap_get_vals2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPb);
LIBRADOS_IOCTX_API(14omap_get_vals2ERKSsS2_S2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPb);

int librados::IoCtx::omap_get_vals(const std::string& oid,
                                   const std::string& start_after,
                                   uint64_t max_return,
                                   std::map<std::string, bufferlist> *out_vals)
{
  return omap_get_vals(oid, start_after, string(), max_return, out_vals);
}
LIBRADOS_IOCTX_API(13omap_get_valsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEE);
LIBRADOS_IOCTX_API(13omap_get_valsERKSsS2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEE);

int librados::IoCtx::omap_get_vals2(
  const std::string& oid,
  const std::string& start_after,
  uint64_t max_return,
  std::map<std::string, bufferlist> *out_vals,
  bool *pmore)
{
  ObjectReadOperation op;
  int r;
  op.omap_get_vals2(start_after, max_return, out_vals, pmore, &r);
  bufferlist bl;
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;
  return r;
}
LIBRADOS_IOCTX_API(14omap_get_vals2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEEPb);
LIBRADOS_IOCTX_API(14omap_get_vals2ERKSsS2_mPSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEEPb);

int librados::IoCtx::omap_get_keys(const std::string& oid,
                                   const std::string& orig_start_after,
                                   uint64_t max_return,
                                   std::set<std::string> *out_keys)
{
  bool first = true;
  string start_after = orig_start_after;
  bool more = true;
  while (max_return > 0 && more) {
    std::set<std::string> out;
    ObjectReadOperation op;
    op.omap_get_keys2(start_after, max_return, &out, &more, nullptr);
    bufferlist bl;
    int ret = operate(oid, &op, &bl);
    if (ret < 0) {
      return ret;
    }
    if (more) {
      if (out.empty()) {
	return -EINVAL;  // wth
      }
      start_after = *out.rbegin();
    }
    if (out.size() <= max_return) {
      max_return -= out.size();
    } else {
      max_return = 0;
    }
    if (first) {
      out_keys->swap(out);
      first = false;
    } else {
      out_keys->insert(out.begin(), out.end());
      out.clear();
    }
  }
  return 0;
}
LIBRADOS_IOCTX_API(13omap_get_keysERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3setIS6_St4lessIS6_ESaIS6_EE);
LIBRADOS_IOCTX_API(13omap_get_keysERKSsS2_mPSt3setISsSt4lessISsESaISsEE);

int librados::IoCtx::omap_get_keys2(
  const std::string& oid,
  const std::string& start_after,
  uint64_t max_return,
  std::set<std::string> *out_keys,
  bool *pmore)
{
  ObjectReadOperation op;
  int r;
  op.omap_get_keys2(start_after, max_return, out_keys, pmore, &r);
  bufferlist bl;
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;
  return r;
}
LIBRADOS_IOCTX_API(14omap_get_keys2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_mPSt3setIS6_St4lessIS6_ESaIS6_EEPb);
LIBRADOS_IOCTX_API(14omap_get_keys2ERKSsS2_mPSt3setISsSt4lessISsESaISsEEPb);

int librados::IoCtx::omap_get_header(const std::string& oid,
                                     bufferlist *bl)
{
  ObjectReadOperation op;
  int r;
  op.omap_get_header(bl, &r);
  bufferlist b;
  int ret = operate(oid, &op, &b);
  if (ret < 0)
    return ret;

  return r;
}
LIBRADOS_IOCTX_API(15omap_get_headerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(15omap_get_headerERKSsPN4ceph6buffer4listE);

int librados::IoCtx::omap_get_vals_by_keys(const std::string& oid,
                                           const std::set<std::string>& keys,
                                           std::map<std::string, bufferlist> *vals)
{
  ObjectReadOperation op;
  int r;
  bufferlist bl;
  op.omap_get_vals_by_keys(keys, vals, &r);
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;

  return r;
}
LIBRADOS_IOCTX_API(21omap_get_vals_by_keysERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKSt3setIS6_St4lessIS6_ESaIS6_EEPSt3mapIS6_N4ceph6buffer4listESB_SaISt4pairIS7_SJ_EEE);
LIBRADOS_IOCTX_API(21omap_get_vals_by_keysERKSsRKSt3setISsSt4lessISsESaISsEEPSt3mapISsN4ceph6buffer4listES5_SaISt4pairIS1_SD_EEE);

int librados::IoCtx::omap_set(const std::string& oid,
                              const map<string, bufferlist>& m)
{
  ObjectWriteOperation op;
  op.omap_set(m);
  return operate(oid, &op);
}
LIBRADOS_IOCTX_API(8omap_setERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SC_EEE);
LIBRADOS_IOCTX_API(8omap_setERKSsRKSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S6_EEE);

int librados::IoCtx::omap_set_header(const std::string& oid,
                                     const bufferlist& bl)
{
  ObjectWriteOperation op;
  op.omap_set_header(bl);
  return operate(oid, &op);
}
LIBRADOS_IOCTX_API(15omap_set_headerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(15omap_set_headerERKSsRKN4ceph6buffer4listE);

int librados::IoCtx::omap_clear(const std::string& oid)
{
  ObjectWriteOperation op;
  op.omap_clear();
  return operate(oid, &op);
}
LIBRADOS_IOCTX_API(10omap_clearERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(10omap_clearERKSs);

int librados::IoCtx::omap_rm_keys(const std::string& oid,
                                  const std::set<std::string>& keys)
{
  ObjectWriteOperation op;
  op.omap_rm_keys(keys);
  return operate(oid, &op);
}
LIBRADOS_IOCTX_API(12omap_rm_keysERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKSt3setIS6_St4lessIS6_ESaIS6_EE);
LIBRADOS_IOCTX_API(12omap_rm_keysERKSsRKSt3setISsSt4lessISsESaISsEE);

int librados::IoCtx::operate(const std::string& oid, librados::ObjectWriteOperation *o)
{
  object_t obj(oid);
  return io_ctx_impl->operate(obj, &o->impl->o, (ceph::real_time *)o->impl->prt);
}
LIBRADOS_IOCTX_API(7operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_20ObjectWriteOperationE);
LIBRADOS_IOCTX_API(7operateERKSsPNS_20ObjectWriteOperationE);

int librados::IoCtx::operate(const std::string& oid, librados::ObjectReadOperation *o, bufferlist *pbl)
{
  object_t obj(oid);
  return io_ctx_impl->operate_read(obj, &o->impl->o, pbl);
}
LIBRADOS_IOCTX_API(7operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_19ObjectReadOperationEPN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(7operateERKSsPNS_19ObjectReadOperationEPN4ceph6buffer4listE);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 librados::ObjectWriteOperation *o)
{
  object_t obj(oid);
  return io_ctx_impl->aio_operate(obj, &o->impl->o, c->pc,
				  io_ctx_impl->snapc, 0);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_20ObjectWriteOperationE);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_20ObjectWriteOperationE);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 ObjectWriteOperation *o, int flags)
{
  object_t obj(oid);
  return io_ctx_impl->aio_operate(obj, &o->impl->o, c->pc,
				  io_ctx_impl->snapc,
				  translate_flags(flags));
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_20ObjectWriteOperationEi);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_20ObjectWriteOperationEi);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 librados::ObjectWriteOperation *o,
				 snap_t snap_seq, std::vector<snap_t>& snaps)
{
  object_t obj(oid);
  vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i)
    snv[i] = snaps[i];
  SnapContext snapc(snap_seq, snv);
  return io_ctx_impl->aio_operate(obj, &o->impl->o, c->pc,
				  snapc, 0);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEE);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEE);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
         librados::ObjectWriteOperation *o,
         snap_t snap_seq, std::vector<snap_t>& snaps,
         const blkin_trace_info *trace_info)
{
  object_t obj(oid);
  vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i)
    snv[i] = snaps[i];
  SnapContext snapc(snap_seq, snv);
  return io_ctx_impl->aio_operate(obj, &o->impl->o, c->pc,
          snapc, 0, trace_info);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEEPK16blkin_trace_info);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEEPK16blkin_trace_info);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
         librados::ObjectWriteOperation *o,
         snap_t snap_seq, std::vector<snap_t>& snaps, int flags,
         const blkin_trace_info *trace_info)
{
  object_t obj(oid);
  vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i)
    snv[i] = snaps[i];
  SnapContext snapc(snap_seq, snv);
  return io_ctx_impl->aio_operate(obj, &o->impl->o, c->pc, snapc,
                                  translate_flags(flags), trace_info);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEEiPK16blkin_trace_info);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_20ObjectWriteOperationEmRSt6vectorImSaImEEiPK16blkin_trace_info);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 librados::ObjectReadOperation *o,
				 bufferlist *pbl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_operate_read(obj, &o->impl->o, c->pc,
				       0, pbl);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_19ObjectReadOperationEPN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_19ObjectReadOperationEPN4ceph6buffer4listE);

// deprecated
int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 librados::ObjectReadOperation *o, 
				 snap_t snapid_unused_deprecated,
				 int flags, bufferlist *pbl)
{
  object_t obj(oid);
  int op_flags = 0;
  if (flags & OPERATION_BALANCE_READS)
    op_flags |= CEPH_OSD_FLAG_BALANCE_READS;
  if (flags & OPERATION_LOCALIZE_READS)
    op_flags |= CEPH_OSD_FLAG_LOCALIZE_READS;
  if (flags & OPERATION_ORDER_READS_WRITES)
    op_flags |= CEPH_OSD_FLAG_RWORDERED;

  return io_ctx_impl->aio_operate_read(obj, &o->impl->o, c->pc,
				       op_flags, pbl);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_19ObjectReadOperationEmiPN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_19ObjectReadOperationEmiPN4ceph6buffer4listE);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
				 librados::ObjectReadOperation *o,
				 int flags, bufferlist *pbl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_operate_read(obj, &o->impl->o, c->pc,
				       translate_flags(flags), pbl);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_19ObjectReadOperationEiPN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_19ObjectReadOperationEiPN4ceph6buffer4listE);

int librados::IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
         librados::ObjectReadOperation *o,
         int flags, bufferlist *pbl, const blkin_trace_info *trace_info)
{
  object_t obj(oid);
  return io_ctx_impl->aio_operate_read(obj, &o->impl->o, c->pc,
               translate_flags(flags), pbl, trace_info);
}
LIBRADOS_IOCTX_API(11aio_operateERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPNS_19ObjectReadOperationEiPN4ceph6buffer4listEPK16blkin_trace_info);
LIBRADOS_IOCTX_API(11aio_operateERKSsPNS_13AioCompletionEPNS_19ObjectReadOperationEiPN4ceph6buffer4listEPK16blkin_trace_info);

void librados::IoCtx::snap_set_read(snap_t seq)
{
  io_ctx_impl->set_snap_read(seq);
}
LIBRADOS_IOCTX_API(13snap_set_readEm);

int librados::IoCtx::selfmanaged_snap_set_write_ctx(snap_t seq, vector<snap_t>& snaps)
{
  vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (unsigned i=0; i<snaps.size(); i++)
    snv[i] = snaps[i];
  return io_ctx_impl->set_snap_write_context(seq, snv);
}
LIBRADOS_IOCTX_API(30selfmanaged_snap_set_write_ctxEmRSt6vectorImSaImEE);

int librados::IoCtx::snap_create(const char *snapname)
{
  return io_ctx_impl->snap_create(snapname);
}
LIBRADOS_IOCTX_API(11snap_createEPKc);

int librados::IoCtx::snap_lookup(const char *name, snap_t *snapid)
{
  return io_ctx_impl->snap_lookup(name, snapid);
}
LIBRADOS_IOCTX_API(11snap_lookupEPKcPm);

int librados::IoCtx::snap_get_stamp(snap_t snapid, time_t *t)
{
  return io_ctx_impl->snap_get_stamp(snapid, t);
}
LIBRADOS_IOCTX_API(14snap_get_stampEmPl);

int librados::IoCtx::snap_get_name(snap_t snapid, std::string *s)
{
  return io_ctx_impl->snap_get_name(snapid, s);
}
LIBRADOS_IOCTX_API(13snap_get_nameEmPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(13snap_get_nameEmPSs);

int librados::IoCtx::snap_remove(const char *snapname)
{
  return io_ctx_impl->snap_remove(snapname);
}
LIBRADOS_IOCTX_API(11snap_removeEPKc);

int librados::IoCtx::snap_list(std::vector<snap_t> *snaps)
{
  return io_ctx_impl->snap_list(snaps);
}
LIBRADOS_IOCTX_API(9snap_listEPSt6vectorImSaImEE);

int librados::IoCtx::snap_rollback(const std::string& oid, const char *snapname)
{
  return io_ctx_impl->rollback(oid, snapname);
}
LIBRADOS_IOCTX_API(13snap_rollbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKc);
LIBRADOS_IOCTX_API(13snap_rollbackERKSsPKc);

// Deprecated name kept for backward compatibility
int librados::IoCtx::rollback(const std::string& oid, const char *snapname)
{
  return snap_rollback(oid, snapname);
}
LIBRADOS_IOCTX_API(8rollbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPKc);
LIBRADOS_IOCTX_API(8rollbackERKSsPKc);

int librados::IoCtx::selfmanaged_snap_create(uint64_t *snapid)
{
  return io_ctx_impl->selfmanaged_snap_create(snapid);
}
LIBRADOS_IOCTX_API(23selfmanaged_snap_createEPm);

void librados::IoCtx::aio_selfmanaged_snap_create(uint64_t *snapid,
                                                  AioCompletion *c)
{
  io_ctx_impl->aio_selfmanaged_snap_create(snapid, c->pc);
}
LIBRADOS_IOCTX_API(27aio_selfmanaged_snap_createEPmPNS_13AioCompletionE);

int librados::IoCtx::selfmanaged_snap_remove(uint64_t snapid)
{
  return io_ctx_impl->selfmanaged_snap_remove(snapid);
}
LIBRADOS_IOCTX_API(23selfmanaged_snap_removeEm);

void librados::IoCtx::aio_selfmanaged_snap_remove(uint64_t snapid,
                                                  AioCompletion *c)
{
  io_ctx_impl->aio_selfmanaged_snap_remove(snapid, c->pc);
}
LIBRADOS_IOCTX_API(27aio_selfmanaged_snap_removeEmPNS_13AioCompletionE);

int librados::IoCtx::selfmanaged_snap_rollback(const std::string& oid, uint64_t snapid)
{
  return io_ctx_impl->selfmanaged_snap_rollback_object(oid,
						       io_ctx_impl->snapc,
						       snapid);
}
LIBRADOS_IOCTX_API(25selfmanaged_snap_rollbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm);
LIBRADOS_IOCTX_API(25selfmanaged_snap_rollbackERKSsm);

int librados::IoCtx::lock_exclusive(const std::string &oid, const std::string &name,
				    const std::string &cookie,
				    const std::string &description,
				    struct timeval * duration, uint8_t flags)
{
  utime_t dur = utime_t();
  if (duration)
    dur.set_from_timeval(duration);

  return rados::cls::lock::lock(this, oid, name, LOCK_EXCLUSIVE, cookie, "",
		  		description, dur, flags);
}
LIBRADOS_IOCTX_API(14lock_exclusiveERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_S8_P7timevalh);
LIBRADOS_IOCTX_API(14lock_exclusiveERKSsS2_S2_S2_P7timevalh);

int librados::IoCtx::lock_shared(const std::string &oid, const std::string &name,
				 const std::string &cookie, const std::string &tag,
				 const std::string &description,
				 struct timeval * duration, uint8_t flags)
{
  utime_t dur = utime_t();
  if (duration)
    dur.set_from_timeval(duration);

  return rados::cls::lock::lock(this, oid, name, LOCK_SHARED, cookie, tag,
		  		description, dur, flags);
}
LIBRADOS_IOCTX_API(11lock_sharedERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_S8_S8_P7timevalh);
LIBRADOS_IOCTX_API(11lock_sharedERKSsS2_S2_S2_S2_P7timevalh);

int librados::IoCtx::unlock(const std::string &oid, const std::string &name,
			    const std::string &cookie)
{
  return rados::cls::lock::unlock(this, oid, name, cookie);
}
LIBRADOS_IOCTX_API(6unlockERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_);
LIBRADOS_IOCTX_API(6unlockERKSsS2_S2_);

struct AioUnlockCompletion : public librados::ObjectOperationCompletion {
  librados::AioCompletionImpl *completion;
  AioUnlockCompletion(librados::AioCompletion *c) : completion(c->pc) {
    completion->get();
  };
  void handle_completion(int r, bufferlist& outbl) override {
    rados_callback_t cb = completion->callback_complete;
    void *cb_arg = completion->callback_complete_arg;
    cb(completion, cb_arg);
    completion->lock.Lock();
    completion->callback_complete = NULL;
    completion->cond.Signal();
    completion->put_unlock();
  }
};

int librados::IoCtx::aio_unlock(const std::string &oid, const std::string &name,
			        const std::string &cookie, AioCompletion *c)
{
  return rados::cls::lock::aio_unlock(this, oid, name, cookie, c);
}
LIBRADOS_IOCTX_API(10aio_unlockERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_PNS_13AioCompletionE);
LIBRADOS_IOCTX_API(10aio_unlockERKSsS2_S2_PNS_13AioCompletionE);

int librados::IoCtx::break_lock(const std::string &oid, const std::string &name,
				const std::string &client, const std::string &cookie)
{
  entity_name_t locker;
  if (!locker.parse(client))
    return -EINVAL;
  return rados::cls::lock::break_lock(this, oid, name, cookie, locker);
}
LIBRADOS_IOCTX_API(10break_lockERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_S8_);
LIBRADOS_IOCTX_API(10break_lockERKSsS2_S2_S2_);

int librados::IoCtx::list_lockers(const std::string &oid, const std::string &name,
				  int *exclusive,
				  std::string *tag,
				  std::list<librados::locker_t> *lockers)
{
  std::list<librados::locker_t> tmp_lockers;
  map<rados::cls::lock::locker_id_t, rados::cls::lock::locker_info_t> rados_lockers;
  std::string tmp_tag;
  ClsLockType tmp_type;
  int r = rados::cls::lock::get_lock_info(this, oid, name, &rados_lockers, &tmp_type, &tmp_tag);
  if (r < 0)
	  return r;

  map<rados::cls::lock::locker_id_t, rados::cls::lock::locker_info_t>::iterator map_it;
  for (map_it = rados_lockers.begin(); map_it != rados_lockers.end(); ++map_it) {
    librados::locker_t locker;
    locker.client = stringify(map_it->first.locker);
    locker.cookie = map_it->first.cookie;
    locker.address = stringify(map_it->second.addr);
    tmp_lockers.push_back(locker);
  }

  if (lockers)
    *lockers = tmp_lockers;
  if (tag)
    *tag = tmp_tag;
  if (exclusive) {
    if (tmp_type == LOCK_EXCLUSIVE)
      *exclusive = 1;
    else
      *exclusive = 0;
  }

  return tmp_lockers.size();
}
LIBRADOS_IOCTX_API(12list_lockersERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_PiPS6_PNS1_4listINS_8locker_tESaISC_EEE);
LIBRADOS_IOCTX_API(12list_lockersERKSsS2_PiPSsPSt4listINS_8locker_tESaIS6_EE);

librados::NObjectIterator librados::IoCtx::nobjects_begin(
    const bufferlist &filter)
{
  rados_list_ctx_t listh;
  rados_nobjects_list_open(io_ctx_impl, &listh);
  NObjectIterator iter((ObjListCtx*)listh);
  if (filter.length() > 0) {
    iter.set_filter(filter);
  }
  iter.get_next();
  return iter;
}
LIBRADOS_IOCTX_API(14nobjects_beginERKN4ceph6buffer4listE);

librados::NObjectIterator librados::IoCtx::nobjects_begin(
  uint32_t pos, const bufferlist &filter)
{
  rados_list_ctx_t listh;
  rados_nobjects_list_open(io_ctx_impl, &listh);
  NObjectIterator iter((ObjListCtx*)listh);
  if (filter.length() > 0) {
    iter.set_filter(filter);
  }
  iter.seek(pos);
  return iter;
}
LIBRADOS_IOCTX_API(14nobjects_beginEjRKN4ceph6buffer4listE);

librados::NObjectIterator librados::IoCtx::nobjects_begin(
  const ObjectCursor& cursor, const bufferlist &filter)
{
  rados_list_ctx_t listh;
  rados_nobjects_list_open(io_ctx_impl, &listh);
  NObjectIterator iter((ObjListCtx*)listh);
  if (filter.length() > 0) {
    iter.set_filter(filter);
  }
  iter.seek(cursor);
  return iter;
}
LIBRADOS_IOCTX_API(14nobjects_beginERKNS_12ObjectCursorERKN4ceph6buffer4listE);

const librados::NObjectIterator& librados::IoCtx::nobjects_end() const
{
  return NObjectIterator::__EndObjectIterator;
}
LIBRADOS_IOCTX_API_CONST(12nobjects_endEv);

int librados::IoCtx::hit_set_list(uint32_t hash, AioCompletion *c,
				  std::list< std::pair<time_t, time_t> > *pls)
{
  return io_ctx_impl->hit_set_list(hash, c->pc, pls);
}
LIBRADOS_IOCTX_API(12hit_set_listEjPNS_13AioCompletionEPNSt7__cxx114listISt4pairIllESaIS6_EEE);
LIBRADOS_IOCTX_API(12hit_set_listEjPNS_13AioCompletionEPSt4listISt4pairIllESaIS5_EE);

int librados::IoCtx::hit_set_get(uint32_t hash,  AioCompletion *c, time_t stamp,
				 bufferlist *pbl)
{
  return io_ctx_impl->hit_set_get(hash, c->pc, stamp, pbl);
}
LIBRADOS_IOCTX_API(11hit_set_getEjPNS_13AioCompletionElPN4ceph6buffer4listE);

uint64_t librados::IoCtx::get_last_version()
{
  return io_ctx_impl->last_version();
}
LIBRADOS_IOCTX_API(16get_last_versionEv);

int librados::IoCtx::aio_read(const std::string& oid, librados::AioCompletion *c,
			      bufferlist *pbl, size_t len, uint64_t off)
{
  return io_ctx_impl->aio_read(oid, c->pc, pbl, len, off,
			       io_ctx_impl->snap_seq);
}
LIBRADOS_IOCTX_API(8aio_readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(8aio_readERKSsPNS_13AioCompletionEPN4ceph6buffer4listEmm);

int librados::IoCtx::aio_read(const std::string& oid, librados::AioCompletion *c,
			      bufferlist *pbl, size_t len, uint64_t off,
			      uint64_t snapid)
{
  return io_ctx_impl->aio_read(oid, c->pc, pbl, len, off, snapid);
}
LIBRADOS_IOCTX_API(8aio_readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPN4ceph6buffer4listEmmm);
LIBRADOS_IOCTX_API(8aio_readERKSsPNS_13AioCompletionEPN4ceph6buffer4listEmmm);

int librados::IoCtx::aio_exec(const std::string& oid,
			      librados::AioCompletion *c, const char *cls,
			      const char *method, bufferlist& inbl,
			      bufferlist *outbl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_exec(obj, c->pc, cls, method, inbl, outbl);
}
LIBRADOS_IOCTX_API(8aio_execERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPKcSC_RN4ceph6buffer4listEPSF_);
LIBRADOS_IOCTX_API(8aio_execERKSsPNS_13AioCompletionEPKcS6_RN4ceph6buffer4listEPS9_);

int librados::IoCtx::aio_cmpext(const std::string& oid,
				librados::AioCompletion *c,
				uint64_t off,
				bufferlist& cmp_bl)
{
  return io_ctx_impl->aio_cmpext(oid, c->pc, off, cmp_bl);
}
LIBRADOS_IOCTX_API(10aio_cmpextERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEmRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(10aio_cmpextERKSsPNS_13AioCompletionEmRN4ceph6buffer4listE);

int librados::IoCtx::aio_sparse_read(const std::string& oid, librados::AioCompletion *c,
				     std::map<uint64_t,uint64_t> *m, bufferlist *data_bl,
				     size_t len, uint64_t off)
{
  return io_ctx_impl->aio_sparse_read(oid, c->pc,
				      m, data_bl, len, off,
				      io_ctx_impl->snap_seq);
}
LIBRADOS_IOCTX_API(15aio_sparse_readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPSt3mapImmSt4lessImESaISt4pairIKmmEEEPN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(15aio_sparse_readERKSsPNS_13AioCompletionEPSt3mapImmSt4lessImESaISt4pairIKmmEEEPN4ceph6buffer4listEmm);

int librados::IoCtx::aio_sparse_read(const std::string& oid, librados::AioCompletion *c,
				     std::map<uint64_t,uint64_t> *m, bufferlist *data_bl,
				     size_t len, uint64_t off, uint64_t snapid)
{
  return io_ctx_impl->aio_sparse_read(oid, c->pc,
				      m, data_bl, len, off, snapid);
}
LIBRADOS_IOCTX_API(15aio_sparse_readERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPSt3mapImmSt4lessImESaISt4pairIKmmEEEPN4ceph6buffer4listEmmm);
LIBRADOS_IOCTX_API(15aio_sparse_readERKSsPNS_13AioCompletionEPSt3mapImmSt4lessImESaISt4pairIKmmEEEPN4ceph6buffer4listEmmm);

int librados::IoCtx::aio_write(const std::string& oid, librados::AioCompletion *c,
			       const bufferlist& bl, size_t len, uint64_t off)
{
  return io_ctx_impl->aio_write(oid, c->pc, bl, len, off);
}
LIBRADOS_IOCTX_API(9aio_writeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERKN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(9aio_writeERKSsPNS_13AioCompletionERKN4ceph6buffer4listEmm);

int librados::IoCtx::aio_append(const std::string& oid, librados::AioCompletion *c,
				const bufferlist& bl, size_t len)
{
  return io_ctx_impl->aio_append(oid, c->pc, bl, len);
}
LIBRADOS_IOCTX_API(10aio_appendERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERKN4ceph6buffer4listEm);
LIBRADOS_IOCTX_API(10aio_appendERKSsPNS_13AioCompletionERKN4ceph6buffer4listEm);

int librados::IoCtx::aio_write_full(const std::string& oid, librados::AioCompletion *c,
				    const bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_write_full(obj, c->pc, bl);
}
LIBRADOS_IOCTX_API(14aio_write_fullERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERKN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(14aio_write_fullERKSsPNS_13AioCompletionERKN4ceph6buffer4listE);

int librados::IoCtx::aio_writesame(const std::string& oid, librados::AioCompletion *c,
				   const bufferlist& bl, size_t write_len,
				   uint64_t off)
{
  return io_ctx_impl->aio_writesame(oid, c->pc, bl, write_len, off);
}
LIBRADOS_IOCTX_API(13aio_writesameERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERKN4ceph6buffer4listEmm);
LIBRADOS_IOCTX_API(13aio_writesameERKSsPNS_13AioCompletionERKN4ceph6buffer4listEmm);

int librados::IoCtx::aio_remove(const std::string& oid, librados::AioCompletion *c)
{
  return io_ctx_impl->aio_remove(oid, c->pc);
}
LIBRADOS_IOCTX_API(10aio_removeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionE);
LIBRADOS_IOCTX_API(10aio_removeERKSsPNS_13AioCompletionE);

int librados::IoCtx::aio_remove(const std::string& oid, librados::AioCompletion *c, int flags)
{
  return io_ctx_impl->aio_remove(oid, c->pc, flags);
}
LIBRADOS_IOCTX_API(10aio_removeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEi);
LIBRADOS_IOCTX_API(10aio_removeERKSsPNS_13AioCompletionEi);

int librados::IoCtx::aio_flush_async(librados::AioCompletion *c)
{
  io_ctx_impl->flush_aio_writes_async(c->pc);
  return 0;
}
LIBRADOS_IOCTX_API(15aio_flush_asyncEPNS_13AioCompletionE);

int librados::IoCtx::aio_flush()
{
  io_ctx_impl->flush_aio_writes();
  return 0;
}
LIBRADOS_IOCTX_API(9aio_flushEv);

struct AioGetxattrDataPP {
  AioGetxattrDataPP(librados::AioCompletionImpl *c, bufferlist *_bl) :
    bl(_bl), completion(c) {}
  bufferlist *bl;
  struct librados::C_AioCompleteAndSafe completion;
};

static void rados_aio_getxattr_completepp(rados_completion_t c, void *arg) {
  AioGetxattrDataPP *cdata = reinterpret_cast<AioGetxattrDataPP*>(arg);
  int rc = rados_aio_get_return_value(c);
  if (rc >= 0) {
    rc = cdata->bl->length();
  }
  cdata->completion.finish(rc);
  delete cdata;
}

int librados::IoCtx::aio_getxattr(const std::string& oid, librados::AioCompletion *c,
				  const char *name, bufferlist& bl)
{
  // create data object to be passed to async callback
  AioGetxattrDataPP *cdata = new AioGetxattrDataPP(c->pc, &bl);
  if (!cdata) {
    return -ENOMEM;
  }
  // create completion callback
  librados::AioCompletionImpl *comp = new librados::AioCompletionImpl;
  comp->set_complete_callback(cdata, rados_aio_getxattr_completepp);
  // call actual getxattr from IoCtxImpl
  object_t obj(oid);
  return io_ctx_impl->aio_getxattr(obj, comp, name, bl);
}
LIBRADOS_IOCTX_API(12aio_getxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPKcRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(12aio_getxattrERKSsPNS_13AioCompletionEPKcRN4ceph6buffer4listE);

int librados::IoCtx::aio_getxattrs(const std::string& oid, AioCompletion *c,
				   map<std::string, bufferlist>& attrset)
{
  object_t obj(oid);
  return io_ctx_impl->aio_getxattrs(obj, c->pc, attrset);
}
LIBRADOS_IOCTX_API(13aio_getxattrsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERSt3mapIS6_N4ceph6buffer4listESt4lessIS6_ESaISt4pairIS7_SE_EEE);
LIBRADOS_IOCTX_API(13aio_getxattrsERKSsPNS_13AioCompletionERSt3mapISsN4ceph6buffer4listESt4lessISsESaISt4pairIS1_S8_EEE);

int librados::IoCtx::aio_setxattr(const std::string& oid, AioCompletion *c,
				  const char *name, bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_setxattr(obj, c->pc, name, bl);
}
LIBRADOS_IOCTX_API(12aio_setxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPKcRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(12aio_setxattrERKSsPNS_13AioCompletionEPKcRN4ceph6buffer4listE);

int librados::IoCtx::aio_rmxattr(const std::string& oid, AioCompletion *c,
				 const char *name)
{
  object_t obj(oid);
  return io_ctx_impl->aio_rmxattr(obj, c->pc, name);
}
LIBRADOS_IOCTX_API(11aio_rmxattrERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPKc);
LIBRADOS_IOCTX_API(11aio_rmxattrERKSsPNS_13AioCompletionEPKc);

int librados::IoCtx::aio_stat(const std::string& oid, librados::AioCompletion *c,
			      uint64_t *psize, time_t *pmtime)
{
  object_t obj(oid);
  return io_ctx_impl->aio_stat(obj, c->pc, psize, pmtime);
}
LIBRADOS_IOCTX_API(8aio_statERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPmPl);
LIBRADOS_IOCTX_API(8aio_statERKSsPNS_13AioCompletionEPmPl);

int librados::IoCtx::aio_cancel(librados::AioCompletion *c)
{
  return io_ctx_impl->aio_cancel(c->pc);
}
LIBRADOS_IOCTX_API(10aio_cancelEPNS_13AioCompletionE);

int librados::IoCtx::watch(const string& oid, uint64_t ver, uint64_t *cookie,
			   librados::WatchCtx *ctx)
{
  object_t obj(oid);
  return io_ctx_impl->watch(obj, cookie, ctx, NULL);
}
LIBRADOS_IOCTX_API(5watchERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmPmPNS_8WatchCtxE);
LIBRADOS_IOCTX_API(5watchERKSsmPmPNS_8WatchCtxE);

int librados::IoCtx::watch2(const string& oid, uint64_t *cookie,
			    librados::WatchCtx2 *ctx2)
{
  object_t obj(oid);
  return io_ctx_impl->watch(obj, cookie, NULL, ctx2);
}
LIBRADOS_IOCTX_API(6watch2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPmPNS_9WatchCtx2E);
LIBRADOS_IOCTX_API(6watch2ERKSsPmPNS_9WatchCtx2E);

int librados::IoCtx::watch3(const string& oid, uint64_t *cookie,
          librados::WatchCtx2 *ctx2, uint32_t timeout)
{
  object_t obj(oid);
  return io_ctx_impl->watch(obj, cookie, NULL, ctx2, timeout);
}
LIBRADOS_IOCTX_API(6watch3ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPmPNS_9WatchCtx2Ej);
LIBRADOS_IOCTX_API(6watch3ERKSsPmPNS_9WatchCtx2Ej);

int librados::IoCtx::aio_watch(const string& oid, AioCompletion *c,
                               uint64_t *cookie,
                               librados::WatchCtx2 *ctx2)
{
  object_t obj(oid);
  return io_ctx_impl->aio_watch(obj, c->pc, cookie, NULL, ctx2);
}
LIBRADOS_IOCTX_API(9aio_watchERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPmPNS_9WatchCtx2E);
LIBRADOS_IOCTX_API(9aio_watchERKSsPNS_13AioCompletionEPmPNS_9WatchCtx2E);

int librados::IoCtx::aio_watch2(const string& oid, AioCompletion *c,
                                uint64_t *cookie,
                                librados::WatchCtx2 *ctx2,
                                uint32_t timeout)
{
  object_t obj(oid);
  return io_ctx_impl->aio_watch(obj, c->pc, cookie, NULL, ctx2, timeout);
}
LIBRADOS_IOCTX_API(10aio_watch2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionEPmPNS_9WatchCtx2Ej);
LIBRADOS_IOCTX_API(10aio_watch2ERKSsPNS_13AioCompletionEPmPNS_9WatchCtx2Ej);

int librados::IoCtx::unwatch(const string& oid, uint64_t handle)
{
  return io_ctx_impl->unwatch(handle);
}
LIBRADOS_IOCTX_API(7unwatchERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEm);
LIBRADOS_IOCTX_API(7unwatchERKSsm);

int librados::IoCtx::unwatch2(uint64_t handle)
{
  return io_ctx_impl->unwatch(handle);
}
LIBRADOS_IOCTX_API(8unwatch2Em);

int librados::IoCtx::aio_unwatch(uint64_t handle, AioCompletion *c)
{
  return io_ctx_impl->aio_unwatch(handle, c->pc);
}
LIBRADOS_IOCTX_API(11aio_unwatchEmPNS_13AioCompletionE);

int librados::IoCtx::watch_check(uint64_t handle)
{
  return io_ctx_impl->watch_check(handle);
}
LIBRADOS_IOCTX_API(11watch_checkEm);

int librados::IoCtx::notify(const string& oid, uint64_t ver, bufferlist& bl)
{
  object_t obj(oid);
  return io_ctx_impl->notify(obj, bl, 0, NULL, NULL, NULL);
}
LIBRADOS_IOCTX_API(6notifyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(6notifyERKSsmRN4ceph6buffer4listE);

int librados::IoCtx::notify2(const string& oid, bufferlist& bl,
			     uint64_t timeout_ms, bufferlist *preplybl)
{
  object_t obj(oid);
  return io_ctx_impl->notify(obj, bl, timeout_ms, preplybl, NULL, NULL);
}
LIBRADOS_IOCTX_API(7notify2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERN4ceph6buffer4listEmPSB_);
LIBRADOS_IOCTX_API(7notify2ERKSsRN4ceph6buffer4listEmPS5_);

int librados::IoCtx::aio_notify(const string& oid, AioCompletion *c,
                                bufferlist& bl, uint64_t timeout_ms,
                                bufferlist *preplybl)
{
  object_t obj(oid);
  return io_ctx_impl->aio_notify(obj, c->pc, bl, timeout_ms, preplybl, NULL,
                                 NULL);
}
LIBRADOS_IOCTX_API(10aio_notifyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_13AioCompletionERN4ceph6buffer4listEmPSD_);
LIBRADOS_IOCTX_API(10aio_notifyERKSsPNS_13AioCompletionERN4ceph6buffer4listEmPS7_);

void librados::IoCtx::notify_ack(const std::string& o,
				 uint64_t notify_id, uint64_t handle,
				 bufferlist& bl)
{
  io_ctx_impl->notify_ack(o, notify_id, handle, bl);
}
LIBRADOS_IOCTX_API(10notify_ackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmmRN4ceph6buffer4listE);
LIBRADOS_IOCTX_API(10notify_ackERKSsmmRN4ceph6buffer4listE);

int librados::IoCtx::list_watchers(const std::string& oid,
                                   std::list<obj_watch_t> *out_watchers)
{
  ObjectReadOperation op;
  int r;
  op.list_watchers(out_watchers, &r);
  bufferlist bl;
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;

  return r;
}
LIBRADOS_IOCTX_API(13list_watchersERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS1_4listI11obj_watch_tSaISA_EEE);
LIBRADOS_IOCTX_API(13list_watchersERKSsPSt4listI11obj_watch_tSaIS4_EE);

int librados::IoCtx::list_snaps(const std::string& oid,
                                   snap_set_t *out_snaps)
{
  ObjectReadOperation op;
  int r;
  if (io_ctx_impl->snap_seq != CEPH_SNAPDIR)
    return -EINVAL;
  op.list_snaps(out_snaps, &r);
  bufferlist bl;
  int ret = operate(oid, &op, &bl);
  if (ret < 0)
    return ret;

  return r;
}
LIBRADOS_IOCTX_API(10list_snapsERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_10snap_set_tE);
LIBRADOS_IOCTX_API(10list_snapsERKSsPNS_10snap_set_tE);

void librados::IoCtx::set_notify_timeout(uint32_t timeout)
{
  io_ctx_impl->set_notify_timeout(timeout);
}
LIBRADOS_IOCTX_API(18set_notify_timeoutEj);

int librados::IoCtx::set_alloc_hint(const std::string& o,
                                    uint64_t expected_object_size,
                                    uint64_t expected_write_size)
{
  object_t oid(o);
  return io_ctx_impl->set_alloc_hint(oid, expected_object_size,
                                     expected_write_size, 0);
}
LIBRADOS_IOCTX_API(14set_alloc_hintERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmm);
LIBRADOS_IOCTX_API(14set_alloc_hintERKSsmm);

int librados::IoCtx::set_alloc_hint2(const std::string& o,
				     uint64_t expected_object_size,
				     uint64_t expected_write_size,
				     uint32_t flags)
{
  object_t oid(o);
  return io_ctx_impl->set_alloc_hint(oid, expected_object_size,
                                     expected_write_size, flags);
}
LIBRADOS_IOCTX_API(15set_alloc_hint2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEmmj);
LIBRADOS_IOCTX_API(15set_alloc_hint2ERKSsmmj);

void librados::IoCtx::set_assert_version(uint64_t ver)
{
  io_ctx_impl->set_assert_version(ver);
}
LIBRADOS_IOCTX_API(18set_assert_versionEm);

void librados::IoCtx::locator_set_key(const string& key)
{
  io_ctx_impl->oloc.key = key;
}
LIBRADOS_IOCTX_API(15locator_set_keyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(15locator_set_keyERKSs);

void librados::IoCtx::set_namespace(const string& nspace)
{
  io_ctx_impl->oloc.nspace = nspace;
}
LIBRADOS_IOCTX_API(13set_namespaceERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(13set_namespaceERKSs);

std::string librados::IoCtx::get_namespace() const
{
  return io_ctx_impl->oloc.nspace;
}
LIBRADOS_IOCTX_API_CONST(13get_namespaceB5cxx11Ev);
LIBRADOS_IOCTX_API_CONST(13get_namespaceEv);

int64_t librados::IoCtx::get_id()
{
  return io_ctx_impl->get_id();
}
LIBRADOS_IOCTX_API(6get_idEv);

uint32_t librados::IoCtx::get_object_hash_position(const std::string& oid)
{
  uint32_t hash;
  int r = io_ctx_impl->get_object_hash_position(oid, &hash);
  if (r < 0)
    hash = 0;
  return hash;
}
LIBRADOS_IOCTX_API(24get_object_hash_positionERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(24get_object_hash_positionERKSs);

uint32_t librados::IoCtx::get_object_pg_hash_position(const std::string& oid)
{
  uint32_t hash;
  int r = io_ctx_impl->get_object_pg_hash_position(oid, &hash);
  if (r < 0)
    hash = 0;
  return hash;
}
LIBRADOS_IOCTX_API(27get_object_pg_hash_positionERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_IOCTX_API(27get_object_pg_hash_positionERKSs);

int librados::IoCtx::get_object_hash_position2(
    const std::string& oid, uint32_t *hash_position)
{
  return io_ctx_impl->get_object_hash_position(oid, hash_position);
}
LIBRADOS_IOCTX_API(25get_object_hash_position2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPj);
LIBRADOS_IOCTX_API(25get_object_hash_position2ERKSsPj);

int librados::IoCtx::get_object_pg_hash_position2(
    const std::string& oid, uint32_t *pg_hash_position)
{
  return io_ctx_impl->get_object_pg_hash_position(oid, pg_hash_position);
}
LIBRADOS_IOCTX_API(28get_object_pg_hash_position2ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPj);
LIBRADOS_IOCTX_API(28get_object_pg_hash_position2ERKSsPj);

librados::config_t librados::IoCtx::cct()
{
  return (config_t)io_ctx_impl->client->cct;
}
LIBRADOS_IOCTX_API(3cctEv);

librados::IoCtx::IoCtx(IoCtxImpl *io_ctx_impl_)
  : io_ctx_impl(io_ctx_impl_)
{
}
LIBRADOS_IOCTX_API(C1EPNS_9IoCtxImplE);
LIBRADOS_IOCTX_API(C2EPNS_9IoCtxImplE);

void librados::IoCtx::set_osdmap_full_try()
{
  io_ctx_impl->objecter->set_osdmap_full_try();
}
LIBRADOS_IOCTX_API(19set_osdmap_full_tryEv);

void librados::IoCtx::unset_osdmap_full_try()
{
  io_ctx_impl->objecter->unset_osdmap_full_try();
}
LIBRADOS_IOCTX_API(21unset_osdmap_full_tryEv);

///////////////////////////// Rados //////////////////////////////

/// symbol versioning helpers for librados::Rados
#define LIBRADOS_RADOS_API(fn)                  \
  LIBRADOS_CXX_API(_ZN8librados5Rados, fn)
#define LIBRADOS_RADOS_API_CONST(fn)            \
  LIBRADOS_CXX_API(_ZNK8librados5Rados, fn)

void librados::Rados::version(int *major, int *minor, int *extra)
{
  rados_version(major, minor, extra);
}
LIBRADOS_RADOS_API(7versionEPiS1_S1_);

librados::Rados::Rados() : client(NULL)
{
}
LIBRADOS_RADOS_API(C1Ev);
LIBRADOS_RADOS_API(C2Ev);

librados::Rados::Rados(IoCtx &ioctx)
{
  client = ioctx.io_ctx_impl->client;
  ceph_assert(client != NULL);
  client->get();
}
LIBRADOS_RADOS_API(C1ERNS_5IoCtxE);
LIBRADOS_RADOS_API(C2ERNS_5IoCtxE);

librados::Rados::~Rados()
{
  shutdown();
LIBRADOS_RADOS_API(D1Ev);
LIBRADOS_RADOS_API(D2Ev);
}

int librados::Rados::init(const char * const id)
{
  return rados_create((rados_t *)&client, id);
}
LIBRADOS_RADOS_API(4initEPKc);

int librados::Rados::init2(const char * const name,
			   const char * const clustername, uint64_t flags)
{
  return rados_create2((rados_t *)&client, clustername, name, flags);
}
LIBRADOS_RADOS_API(5init2EPKcS2_m);

int librados::Rados::init_with_context(config_t cct_)
{
  return rados_create_with_context((rados_t *)&client, (rados_config_t)cct_);
}
LIBRADOS_RADOS_API(17init_with_contextEPv);

int librados::Rados::connect()
{
  return client->connect();
}
LIBRADOS_RADOS_API(7connectEv);

librados::config_t librados::Rados::cct()
{
  return (config_t)client->cct;
}
LIBRADOS_RADOS_API(3cctEv);

int librados::Rados::watch_flush()
{
  if (!client)
    return -EINVAL;
  return client->watch_flush();
}
LIBRADOS_RADOS_API(11watch_flushEv);

int librados::Rados::aio_watch_flush(AioCompletion *c)
{
  if (!client)
    return -EINVAL;
  return client->async_watch_flush(c->pc);
}
LIBRADOS_RADOS_API(15aio_watch_flushEPNS_13AioCompletionE);

void librados::Rados::shutdown()
{
  if (!client)
    return;
  if (client->put()) {
    client->shutdown();
    delete client;
    client = NULL;
  }
}
LIBRADOS_RADOS_API(8shutdownEv);

uint64_t librados::Rados::get_instance_id()
{
  return client->get_instance_id();
}
LIBRADOS_RADOS_API(15get_instance_idEv);

int librados::Rados::get_min_compatible_osd(int8_t* require_osd_release)
{
  return client->get_min_compatible_osd(require_osd_release);
}
LIBRADOS_RADOS_API(22get_min_compatible_osdEPa);

int librados::Rados::get_min_compatible_client(int8_t* min_compat_client,
                                               int8_t* require_min_compat_client)
{
  return client->get_min_compatible_client(min_compat_client,
                                           require_min_compat_client);
}
LIBRADOS_RADOS_API(25get_min_compatible_clientEPaS1_);

int librados::Rados::conf_read_file(const char * const path) const
{
  return rados_conf_read_file((rados_t)client, path);
}
LIBRADOS_RADOS_API_CONST(14conf_read_fileEPKc);

int librados::Rados::conf_parse_argv(int argc, const char ** argv) const
{
  return rados_conf_parse_argv((rados_t)client, argc, argv);
}
LIBRADOS_RADOS_API_CONST(15conf_parse_argvEiPPKc);

int librados::Rados::conf_parse_argv_remainder(int argc, const char ** argv,
					       const char ** remargv) const
{
  return rados_conf_parse_argv_remainder((rados_t)client, argc, argv, remargv);
}
LIBRADOS_RADOS_API_CONST(25conf_parse_argv_remainderEiPPKcS3_);

int librados::Rados::conf_parse_env(const char *name) const
{
  return rados_conf_parse_env((rados_t)client, name);
}
LIBRADOS_RADOS_API_CONST(14conf_parse_envEPKc);

int librados::Rados::conf_set(const char *option, const char *value)
{
  return rados_conf_set((rados_t)client, option, value);
}
LIBRADOS_RADOS_API(8conf_setEPKcS2_);

int librados::Rados::conf_get(const char *option, std::string &val)
{
  char *str = NULL;
  const auto& conf = client->cct->_conf;
  int ret = conf.get_val(option, &str, -1);
  if (ret) {
    free(str);
    return ret;
  }
  val = str;
  free(str);
  return 0;
}
LIBRADOS_RADOS_API(8conf_getEPKcRNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_RADOS_API(8conf_getEPKcRSs);

int librados::Rados::service_daemon_register(
  const std::string& service,  ///< service name (e.g., 'rgw')
  const std::string& name,     ///< daemon name (e.g., 'gwfoo')
  const std::map<std::string,std::string>& metadata) ///< static metadata about daemon
{
  return client->service_daemon_register(service, name, metadata);
}
LIBRADOS_RADOS_API(23service_daemon_registerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_RKSt3mapIS6_S6_St4lessIS6_ESaISt4pairIS7_S6_EEE);
LIBRADOS_RADOS_API(23service_daemon_registerERKSsS2_RKSt3mapISsSsSt4lessISsESaISt4pairIS1_SsEEE);

int librados::Rados::service_daemon_update_status(
  std::map<std::string,std::string>&& status)
{
  return client->service_daemon_update_status(std::move(status));
}
LIBRADOS_RADOS_API(28service_daemon_update_statusEOSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_St4lessIS7_ESaISt4pairIKS7_S7_EEE);
LIBRADOS_RADOS_API(28service_daemon_update_statusEOSt3mapISsSsSt4lessISsESaISt4pairIKSsSsEEE);

int librados::Rados::pool_create(const char *name)
{
  string str(name);
  return client->pool_create(str);
}
LIBRADOS_RADOS_API(11pool_createEPKc);

int librados::Rados::pool_create(const char *name, uint64_t auid)
{
  if (auid != CEPH_AUTH_UID_DEFAULT) {
    return -EINVAL;
  }
  string str(name);
  return client->pool_create(str);
}
LIBRADOS_RADOS_API(11pool_createEPKcm);

int librados::Rados::pool_create(const char *name, uint64_t auid, __u8 crush_rule)
{
  if (auid != CEPH_AUTH_UID_DEFAULT) {
    return -EINVAL;
  }
  string str(name);
  return client->pool_create(str, crush_rule);
}
LIBRADOS_RADOS_API(11pool_createEPKcmh);

int librados::Rados::pool_create_with_rule(const char *name, __u8 crush_rule)
{
  string str(name);
  return client->pool_create(str, crush_rule);
}
LIBRADOS_RADOS_API(21pool_create_with_ruleEPKch);

int librados::Rados::pool_create_async(const char *name, PoolAsyncCompletion *c)
{
  string str(name);
  return client->pool_create_async(str, c->pc);
}
LIBRADOS_RADOS_API(17pool_create_asyncEPKcPNS_19PoolAsyncCompletionE);

int librados::Rados::pool_create_async(const char *name, uint64_t auid, PoolAsyncCompletion *c)
{
  if (auid != CEPH_AUTH_UID_DEFAULT) {
    return -EINVAL;
  }
  string str(name);
  return client->pool_create_async(str, c->pc);
}
LIBRADOS_RADOS_API(17pool_create_asyncEPKcmPNS_19PoolAsyncCompletionE);

int librados::Rados::pool_create_async(const char *name, uint64_t auid, __u8 crush_rule,
				       PoolAsyncCompletion *c)
{
  if (auid != CEPH_AUTH_UID_DEFAULT) {
    return -EINVAL;
  }
  string str(name);
  return client->pool_create_async(str, c->pc, crush_rule);
}
LIBRADOS_RADOS_API(17pool_create_asyncEPKcmhPNS_19PoolAsyncCompletionE);

int librados::Rados::pool_create_with_rule_async(
  const char *name, __u8 crush_rule,
  PoolAsyncCompletion *c)
{
  string str(name);
  return client->pool_create_async(str, c->pc, crush_rule);
}
LIBRADOS_RADOS_API(27pool_create_with_rule_asyncEPKchPNS_19PoolAsyncCompletionE);

int librados::Rados::pool_get_base_tier(int64_t pool_id, int64_t* base_tier)
{
  tracepoint(librados, rados_pool_get_base_tier_enter, (rados_t)client, pool_id);
  int retval = client->pool_get_base_tier(pool_id, base_tier);
  tracepoint(librados, rados_pool_get_base_tier_exit, retval, *base_tier);
  return retval;
}
LIBRADOS_RADOS_API(18pool_get_base_tierElPl);

int librados::Rados::pool_delete(const char *name)
{
  return client->pool_delete(name);
}
LIBRADOS_RADOS_API(11pool_deleteEPKc);

int librados::Rados::pool_delete_async(const char *name, PoolAsyncCompletion *c)
{
  return client->pool_delete_async(name, c->pc);
}
LIBRADOS_RADOS_API(17pool_delete_asyncEPKcPNS_19PoolAsyncCompletionE);

int librados::Rados::pool_list(std::list<std::string>& v)
{
  std::list<std::pair<int64_t, std::string> > pools;
  int r = client->pool_list(pools);
  if (r < 0) {
    return r;
  }

  v.clear();
  for (std::list<std::pair<int64_t, std::string> >::iterator it = pools.begin();
       it != pools.end(); ++it) {
    v.push_back(it->second);
  }
  return 0;
}
LIBRADOS_RADOS_API(9pool_listERNSt7__cxx114listINS1_12basic_stringIcSt11char_traitsIcESaIcEEESaIS7_EEE);
LIBRADOS_RADOS_API(9pool_listERSt4listISsSaISsEE);

int librados::Rados::pool_list2(std::list<std::pair<int64_t, std::string> >& v)
{
  return client->pool_list(v);
}
LIBRADOS_RADOS_API(10pool_list2ERNSt7__cxx114listISt4pairIlNS1_12basic_stringIcSt11char_traitsIcESaIcEEEESaIS9_EEE);
LIBRADOS_RADOS_API(10pool_list2ERSt4listISt4pairIlSsESaIS3_EE);

int64_t librados::Rados::pool_lookup(const char *name)
{
  return client->lookup_pool(name);
}
LIBRADOS_RADOS_API(11pool_lookupEPKc);

int librados::Rados::pool_reverse_lookup(int64_t id, std::string *name)
{
  return client->pool_get_name(id, name);
}
LIBRADOS_RADOS_API(19pool_reverse_lookupElPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_RADOS_API(19pool_reverse_lookupElPSs);

int librados::Rados::mon_command(string cmd, const bufferlist& inbl,
				 bufferlist *outbl, string *outs)
{
  vector<string> cmdvec;
  cmdvec.push_back(cmd);
  return client->mon_command(cmdvec, inbl, outbl, outs);
}
LIBRADOS_RADOS_API(11mon_commandENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN4ceph6buffer4listEPS9_PS6_);
LIBRADOS_RADOS_API(11mon_commandESsRKN4ceph6buffer4listEPS3_PSs);

int librados::Rados::osd_command(int osdid, std::string cmd, const bufferlist& inbl,
                                 bufferlist *outbl, std::string *outs)
{
  vector<string> cmdvec;
  cmdvec.push_back(cmd);
  return client->osd_command(osdid, cmdvec, inbl, outbl, outs);
}
LIBRADOS_RADOS_API(11osd_commandEiNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN4ceph6buffer4listEPS9_PS6_);
LIBRADOS_RADOS_API(11osd_commandEiSsRKN4ceph6buffer4listEPS3_PSs);

int librados::Rados::mgr_command(std::string cmd, const bufferlist& inbl,
                                 bufferlist *outbl, std::string *outs)
{
  vector<string> cmdvec;
  cmdvec.push_back(cmd);
  return client->mgr_command(cmdvec, inbl, outbl, outs);
}
LIBRADOS_RADOS_API(11mgr_commandENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN4ceph6buffer4listEPS9_PS6_);
LIBRADOS_RADOS_API(11mgr_commandESsRKN4ceph6buffer4listEPS3_PSs);

int librados::Rados::pg_command(const char *pgstr, std::string cmd, const bufferlist& inbl,
                                bufferlist *outbl, std::string *outs)
{
  vector<string> cmdvec;
  cmdvec.push_back(cmd);

  pg_t pgid;
  if (!pgid.parse(pgstr))
    return -EINVAL;

  return client->pg_command(pgid, cmdvec, inbl, outbl, outs);
}
LIBRADOS_RADOS_API(10pg_commandEPKcNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERKN4ceph6buffer4listEPSB_PS8_);
LIBRADOS_RADOS_API(10pg_commandEPKcSsRKN4ceph6buffer4listEPS5_PSs);

int librados::Rados::ioctx_create(const char *name, IoCtx &io)
{
  rados_ioctx_t p;
  int ret = rados_ioctx_create((rados_t)client, name, &p);
  if (ret)
    return ret;
  io.close();
  io.io_ctx_impl = (IoCtxImpl*)p;
  return 0;
}
LIBRADOS_RADOS_API(12ioctx_createEPKcRNS_5IoCtxE);

int librados::Rados::ioctx_create2(int64_t pool_id, IoCtx &io)
{
  rados_ioctx_t p;
  int ret = rados_ioctx_create2((rados_t)client, pool_id, &p);
  if (ret)
    return ret;
  io.close();
  io.io_ctx_impl = (IoCtxImpl*)p;
  return 0;
}
LIBRADOS_RADOS_API(13ioctx_create2ElRNS_5IoCtxE);

void librados::Rados::test_blacklist_self(bool set)
{
  client->blacklist_self(set);
}
LIBRADOS_RADOS_API(19test_blacklist_selfEb);

int librados::Rados::get_pool_stats(std::list<string>& v,
				    stats_map& result)
{
  map<string,::pool_stat_t> rawresult;
  int r = client->get_pool_stats(v, rawresult);
  for (map<string,::pool_stat_t>::iterator p = rawresult.begin();
       p != rawresult.end();
       ++p) {
    pool_stat_t& pv = result[p->first];
    auto& pstat = p->second;
    store_statfs_t &statfs = pstat.store_stats;
    uint64_t allocated_bytes = pstat.get_allocated_bytes();
    // FIXME: raw_used_rate is unknown hence use 1.0 here
    // meaning we keep net amount aggregated over all replicas
    // Not a big deal so far since this field isn't exposed
    uint64_t user_bytes = pstat.get_user_bytes(1.0);

    object_stat_sum_t *sum = &p->second.stats.sum;
    pv.num_kb = shift_round_up(allocated_bytes, 10);
    pv.num_bytes = allocated_bytes;
    pv.num_objects = sum->num_objects;
    pv.num_object_clones = sum->num_object_clones;
    pv.num_object_copies = sum->num_object_copies;
    pv.num_objects_missing_on_primary = sum->num_objects_missing_on_primary;
    pv.num_objects_unfound = sum->num_objects_unfound;
    pv.num_objects_degraded = sum->num_objects_degraded;
    pv.num_rd = sum->num_rd;
    pv.num_rd_kb = sum->num_rd_kb;
    pv.num_wr = sum->num_wr;
    pv.num_wr_kb = sum->num_wr_kb;
    pv.num_user_bytes = user_bytes;
    pv.compressed_bytes_orig = statfs.data_compressed_original;
    pv.compressed_bytes = statfs.data_compressed;
    pv.compressed_bytes_alloc = statfs.data_compressed_allocated;
  }
  return r;
}
LIBRADOS_RADOS_API(14get_pool_statsERNSt7__cxx114listINS1_12basic_stringIcSt11char_traitsIcESaIcEEESaIS7_EEERSt3mapIS7_17rados_pool_stat_tSt4lessIS7_ESaISt4pairIKS7_SC_EEE);
LIBRADOS_RADOS_API(14get_pool_statsERSt4listISsSaISsEERSt3mapISs17rados_pool_stat_tSt4lessISsESaISt4pairIKSsS6_EEE);

int librados::Rados::get_pool_stats(std::list<string>& v,
				    std::map<string, stats_map>& result)
{
  stats_map m;
  int r = get_pool_stats(v, m);
  if (r < 0)
    return r;
  for (map<string,pool_stat_t>::iterator p = m.begin();
       p != m.end();
       ++p) {
    result[p->first][string()] = p->second;
  }
  return r;
}
LIBRADOS_RADOS_API(14get_pool_statsERNSt7__cxx114listINS1_12basic_stringIcSt11char_traitsIcESaIcEEESaIS7_EEERSt3mapIS7_SB_IS7_17rados_pool_stat_tSt4lessIS7_ESaISt4pairIKS7_SC_EEESE_SaISF_ISG_SJ_EEE);
LIBRADOS_RADOS_API(14get_pool_statsERSt4listISsSaISsEERSt3mapISsS5_ISs17rados_pool_stat_tSt4lessISsESaISt4pairIKSsS6_EEES8_SaIS9_ISA_SD_EEE);

int librados::Rados::get_pool_stats(std::list<string>& v,
				    string& category, // unused
				    std::map<string, stats_map>& result)
{
  return -EOPNOTSUPP;
}
LIBRADOS_RADOS_API(14get_pool_statsERNSt7__cxx114listINS1_12basic_stringIcSt11char_traitsIcESaIcEEESaIS7_EEERS7_RSt3mapIS7_SC_IS7_17rados_pool_stat_tSt4lessIS7_ESaISt4pairIKS7_SD_EEESF_SaISG_ISH_SK_EEE);
LIBRADOS_RADOS_API(14get_pool_statsERSt4listISsSaISsEERSsRSt3mapISsS6_ISs17rados_pool_stat_tSt4lessISsESaISt4pairIKSsS7_EEES9_SaISA_ISB_SE_EEE);

bool librados::Rados::get_pool_is_selfmanaged_snaps_mode(const std::string& pool)
{
  return client->get_pool_is_selfmanaged_snaps_mode(pool);
}
LIBRADOS_RADOS_API(34get_pool_is_selfmanaged_snaps_modeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_RADOS_API(34get_pool_is_selfmanaged_snaps_modeERKSs);

int librados::Rados::cluster_stat(cluster_stat_t& result)
{
  ceph_statfs stats;
  int r = client->get_fs_stats(stats);
  result.kb = stats.kb;
  result.kb_used = stats.kb_used;
  result.kb_avail = stats.kb_avail;
  result.num_objects = stats.num_objects;
  return r;
}
LIBRADOS_RADOS_API(12cluster_statER20rados_cluster_stat_t);

int librados::Rados::cluster_fsid(string *fsid)
{
  return client->get_fsid(fsid);
}
LIBRADOS_RADOS_API(12cluster_fsidEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_RADOS_API(12cluster_fsidEPSs);

/// symbol versioning helpers for librados::PlacementGroup
#define LIBRADOS_PLACEMENT_GROUP_API(fn)        \
  LIBRADOS_CXX_API(_ZN8librados14PlacementGroup, fn)

namespace librados {
  struct PlacementGroupImpl {
    pg_t pgid;
  };

  PlacementGroup::PlacementGroup()
    : impl{new PlacementGroupImpl}
  {}
  LIBRADOS_PLACEMENT_GROUP_API(C1Ev);
  LIBRADOS_PLACEMENT_GROUP_API(C2Ev);

  PlacementGroup::PlacementGroup(const PlacementGroup& pg)
    : impl{new PlacementGroupImpl}
  {
    impl->pgid = pg.impl->pgid;
  }
  LIBRADOS_PLACEMENT_GROUP_API(C1ERKS0_);
  LIBRADOS_PLACEMENT_GROUP_API(C2ERKS0_);

  PlacementGroup::~PlacementGroup()
  {}
  LIBRADOS_PLACEMENT_GROUP_API(D1Ev);
  LIBRADOS_PLACEMENT_GROUP_API(D2Ev);

  bool PlacementGroup::parse(const char* s)
  {
    return impl->pgid.parse(s);
  }
  LIBRADOS_PLACEMENT_GROUP_API(5parseEPKc);
}

std::ostream& librados::operator<<(std::ostream& out,
				   const librados::PlacementGroup& pg)
{
  return out << pg.impl->pgid;
}
LIBRADOS_CXX_API(_ZN8libradoslsERSoRKNS_, 14PlacementGroupE);

int librados::Rados::get_inconsistent_pgs(int64_t pool_id,
					  std::vector<PlacementGroup>* pgs)
{
  std::vector<string> pgids;
  if (auto ret = client->get_inconsistent_pgs(pool_id, &pgids); ret) {
    return ret;
  }
  for (const auto& pgid : pgids) {
    librados::PlacementGroup pg;
    if (!pg.parse(pgid.c_str())) {
      return -EINVAL;
    }
    pgs->emplace_back(pg);
  }
  return 0;
}
LIBRADOS_RADOS_API(20get_inconsistent_pgsElPSt6vectorINS_14PlacementGroupESaIS2_EE);

int librados::Rados::get_inconsistent_objects(const PlacementGroup& pg,
					      const object_id_t &start_after,
					      unsigned max_return,
					      AioCompletion *c,
					      std::vector<inconsistent_obj_t>* objects,
					      uint32_t* interval)
{
  IoCtx ioctx;
  const pg_t pgid = pg.impl->pgid;
  int r = ioctx_create2(pgid.pool(), ioctx);
  if (r < 0) {
    return r;
  }

  return ioctx.io_ctx_impl->get_inconsistent_objects(pgid,
						     start_after,
						     max_return,
						     c->pc,
						     objects,
						     interval);
}
LIBRADOS_RADOS_API(24get_inconsistent_objectsERKNS_14PlacementGroupERKNS_11object_id_tEjPNS_13AioCompletionEPSt6vectorINS_18inconsistent_obj_tESaISA_EEPj);

int librados::Rados::get_inconsistent_snapsets(const PlacementGroup& pg,
					       const object_id_t &start_after,
					       unsigned max_return,
					       AioCompletion *c,
					       std::vector<inconsistent_snapset_t>* snapsets,
					       uint32_t* interval)
{
  IoCtx ioctx;
  const pg_t pgid = pg.impl->pgid;
  int r = ioctx_create2(pgid.pool(), ioctx);
  if (r < 0) {
    return r;
  }

  return ioctx.io_ctx_impl->get_inconsistent_snapsets(pgid,
						      start_after,
						      max_return,
						      c->pc,
						      snapsets,
						      interval);
}
LIBRADOS_RADOS_API(25get_inconsistent_snapsetsERKNS_14PlacementGroupERKNS_11object_id_tEjPNS_13AioCompletionEPSt6vectorINS_22inconsistent_snapset_tESaISA_EEPj);

int librados::Rados::wait_for_latest_osdmap()
{
  return client->wait_for_latest_osdmap();
}
LIBRADOS_RADOS_API(22wait_for_latest_osdmapEv);

int librados::Rados::blacklist_add(const std::string& client_address,
				   uint32_t expire_seconds)
{
  return client->blacklist_add(client_address, expire_seconds);
}
LIBRADOS_RADOS_API(13blacklist_addERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj);
LIBRADOS_RADOS_API(13blacklist_addERKSsj);

librados::PoolAsyncCompletion *librados::Rados::pool_async_create_completion()
{
  PoolAsyncCompletionImpl *c = new PoolAsyncCompletionImpl;
  return new PoolAsyncCompletion(c);
}
LIBRADOS_RADOS_API(28pool_async_create_completionEv);

librados::AioCompletion *librados::Rados::aio_create_completion()
{
  AioCompletionImpl *c = new AioCompletionImpl;
  return new AioCompletion(c);
}
LIBRADOS_RADOS_API(21aio_create_completionEv);

librados::AioCompletion *librados::Rados::aio_create_completion(void *cb_arg,
								callback_t cb_complete,
								callback_t cb_safe)
{
  AioCompletionImpl *c;
  int r = rados_aio_create_completion(cb_arg, cb_complete, cb_safe, (void**)&c);
  ceph_assert(r == 0);
  return new AioCompletion(c);
}
LIBRADOS_RADOS_API(21aio_create_completionEPvPFvS1_S1_ES3_);

librados::ObjectOperation::ObjectOperation()
{
  impl = new ObjectOperationImpl;
}
LIBRADOS_OBJECT_OPERATION_API(C1Ev);
LIBRADOS_OBJECT_OPERATION_API(C2Ev);

librados::ObjectOperation::~ObjectOperation()
{
  delete impl;
}
LIBRADOS_OBJECT_OPERATION_API(D0Ev);
LIBRADOS_OBJECT_OPERATION_API(D1Ev);
LIBRADOS_OBJECT_OPERATION_API(D2Ev);


///////////////////////////// ListObject //////////////////////////////

/// symbol versioning helpers for librados::ListObject
#define LIBRADOS_LIST_OBJECT_API(fn)            \
  LIBRADOS_CXX_API(_ZN8librados10ListObject, fn)
#define LIBRADOS_LIST_OBJECT_API_CONST(fn)      \
  LIBRADOS_CXX_API(_ZNK8librados10ListObject, fn)

librados::ListObject::ListObject() : impl(NULL)
{
}
LIBRADOS_LIST_OBJECT_API(C1Ev);
LIBRADOS_LIST_OBJECT_API(C2Ev);

librados::ListObject::ListObject(librados::ListObjectImpl *i): impl(i)
{
}
LIBRADOS_LIST_OBJECT_API(C1EPNS_14ListObjectImplE);
LIBRADOS_LIST_OBJECT_API(C2EPNS_14ListObjectImplE);

librados::ListObject::ListObject(const ListObject& rhs)
{
  if (rhs.impl == NULL) {
    impl = NULL;
    return;
  }
  impl = new ListObjectImpl();
  *impl = *(rhs.impl);
}
LIBRADOS_LIST_OBJECT_API(C1ERKS0_);
LIBRADOS_LIST_OBJECT_API(C2ERKS0_);

librados::ListObject& librados::ListObject::operator=(const ListObject& rhs)
{
  if (rhs.impl == NULL) {
    delete impl;
    impl = NULL;
    return *this;
  }
  if (impl == NULL)
    impl = new ListObjectImpl();
  *impl = *(rhs.impl);
  return *this;
}
LIBRADOS_LIST_OBJECT_API(aSERKS0_);

librados::ListObject::~ListObject()
{
  if (impl)
    delete impl;
  impl = NULL;
}
LIBRADOS_LIST_OBJECT_API(D1Ev);
LIBRADOS_LIST_OBJECT_API(D2Ev);

const std::string& librados::ListObject::get_nspace() const
{
  return impl->get_nspace();
}
LIBRADOS_LIST_OBJECT_API_CONST(10get_nspaceB5cxx11Ev);
LIBRADOS_LIST_OBJECT_API_CONST(10get_nspaceEv);

const std::string& librados::ListObject::get_oid() const
{
  return impl->get_oid();
}
LIBRADOS_LIST_OBJECT_API_CONST(7get_oidB5cxx11Ev);
LIBRADOS_LIST_OBJECT_API_CONST(7get_oidEv);

const std::string& librados::ListObject::get_locator() const
{
  return impl->get_locator();
}
LIBRADOS_LIST_OBJECT_API_CONST(11get_locatorB5cxx11Ev);
LIBRADOS_LIST_OBJECT_API_CONST(11get_locatorEv);

std::ostream& librados::operator<<(std::ostream& out, const librados::ListObject& lop)
{
  out << *(lop.impl);
  return out;
}
LIBRADOS_CXX_API(_ZN8libradoslsERSoRKNS_, 10ListObjectE);

/// symbol versioning helpers for librados::ObjectCursor
#define LIBRADOS_OBJECT_CURSOR_API(fn)          \
  LIBRADOS_CXX_API(_ZN8librados12ObjectCursor, fn)
#define LIBRADOS_OBJECT_CURSOR_API_CONST(fn)    \
  LIBRADOS_CXX_API(_ZNK8librados12ObjectCursor, fn)

librados::ObjectCursor::ObjectCursor()
{
  c_cursor = (rados_object_list_cursor)new hobject_t();
}
LIBRADOS_OBJECT_CURSOR_API(C1Ev);
LIBRADOS_OBJECT_CURSOR_API(C2Ev);

librados::ObjectCursor::~ObjectCursor()
{
  hobject_t *h = (hobject_t *)c_cursor;
  delete h;
}
LIBRADOS_OBJECT_CURSOR_API(D1Ev);
LIBRADOS_OBJECT_CURSOR_API(D2Ev);

librados::ObjectCursor::ObjectCursor(rados_object_list_cursor c)
{
  if (!c) {
    c_cursor = nullptr;
  } else {
    c_cursor = (rados_object_list_cursor)new hobject_t(*(hobject_t *)c);
  }
}
LIBRADOS_OBJECT_CURSOR_API(C1EPv);
LIBRADOS_OBJECT_CURSOR_API(C2EPv);

librados::ObjectCursor& librados::ObjectCursor::operator=(const librados::ObjectCursor& rhs)
{
  if (rhs.c_cursor != nullptr) {
    hobject_t *h = (hobject_t*)rhs.c_cursor;
    c_cursor = (rados_object_list_cursor)(new hobject_t(*h));
  } else {
    c_cursor = nullptr;
  }
  return *this;
}
LIBRADOS_OBJECT_CURSOR_API(aSERKS0_);

bool librados::ObjectCursor::operator<(const librados::ObjectCursor &rhs) const
{
  const hobject_t lhs_hobj = (c_cursor == nullptr) ? hobject_t() : *((hobject_t*)c_cursor);
  const hobject_t rhs_hobj = (rhs.c_cursor == nullptr) ? hobject_t() : *((hobject_t*)(rhs.c_cursor));
  return lhs_hobj < rhs_hobj;
}
LIBRADOS_OBJECT_CURSOR_API_CONST(ltERKS0_);

bool librados::ObjectCursor::operator==(const librados::ObjectCursor &rhs) const
{
  const hobject_t lhs_hobj = (c_cursor == nullptr) ? hobject_t() : *((hobject_t*)c_cursor);
  const hobject_t rhs_hobj = (rhs.c_cursor == nullptr) ? hobject_t() : *((hobject_t*)(rhs.c_cursor));
  return cmp(lhs_hobj, rhs_hobj) == 0;
}
LIBRADOS_OBJECT_CURSOR_API_CONST(eqERKS0_);

librados::ObjectCursor::ObjectCursor(const librados::ObjectCursor &rhs)
{
  *this = rhs;
}
LIBRADOS_OBJECT_CURSOR_API(C1ERKS0_);
LIBRADOS_OBJECT_CURSOR_API(C2ERKS0_);

librados::ObjectCursor librados::IoCtx::object_list_begin()
{
  hobject_t *h = new hobject_t(io_ctx_impl->objecter->enumerate_objects_begin());
  ObjectCursor oc;
  oc.set((rados_object_list_cursor)h);
  return oc;
}
LIBRADOS_IOCTX_API(17object_list_beginEv);

librados::ObjectCursor librados::IoCtx::object_list_end()
{
  hobject_t *h = new hobject_t(io_ctx_impl->objecter->enumerate_objects_end());
  librados::ObjectCursor oc;
  oc.set((rados_object_list_cursor)h);
  return oc;
}
LIBRADOS_IOCTX_API(15object_list_endEv);

void librados::ObjectCursor::set(rados_object_list_cursor c)
{
  delete (hobject_t*)c_cursor;
  c_cursor = c;
}
LIBRADOS_OBJECT_CURSOR_API(3setEPv);

string librados::ObjectCursor::to_str() const
{
  stringstream ss;
  ss << *(hobject_t *)c_cursor;
  return ss.str();
}
LIBRADOS_OBJECT_CURSOR_API_CONST(6to_strB5cxx11Ev);
LIBRADOS_OBJECT_CURSOR_API_CONST(6to_strEv);

bool librados::ObjectCursor::from_str(const string& s)
{
  if (s.empty()) {
    *(hobject_t *)c_cursor = hobject_t();
    return true;
  }
  return ((hobject_t *)c_cursor)->parse(s);
}
LIBRADOS_OBJECT_CURSOR_API(8from_strERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE);
LIBRADOS_OBJECT_CURSOR_API(8from_strERKSs);

CEPH_RADOS_API std::ostream& librados::operator<<(std::ostream& os, const librados::ObjectCursor& oc)
{
  if (oc.c_cursor) {
    os << *(hobject_t *)oc.c_cursor;
  } else {
    os << hobject_t();
  }
  return os;
}
LIBRADOS_CXX_API(_ZN8libradoslsERSoRKNS_, 12ObjectCursorE);

bool librados::IoCtx::object_list_is_end(const ObjectCursor &oc)
{
  hobject_t *h = (hobject_t *)oc.c_cursor;
  return h->is_max();
}
LIBRADOS_IOCTX_API(18object_list_is_endERKNS_12ObjectCursorE);

int librados::IoCtx::object_list(const ObjectCursor &start,
                const ObjectCursor &finish,
                const size_t result_item_count,
                const bufferlist &filter,
                std::vector<ObjectItem> *result,
                ObjectCursor *next)
{
  ceph_assert(result != nullptr);
  ceph_assert(next != nullptr);
  result->clear();

  C_SaferCond cond;
  hobject_t next_hash;
  std::list<librados::ListObjectImpl> obj_result;
  io_ctx_impl->objecter->enumerate_objects(
      io_ctx_impl->poolid,
      io_ctx_impl->oloc.nspace,
      *((hobject_t*)start.c_cursor),
      *((hobject_t*)finish.c_cursor),
      result_item_count,
      filter,
      &obj_result,
      &next_hash,
      &cond);

  int r = cond.wait();
  if (r < 0) {
    next->set((rados_object_list_cursor)(new hobject_t(hobject_t::get_max())));
    return r;
  }

  next->set((rados_object_list_cursor)(new hobject_t(next_hash)));

  for (std::list<librados::ListObjectImpl>::iterator i = obj_result.begin();
       i != obj_result.end(); ++i) {
    ObjectItem oi;
    oi.oid = i->oid;
    oi.nspace = i->nspace;
    oi.locator = i->locator;
    result->push_back(oi);
  }

  return obj_result.size();
}
LIBRADOS_IOCTX_API(11object_listERKNS_12ObjectCursorES3_mRKN4ceph6buffer4listEPSt6vectorINS_10ObjectItemESaISA_EEPS1_);

void librados::IoCtx::object_list_slice(
    const ObjectCursor start,
    const ObjectCursor finish,
    const size_t n,
    const size_t m,
    ObjectCursor *split_start,
    ObjectCursor *split_finish)
{
  ceph_assert(split_start != nullptr);
  ceph_assert(split_finish != nullptr);

  io_ctx_impl->object_list_slice(
      *((hobject_t*)(start.c_cursor)),
      *((hobject_t*)(finish.c_cursor)),
      n,
      m,
      (hobject_t*)(split_start->c_cursor),
      (hobject_t*)(split_finish->c_cursor));
}
LIBRADOS_IOCTX_API(17object_list_sliceENS_12ObjectCursorES1_mmPS1_S2_);

int librados::IoCtx::application_enable(const std::string& app_name,
                                        bool force)
{
  return io_ctx_impl->application_enable(app_name, force);
}
LIBRADOS_IOCTX_API(18application_enableERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb);
LIBRADOS_IOCTX_API(18application_enableERKSsb);

int librados::IoCtx::application_enable_async(const std::string& app_name,
                                              bool force,
                                              PoolAsyncCompletion *c)
{
  io_ctx_impl->application_enable_async(app_name, force, c->pc);
  return 0;
}
LIBRADOS_IOCTX_API(24application_enable_asyncERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEbPNS_19PoolAsyncCompletionE);
LIBRADOS_IOCTX_API(24application_enable_asyncERKSsbPNS_19PoolAsyncCompletionE);

int librados::IoCtx::application_list(std::set<std::string> *app_names)
{
  return io_ctx_impl->application_list(app_names);
}
LIBRADOS_IOCTX_API(16application_listEPSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS7_ESaIS7_EE);
LIBRADOS_IOCTX_API(16application_listEPSt3setISsSt4lessISsESaISsEE);

int librados::IoCtx::application_metadata_get(const std::string& app_name,
                                              const std::string &key,
                                              std::string* value)
{
  return io_ctx_impl->application_metadata_get(app_name, key, value);
}
LIBRADOS_IOCTX_API(24application_metadata_getERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_PS6_);
LIBRADOS_IOCTX_API(24application_metadata_getERKSsS2_PSs);

int librados::IoCtx::application_metadata_set(const std::string& app_name,
                                              const std::string &key,
                                              const std::string& value)
{
  return io_ctx_impl->application_metadata_set(app_name, key, value);
}
LIBRADOS_IOCTX_API(24application_metadata_setERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_S8_);
LIBRADOS_IOCTX_API(24application_metadata_setERKSsS2_S2_);

int librados::IoCtx::application_metadata_remove(const std::string& app_name,
                                                 const std::string &key)
{
  return io_ctx_impl->application_metadata_remove(app_name, key);
}
LIBRADOS_IOCTX_API(27application_metadata_removeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_);
LIBRADOS_IOCTX_API(27application_metadata_removeERKSsS2_);

int librados::IoCtx::application_metadata_list(const std::string& app_name,
                                               std::map<std::string, std::string> *values)
{
  return io_ctx_impl->application_metadata_list(app_name, values);
}
LIBRADOS_IOCTX_API(25application_metadata_listERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPSt3mapIS6_S6_St4lessIS6_ESaISt4pairIS7_S6_EEE);
LIBRADOS_IOCTX_API(25application_metadata_listERKSsPSt3mapISsSsSt4lessISsESaISt4pairIS1_SsEEE);
