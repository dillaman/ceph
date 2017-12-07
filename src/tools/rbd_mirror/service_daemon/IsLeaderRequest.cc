// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd_mirror/service_daemon/IsLeaderRequest.h"
#include "include/Context.h"
#include "common/debug.h"
#include "common/errno.h"
#include "json_spirit/json_spirit.h"
#include "librbd/Utils.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::service_daemon::IsLeaderRequest: " \
                           << this << " " << __func__ << ": "

namespace rbd {
namespace mirror {
namespace service_daemon {

using librbd::util::create_rados_callback;

template <typename I>
void IsLeaderRequest<I>::send() {
  service_dump();
}

template <typename I>
void IsLeaderRequest<I>::service_dump() {
  dout(10) << dendl;

  std::string cmd = "{\"prefix\": \"service dump\", \"format\": \"json\"}";
  bufferlist in_bl;

  auto aio_comp = create_rados_callback<
    IsLeaderRequest<I>, &IsLeaderRequest<I>::handle_service_dump>(this);
  int r = m_rados->mgr_command_async(cmd, in_bl, &m_out_bl, &m_out_status,
                                     aio_comp);
  assert(r == 0);
  aio_comp->release();
}

template <typename I>
void IsLeaderRequest<I>::handle_service_dump(int r) {
  dout(10) << "r=" << r << dendl;
  if (r < 0) {
    derr << "failed to dump service daemons: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  std::set<std::pair<uint64_t, uint64_t>> instances;

  /**
   * Expected format:
   *  {
   *    "services": {
   *      "rbd-mirror": {
   *        "daemons": {
   *          "<name>": {
   *            "start_epoch": <epoch>,
   *            "gid": <instance id>,
   *            ...
   *          },
   *          ...
   *        }
   *      },
   *      ...
   *    }
   *  }
   */
  std::string response(m_out_bl.c_str(), m_out_bl.length());
  json_spirit::mValue root;
  if (!json_spirit::read(response, root)) {
    derr << "unparseable JSON: " << response << dendl;
    finish(-EINVAL);
    return;
  } else if (root.type() != json_spirit::obj_type) {
    derr << "response not JSON object: " << response << dendl;
    finish(-EINVAL);
    return;
  }

  auto& root_object = root.get_obj();
  auto services_it = root_object.find("services");
  if (services_it == root_object.end() ||
      services_it->second.type() != json_spirit::obj_type) {
    derr << "response missing 'services' object key" << dendl;
    finish(-EINVAL);
    return;
  }

  auto& service_object = services_it->second.get_obj();
  auto rbd_mirror_it = service_object.find("rbd-mirror");
  if (rbd_mirror_it == service_object.end()) {
    dout(5) << "rbd-mirror daemons not yet registered" << dendl;
    finish(-ENOENT);
    return;
  } else if (rbd_mirror_it->second.type() != json_spirit::obj_type) {
    derr << "response missing 'rbd-mirror' object key" << dendl;
    finish(-EINVAL);
    return;
  }

  auto& rbd_mirror_object = rbd_mirror_it->second.get_obj();
  auto daemons_it = rbd_mirror_object.find("daemons");
  if (daemons_it == rbd_mirror_object.end() ||
      daemons_it->second.type() != json_spirit::obj_type) {
    derr << "response missing 'daemons' object key" << dendl;
    finish(-EINVAL);
    return;
  }

  auto& daemons_object = daemons_it->second.get_obj();
  for (auto& instance : daemons_object) {
    dout(10) << "parsing instance: " << instance.first << dendl;
    if (instance.second.type() != json_spirit::obj_type) {
      derr << "response contains non-object instance " << instance.first
           << dendl;
      continue;
    }

    auto& instance_object = instance.second.get_obj();
    auto start_epoch_it = instance_object.find("start_epoch");
    if (start_epoch_it == instance_object.end() ||
        start_epoch_it->second.type() != json_spirit::int_type) {
      derr << "cannot locate 'start_epoch' in instance " << instance.first
           << dendl;
      continue;
    }

    auto gid_it = instance_object.find("gid");
    if (gid_it == instance_object.end() ||
        gid_it->second.type() != json_spirit::int_type) {
      derr << "cannot locate 'gid' in instance " << instance.first
           << dendl;
      continue;
    }

    instances.insert({start_epoch_it->second.get_uint64(),
                      gid_it->second.get_uint64()});
  }

  // leader is the oldest daemon (as ordered by start_epoch followed by gid)
  *m_is_leader = (!instances.empty() &&
                  instances.begin()->second == m_rados->get_instance_id());
  finish(0);
}

template <typename I>
void IsLeaderRequest<I>::finish(int r) {
  dout(10) << "r=" << r << dendl;

  m_on_finish->complete(r);
  delete this;
}

} // namespace service_daemon
} // namespace mirror
} // namespace rbd

template class rbd::mirror::service_daemon::IsLeaderRequest<librbd::ImageCtx>;
