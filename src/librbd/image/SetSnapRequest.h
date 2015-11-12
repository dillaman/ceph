// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IMAGE_SNAP_SET_REQUEST_H
#define CEPH_LIBRBD_IMAGE_SNAP_SET_REQUEST_H

#include "include/int_types.h"
#include "librbd/parent_types.h"
#include <string>

class Context;

namespace librbd {

template <typename> class ExclusiveLock;
class ImageCtx;
class ObjectMap;

namespace image {

template <typename> class RefreshParentRequest;

template <typename ImageCtxT = ImageCtx>
class SetSnapRequest {
public:
  static SetSnapRequest *create(ImageCtxT &image_ctx,
                                const std::string &snap_name,
                                Context *on_finish) {
    return new SetSnapRequest(image_ctx, snap_name, on_finish);
  }

  ~SetSnapRequest();

  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    | (set snap)
   *    |-----------> BLOCK_WRITES ---> SHUTDOWN_EXCLUSIVE_LOCK
   *    |                 .                 |
   *    |                 .                 |
   *    |                 v                 |
   *    |         (skip shutdown if         |
   *    |          lock disabled)           |
   *    |                                   |
   *    |                                   |
   *    |       <BLOCK or SHUTDOWN>         |              <BLOCK or SHUTDOWN>
   *    |             .                     |                    .
   *    | (object map .                     v                    . (no parent)
   *    |  disabled / .               . . REFRESH_PARENT         .
   *    |  no parent) .               .     |                    .
   *    |             .  (object map  .     v                    .
   *    |             .     disabled) .   REFRESH_OBJECT_MAP < . .
   *    |             .               .     |
   *    |             .               .     v
   *    |             . . . . . . . . . > <finish> <  . . . . . .
   *    |                                   ^  ^                .
   *    | (unset snap /                     |  |                .
   *    |  no exclusive lock /              |  |                .
   *    |  no parent)                       |  |                .
   *    |-----------------------------------/  |                . (object map
   *    |                                      |                .  disabled)
   *    |                                 REFRESH_OBJECT_MAP    .
   *    |                                    ^ ^                .
   *    | (unset snap /                      | |                .
   *    |  no exclusive lock)                | |                .
   *    |------------------------------------/ |                .
   *    |                                      |                .
   *    | (unset snap /                        |                .
   *    |  exclusive lock)                     |                .
   *    \-----------------------------> INIT_EXCLUSIVE_LOCK . . .
   *
   * @endverbatim
   */

  SetSnapRequest(ImageCtxT &image_ctx, const std::string &snap_name,
                Context *on_finish);

  ImageCtxT &m_image_ctx;
  std::string m_snap_name;
  Context *m_on_finish;

  uint64_t m_snap_id;
  ExclusiveLock<ImageCtxT> *m_exclusive_lock;
  ObjectMap *m_object_map;
  RefreshParentRequest<ImageCtxT> *m_refresh_parent;

  bool m_writes_blocked;

  void send_init_exclusive_lock();
  Context *handle_init_exclusive_lock(int *result);

  void send_block_writes();
  Context *handle_block_writes(int *result);

  Context *send_shut_down_exclusive_lock(int *result);
  Context *handle_shut_down_exclusive_lock(int *result);

  Context *send_refresh_parent(int *result);
  Context *handle_refresh_parent(int *result);

  Context *send_refresh_object_map(int *result);
  Context *handle_refresh_object_map(int *result);

  Context *send_finalize_refresh_parent(int *result);
  Context *handle_finalize_refresh_parent(int *result);

  int apply();
};

} // namespace image
} // namespace librbd

extern template class librbd::image::SetSnapRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IMAGE_SNAP_SET_REQUEST_H
