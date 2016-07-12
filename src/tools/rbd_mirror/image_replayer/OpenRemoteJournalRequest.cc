// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "OpenRemoteJournalRequest.h"
#include "OpenJournalRequest.h"
#include "common/debug.h"
#include "common/dout.h"
#include "common/errno.h"
#include "cls/rbd/cls_rbd_client.h"
#include "journal/Journaler.h"
#include "librbd/Utils.h"

#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::image_replayer::OpenRemoteJournalRequest: " \
                           << this << " " << __func__

namespace rbd {
namespace mirror {
namespace image_replayer {

using librbd::util::create_context_callback;
using librbd::util::create_rados_ack_callback;

template <typename I>
void OpenRemoteJournalRequest<I>::send() {
  open_remote_journal();
}

template <typename I>
void OpenRemoteJournalRequest<I>::open_remote_journal() {
  dout(20) << dendl;

  Context *ctx = create_context_callback<
    OpenRemoteJournalRequest<I>,
    &OpenRemoteJournalRequest<I>::handle_open_remote_journal>(this);
  OpenJournalRequest<I> *request = OpenJournalRequest<I>::create(
    m_remote_io_ctx, m_remote_image_id, m_local_mirror_uuid, m_work_queue,
    m_timer, m_timer_lock, m_mirror_peer_tag_data_list,
    m_mirror_peer_client_meta, &m_journaler, ctx);
  request->send();
}

template <typename I>
void OpenRemoteJournalRequest<I>::handle_open_remote_journal(int r) {
}

template <typename I>
void OpenRemoteJournalRequest<I>::shut_down_remote_journal() {
  dout(20) << dendl;
}

template <typename I>
void OpenRemoteJournalRequest<I>::handle_shut_down_remote_journal(int r) {
  dout(20) << ": r=" << r << dendl;
}

template <typename I>
void OpenRemoteJournalRequest<I>::save_result(int r) {
  if (m_ret_val < 0 || r >= 0) {
    return;
  }

  dout(20) << ": r=" << r << dendl;
  m_ret_val = r;
}

template <typename I>
void OpenRemoteJournalRequest<I>::finish() {
  dout(20) << ": r=" << m_ret_val << dendl;
  m_on_finish->complete(m_ret_val);
  delete this;
}

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

template class rbd::mirror::image_replayer::OpenRemoteJournalRequest<librbd::ImageCtx>;
