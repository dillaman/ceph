// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_MANAGED_LOCK_RELEASE_REQUEST_H
#define CEPH_LIBRBD_MANAGED_LOCK_RELEASE_REQUEST_H

#include "include/rados/librados.hpp"
#include <string>

class Context;
class ContextWQ;

namespace librbd {

template <typename> class Watcher;

namespace managed_lock {

template <typename ImageCtxT>
class ReleaseRequest {
public:
  static ReleaseRequest* create(librados::IoCtx& ioctx,
                                Watcher<ImageCtxT> *watcher,
                                ContextWQ *work_queue,
                                const std::string& oid,
                                const std::string& cookie,
                                Context *on_finish);

  ~ReleaseRequest();
  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * UNLOCK
   *    |
   *    v
   * <finish>
   *
   * @endverbatim
   */

  ReleaseRequest(librados::IoCtx& ioctx, Watcher<ImageCtxT> *watcher,
                 ContextWQ *work_queue, const std::string& oid,
                 const std::string& cookie, Context *on_finish);

  librados::IoCtx& m_ioctx;
  Watcher<ImageCtxT> *m_watcher;
  std::string m_oid;
  std::string m_cookie;
  Context *m_on_finish;

  void send_unlock();
  void handle_unlock(int r);

  void finish();

};

} // namespace managed_lock
} // namespace librbd

#endif // CEPH_LIBRBD_MANAGED_LOCK_RELEASE_REQUEST_H
