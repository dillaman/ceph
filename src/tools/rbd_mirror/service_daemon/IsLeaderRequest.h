// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_MIRROR_SERVICE_DAEMON_IS_LEADER_REQUEST_H
#define CEPH_RBD_MIRROR_SERVICE_DAEMON_IS_LEADER_REQUEST_H

#include "include/buffer.h"
#include "tools/rbd_mirror/types.h"
#include <string>

struct Context;
namespace librbd { struct ImageCtx; }

namespace rbd {
namespace mirror {
namespace service_daemon {

template <typename ImageCtxT = librbd::ImageCtx>
class IsLeaderRequest {
public:
  static IsLeaderRequest* create(RadosRef rados, bool* is_leader,
                                 Context* on_finish) {
    return new IsLeaderRequest(rados, is_leader, on_finish);
  }

  IsLeaderRequest(RadosRef rados, bool* is_leader, Context* on_finish)
    : m_rados(rados), m_is_leader(is_leader),
      m_on_finish(on_finish) {
  }

  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * SERVICE_DUMP
   *    |
   *    v
   * <finish>
   *
   * @endverbatim
   */

  RadosRef m_rados;
  bool* m_is_leader;
  Context* m_on_finish;

  ceph::bufferlist m_out_bl;
  std::string m_out_status;

  void service_dump();
  void handle_service_dump(int r);

  void finish(int r);
};

} // namespace service_daemon
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::service_daemon::IsLeaderRequest<librbd::ImageCtx>;

#endif // CEPH_RBD_MIRROR_SERVICE_DAEMON_IS_LEADER_REQUEST_H
