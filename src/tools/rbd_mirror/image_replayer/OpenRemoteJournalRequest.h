// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RBD_MIRROR_IMAGE_REPLAYER_OPEN_REMOTE_JOURNAL_REQUEST_H
#define RBD_MIRROR_IMAGE_REPLAYER_OPEN_REMOTE_JOURNAL_REQUEST_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/rados/librados.hpp"
#include "librbd/journal/Types.h"
#include "librbd/journal/TypeTraits.h"
#include <list>
#include <string>

class Context;
class ContextWQ;
class Mutex;
class SafeTimer;
namespace journal { class Journaler; }
namespace librbd { class ImageCtx; }

namespace rbd {
namespace mirror {
namespace image_replayer {

template <typename ImageCtxT = librbd::ImageCtx>
class OpenRemoteJournalRequest {
public:
  typedef librbd::journal::TypeTraits<ImageCtxT> TypeTraits;
  typedef typename TypeTraits::Journaler Journaler;

  typedef std::pair<uint64_t, librbd::journal::TagData> TagTidData;
  typedef std::list<TagTidData> TagDataList;

  using MirrorPeerClientMeta = librbd::journal::MirrorPeerClientMeta;

  OpenRemoteJournalRequest(librados::IoCtx &remote_io_ctx,
                           const std::string &remote_image_id,
                           const std::string &global_image_id,
                           ContextWQ *work_queue, SafeTimer *timer,
                           Mutex *timer_lock,
                           const std::string &local_mirror_uuid,
                           Context *on_finish);

  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * OPEN_REMOTE_JOURNAL
   *    |
   *    v (skip if registered)
   * REGISTER_CLIENT * * * * * * *
   *    |                        *
   *    v (skip if up-to-date)   *
   * UPDATE_CLIENT * * * * * * * *
   *    |                        *
   *    |                        *
   *    |                 SHUT_DOWN_REMOTE_JOURNAL
   *    |                        |
   *    v                        |
   * <finish> <------------------/
   *
   * @endverbatim
   */

  librados::IoCtx &m_remote_io_ctx;
  std::string m_remote_image_id;
  std::string m_global_image_id;
  ContextWQ *m_work_queue;
  SafeTimer *m_timer;
  Mutex *m_timer_lock;
  std::string m_local_mirror_uuid;
  TagDataList *m_mirror_peer_tag_data_list;
  MirrorPeerClientMeta *m_mirror_peer_client_meta;
  Context *m_on_finish;

  Journaler *m_journaler = nullptr;
  int m_ret_val = 0;

  void open_remote_journal();
  void handle_open_remote_journal(int r);

  void shut_down_remote_journal();
  void handle_shut_down_remote_journal(int r);

  void save_result(int r);
  void finish();

};

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::image_replayer::OpenRemoteJournalRequest<librbd::ImageCtx>;

#endif // RBD_MIRROR_IMAGE_REPLAYER_OPEN_REMOTE_JOURNAL_REQUEST_H
