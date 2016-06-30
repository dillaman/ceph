// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RBD_MIRROR_IMAGE_REPLAYER_START_REQUEST_H
#define RBD_MIRROR_IMAGE_REPLAYER_START_REQUEST_H

#include "librbd/journal/TypeTraits.h"
#include "tools/rbd_mirror/BaseRequest.h"

class Context;
namespace journal { class Journaler; }
namespace librbd { class ImageCtx; }

namespace rbd {
namespace mirror {

class ProgressContext;

namespace image_replayer {

template <typename ImageCtxT = librbd::ImageCtx>
class StartRequest : public BaseRequest {
public:
  typedef librbd::journal::TypeTraits<ImageCtxT> TypeTraits;
  typedef typename TypeTraits::Journaler Journaler;

  static StartRequest *create();

  StartRequest();

  void send();
  void cancel();

private:

  void get_local_image_id();
  void handle_get_local_image_id(int r);

  void bootstrap();
  void handle_bootstrap(int r);

};

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::image_replayer::StartRequest<librbd::ImageCtx>;

#endif // RBD_MIRROR_IMAGE_REPLAYER_START_REQUEST_H

