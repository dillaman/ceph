// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_MIRROR_IMAGE_REPLAYER_JOURNAL_STATE_BUILDER_H
#define CEPH_RBD_MIRROR_IMAGE_REPLAYER_JOURNAL_STATE_BUILDER_H

#include "tools/rbd_mirror/image_replayer/StateBuilder.h"
#include "cls/journal/cls_journal_types.h"
#include "librbd/journal/Types.h"
#include "librbd/journal/TypeTraits.h"
#include <string>

struct Context;

namespace librbd { struct ImageCtx; }

namespace rbd {
namespace mirror {
namespace image_replayer {
namespace journal {

template <typename ImageCtxT>
class StateBuilder : public image_replayer::StateBuilder<ImageCtxT> {
public:
  typedef librbd::journal::TypeTraits<ImageCtxT> TypeTraits;
  typedef typename TypeTraits::Journaler Journaler;

  static StateBuilder* create(const std::string& global_image_id) {
    return new StateBuilder(global_image_id);
  }

  StateBuilder(const std::string& global_image_id);
  ~StateBuilder() override;

  void close(Context* on_finish) override;

  bool is_disconnected() const override;

  bool is_local_primary() const override;
  bool is_linked() const override;

  cls::rbd::MirrorImageMode get_mirror_image_mode() const override;

  std::string local_tag_owner;

  Journaler* remote_journaler = nullptr;
  cls::journal::ClientState remote_client_state =
    cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta remote_client_meta;

private:

  void shut_down_remote_journaler(Context* on_finish);
  void handle_shut_down_remote_journaler(int r, Context* on_finish);
};

} // namespace journal
} // namespace image_replayer
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::image_replayer::journal::StateBuilder<librbd::ImageCtx>;

#endif // CEPH_RBD_MIRROR_IMAGE_REPLAYER_JOURNAL_STATE_BUILDER_H
