// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RBD_MIRROR_IMAGE_REPLAYER_OPEN_JOURNAL_REQUEST_H
#define RBD_MIRROR_IMAGE_REPLAYER_OPEN_JOURNAL_REQUEST_H

#include "include/buffer.h"
#include "include/int_types.h"
#include "include/rados/librados.hpp"
#include "cls/journal/cls_journal_types.h"
#include "librbd/journal/Types.h"
#include "librbd/journal/TypeTraits.h"
#include <list>
#include <string>
#include <utility>

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
class OpenJournalRequest {
public:
  typedef librbd::journal::TypeTraits<ImageCtxT> TypeTraits;
  typedef typename TypeTraits::Journaler Journaler;

  typedef std::pair<uint64_t, librbd::journal::TagData> TagTidData;
  typedef std::list<TagTidData> TagDataList;

  using MirrorPeerClientMeta = librbd::journal::MirrorPeerClientMeta;

  static OpenJournalRequest *create(librados::IoCtx &io_ctx,
                                    const std::string &journal_id,
                                    const std::string &client_id,
                                    ContextWQ *work_queue, SafeTimer *timer,
                                    Mutex *timer_lock,
                                    TagDataList *tag_data_list,
                                    MirrorPeerClientMeta *client_meta,
                                    Journaler **opened_journaler,
                                    Context *on_finish) {
    return new OpenJournalRequest(io_ctx, journal_id, client_id, work_queue,
                                  timer, timer_lock, tag_data_list, client_meta,
                                  opened_journaler, on_finish);
  }

  OpenJournalRequest(librados::IoCtx &io_ctx, const std::string &journal_id,
                     const std::string &client_id, ContextWQ *work_queue,
                     SafeTimer *timer, Mutex *timer_lock,
                     TagDataList *tag_data_list,
                     MirrorPeerClientMeta *client_meta,
                     Journaler **opened_journaler, Context *on_finish);

  void send();

private:
  /**
  * @verbatim
  *
  * <start>
  *    |
  *    v
  * INIT_JOURNALER
  *    |
  *    v
  * TAG_LIST
  *    |
  *    v (skip if needed by caller)
  * SHUT_DOWN_JOURNALER
  *    |
  *    v
  * <finish>
  *
  * @endverbatim
  */

  typedef std::list<cls::journal::Tag> Tags;

  librados::IoCtx &m_io_ctx;
  std::string m_journal_id;
  std::string m_client_id;
  ContextWQ *m_work_queue;
  SafeTimer *m_timer;
  Mutex *m_timer_lock;
  TagDataList *m_tag_data_list;
  MirrorPeerClientMeta *m_mirror_peer_client_meta;
  Journaler **m_opened_journaler;
  Context *m_on_finish;

  int m_ret_val = 0;
  Journaler *m_journaler;
  cls::journal::Client m_client;
  uint64_t m_tag_class = 0;
  Tags m_tags;

  void init_journaler();
  void handle_init_journaler(int r);

  void tag_list();
  void handle_tag_list(int r);

  void shut_down_journaler();
  void handle_shut_down_journaler(int r);

  void save_result(int r);
  void finish();

  int decode_client_data(const std::string &client_id,
                         const std::string &client_type,
                         bool ignore_missing,
                         librbd::journal::ClientData *client_data);

};

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::image_replayer::OpenJournalRequest<librbd::ImageCtx>;

#endif // RBD_MIRROR_IMAGE_REPLAYER_OPEN_JOURNAL_REQUEST_H
