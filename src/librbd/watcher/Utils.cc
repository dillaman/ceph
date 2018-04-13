// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/watcher/Utils.h"
#include "common/dout.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::watcher::util::C_NotifyAck " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace watcher {
namespace util {

C_NotifyAckBase::C_NotifyAckBase(CephContext* cct, uint64_t notify_id,
                                 uint64_t handle)
  : cct(cct), notify_id(notify_id), handle(handle) {
  ldout(cct, 10) << "id=" << notify_id << ", " << "handle=" << handle << dendl;
}

void C_NotifyAckBase::finish(int r) {
  ldout(cct, 10) << "r=" << r << dendl;
}

} // namespace util
} // namespace watcher
} // namespace librbd
