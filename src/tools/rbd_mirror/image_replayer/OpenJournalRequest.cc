// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "OpenJournalRequest.h"
#include "Utils.h"
#include "common/debug.h"
#include "common/dout.h"
#include "common/errno.h"
#include "global/global_context.h"
#include "cls/journal/cls_journal_client.h"
#include "journal/Journaler.h"
#include "librbd/Journal.h"
#include "librbd/Utils.h"

#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::image_replayer::OpenJournalRequest: " \
                           << this << " " << __func__

namespace rbd {
namespace mirror {
namespace image_replayer {

using librbd::util::create_context_callback;

template <typename I>
OpenJournalRequest<I>::OpenJournalRequest(librados::IoCtx &io_ctx,
                                          const std::string &journal_id,
                                          const std::string &client_id,
                                          ContextWQ *work_queue,
                                          SafeTimer *timer,
                                          Mutex *timer_lock,
                                          TagDataList *tag_data_list,
                                          MirrorPeerClientMeta *client_meta,
                                          Journaler **opened_journaler,
                                          Context *on_finish)
  : m_io_ctx(io_ctx), m_journal_id(journal_id), m_client_id(client_id),
    m_work_queue(work_queue), m_timer(timer), m_timer_lock(timer_lock),
    m_tag_data_list(tag_data_list), m_mirror_peer_client_meta(client_meta),
    m_opened_journaler(opened_journaler), m_on_finish(on_finish) {
}

template <typename I>
void OpenJournalRequest<I>::send() {
  init_journaler();
}

template <typename I>
void OpenJournalRequest<I>::init_journaler() {
  dout(20) << dendl;

  double commit_interval = g_ceph_context->_conf->rbd_journal_commit_age;
  m_journaler = utils::create_journaler<I>(m_work_queue, m_timer, m_timer_lock,
                                           m_io_ctx, nullptr, m_journal_id,
                                           m_client_id, commit_interval);

  Context *ctx = create_context_callback<
    OpenJournalRequest<I>, &OpenJournalRequest<I>::handle_init_journaler>(this);
  m_journaler->init(ctx);
}

template <typename I>
void OpenJournalRequest<I>::handle_init_journaler(int r) {
  dout(20) << ": r=" << r << dendl;

  if (r < 0) {
    derr << ": failed to initialize journaler: " << cpp_strerror(r) << dendl;
    save_result(r);
    shut_down_journaler();
    return;
  }

  // decode the master image journal client
  librbd::journal::ClientData image_client_data;
  r = decode_client_data(librbd::Journal<>::IMAGE_CLIENT_ID, "image",
                         false, &image_client_data);
  if (r < 0) {
    save_result(r);
    shut_down_journaler();
    return;
  }

  librbd::journal::ImageClientMeta *image_client_meta =
    boost::get<librbd::journal::ImageClientMeta>(
      &image_client_data.client_meta);
  if (image_client_meta == nullptr) {
    derr << ": unknown image journal client registration" << dendl;
    save_result(-EINVAL);
    shut_down_journaler();
    return;
  }

  m_tag_class = image_client_meta->tag_class;
  dout(10) << ": tag class=" << m_tag_class << dendl;

  if (m_client_id == librbd::Journal<>::IMAGE_CLIENT_ID) {
    tag_list();
    return;
  }

  // decode the mirror peer journal client
  librbd::journal::ClientData mirror_peer_client_data;
  r = decode_client_data(m_client_id, "mirror peer", true,
                         &mirror_peer_client_data);
  if (r == -ENOENT) {
    dout(10) << ": mirror peer client not registered" << dendl;
    m_tag_data_list->clear();
    *m_mirror_peer_client_meta = MirrorPeerClientMeta();
    shut_down_journaler();
    return;
  } else if (r < 0) {
    derr << ": failed to retrieve mirror peer journal client: "
         << cpp_strerror(r) << dendl;
    save_result(r);
    shut_down_journaler();
    return;
  }

  librbd::journal::MirrorPeerClientMeta *mirror_peer_client_meta =
    boost::get<librbd::journal::MirrorPeerClientMeta>(
      &mirror_peer_client_data.client_meta);
  if (mirror_peer_client_meta == nullptr) {
    derr << ": unknown mirror peer journal client registration" << dendl;
    save_result(-EINVAL);
    shut_down_journaler();
    return;
  }

  if (m_mirror_peer_client_meta != nullptr) {
    *m_mirror_peer_client_meta = *mirror_peer_client_meta;
  }

  tag_list();
}

template <typename I>
void OpenJournalRequest<I>::tag_list() {
  uint64_t start_after_tag_tid = 0;
  auto &object_positions = m_client.commit_position.object_positions;
  if (!object_positions.empty() && object_positions.front().tag_tid > 0) {
    start_after_tag_tid = object_positions.front().tag_tid - 1;
  }

  dout(20) << ": start_after_tag_tid=" << start_after_tag_tid << dendl;

  Context *ctx = create_context_callback<
    OpenJournalRequest<I>, &OpenJournalRequest<I>::handle_tag_list>(this);
  m_journaler->get_tags(m_tag_class, start_after_tag_tid, &m_tags, ctx);
}

template <typename I>
void OpenJournalRequest<I>::handle_tag_list(int r) {
  dout(20) << ": r=" << r << dendl;

  if (r < 0) {
    derr << ": failed to decode tags: " << cpp_strerror(r) << dendl;
    save_result(r);
    shut_down_journaler();
    return;
  }

  m_tag_data_list->clear();
  for (auto tag : m_tags) {
    librbd::journal::TagData tag_data;
    try {
      bufferlist::iterator it = tag.data.begin();
      ::decode(tag_data, it);
    } catch (const buffer::error &err) {
      derr << ": failed to decode tag " << tag.tid << " data: " << err.what()
           << dendl;
      save_result(-EBADMSG);
      shut_down_journaler();
      return;
    }

    m_tag_data_list->push_back(std::make_pair(tag.tid, tag_data));
  }

  shut_down_journaler();
}

template <typename I>
void OpenJournalRequest<I>::shut_down_journaler() {
  if (m_ret_val == 0 && m_opened_journaler != nullptr) {
    *m_opened_journaler = m_journaler;
    finish();
    return;
  } else if (m_opened_journaler != nullptr) {
    *m_opened_journaler = nullptr;
  }

  dout(20) << dendl;
  Context *ctx = create_context_callback<
    OpenJournalRequest<I>,
    &OpenJournalRequest<I>::handle_shut_down_journaler>(this);
  m_journaler->shut_down(ctx);
}

template <typename I>
void OpenJournalRequest<I>::handle_shut_down_journaler(int r) {
  dout(20) << ": r=" << r << dendl;

  if (r < 0) {
    // just log the failure -- no need to bubble it up
    derr << ": failed to shut down journaler: " << cpp_strerror(r) << dendl;
  }

  delete m_journaler;
  finish();
}

template <typename I>
void OpenJournalRequest<I>::save_result(int r) {
  if (m_ret_val < 0 || r >= 0) {
    return;
  }

  dout(20) << ": r=" << r << dendl;
  m_ret_val = r;
}

template <typename I>
void OpenJournalRequest<I>::finish() {
  dout(20) << ": r=" << m_ret_val << dendl;
  m_on_finish->complete(m_ret_val);
  delete this;
}

template <typename I>
int OpenJournalRequest<I>::decode_client_data(
    const std::string &client_id,
    const std::string &client_type, bool ignore_missing,
    librbd::journal::ClientData *client_data) {

  int r = m_journaler->get_cached_client(client_id, &m_client);
  if (r == -ENOENT && ignore_missing) {
    return r;
  } else if (r < 0) {
    derr << ": failed to retrieve " << client_type << " journal client: "
         << cpp_strerror(r) << dendl;
    return r;
  }
  dout(20) << ": " << client_type << " journal client: " << m_client << dendl;

  bufferlist::iterator it = m_client.data.begin();
  try {
    ::decode(*client_data, it);
  } catch (const buffer::error &err) {
    derr << ": failed to decode " << client_type << " journal client meta "
         << "data: " << err.what() << dendl;
    return -EBADMSG;
  }
  return 0;
}

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

template class rbd::mirror::image_replayer::OpenJournalRequest<librbd::ImageCtx>;
