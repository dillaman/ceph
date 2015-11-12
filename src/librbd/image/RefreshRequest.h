// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IMAGE_REFRESH_REQUEST_H
#define CEPH_LIBRBD_IMAGE_REFRESH_REQUEST_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/rbd_types.h"
#include "common/snap_types.h"
#include "cls/lock/cls_lock_types.h"
#include "librbd/parent_types.h"
#include <string>
#include <vector>

class Context;

namespace librbd {

class ImageCtx;
class Journal;

namespace image {

template<typename> class RefreshParentRequest;

template<typename ImageCtxT = ImageCtx>
class RefreshRequest {
public:
  static  RefreshRequest *create(ImageCtxT &image_ctx, Context *on_finish) {
    return new RefreshRequest(image_ctx, on_finish);
  }

  void send();

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    | (v1)
   *    |-----> V1_READ_HEADER ---> V1_GET_SNAPSHOTS ---> V1_GET_LOCKS
   *    |                                                     |
   *    | (v2)                                                v
   *    \-----> V2_GET_MUTABLE_METADATA                    <apply>
   *                |                                       |   .
   *                v                                       |   .
   *            V2_GET_FLAGS                                |   .
   *                |                                       |   .
   *                v                                       |   .
   *            V2_GET_SNAPSHOTS . . .                      |   .
   *                |                .                      |   .
   *                v                .                      |   .
   *            V2_REFRESH_PARENT    . (no parent /         |   .
   *                |                .  not needed)         |   .
   *                v                .                      |   .
   *     . . . . <apply> < . . . . . .                      |   .
   *     .        . . |                                     |   .
   *     .        . . |                                     |   .
   *     .        . . \---> V2_SHUT_DOWN_EXCLUSIVE_LOCK     |   .
   *     .        . .                          |            |   .
   *     .        . .                          |            |   .
   *     .        . . . . > V2_CLOSE_JOURNAL   |            |   .
   *     .        .             |              |            |   .
   *     .        v             v              |            |   .
   *     .      V2_FINALIZE_REFRESH_PARENT <---/            |   .
   *     .          .        |                              |   .
   *     .          .        \-------> FLUSH < -------------/   .
   *     .          .                    |                      .
   *     .          . (no new snap)      v        (no new snap) .
   *     .          . . . . . . . . > <finish> <  . . . . . . . .
   *     .                               ^
   *     .  (no parent / not needed)     .
   *     . . . . . . . . . . . . . . . . .
   *
   * @endverbatim
   */

  ImageCtxT &m_image_ctx;
  Context *m_on_finish;

  bool m_flush_aio;
  RefreshParentRequest<ImageCtxT> *m_refresh_parent;

  bufferlist m_out_bl;

  uint8_t m_order;
  uint64_t m_size;
  uint64_t m_features;
  uint64_t m_incompatible_features;
  uint64_t m_flags;
  std::string m_object_prefix;
  parent_info m_parent_md;

  ::SnapContext m_snapc;
  std::vector<std::string> m_snap_names;
  std::vector<uint64_t> m_snap_sizes;
  std::vector<parent_info> m_snap_parents;
  std::vector<uint8_t> m_snap_protection;
  std::vector<uint64_t> m_snap_flags;

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t> m_lockers;
  std::string m_lock_tag;
  bool m_exclusive_locked;

  RefreshRequest(ImageCtxT &image_ctx, Context *on_finish);

  void send_v1_read_header();
  Context *handle_v1_read_header(int *result);

  void send_v1_get_snapshots();
  Context *handle_v1_get_snapshots(int *result);

  void send_v1_get_locks();
  Context *handle_v1_get_locks(int *result);

  void send_v2_get_mutable_metadata();
  Context *handle_v2_get_mutable_metadata(int *result);

  void send_v2_get_flags();
  Context *handle_v2_get_flags(int *result);

  void send_v2_get_snapshots();
  Context *handle_v2_get_snapshots(int *result);

  Context *send_v2_refresh_parent();
  Context *handle_v2_refresh_parent(int *result);

  Context *send_v2_finalize_refresh_parent();
  Context *handle_v2_finalize_refresh_parent(int *result);

  Context *send_v2_shut_down_exclusive_lock();
  Context *handle_v2_shut_down_exclusive_lock(int *result);

  Context *send_v2_close_journal();
  Context *handle_v2_close_journal(int *result);

  Context *send_flush_aio();
  Context *handle_flush_aio(int *result);

  void apply();
  int get_parent_info(uint64_t snap_id, parent_info *parent_md);
};

} // namespace image
} // namespace librbd

extern template class librbd::image::RefreshRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IMAGE_REFRESH_REQUEST_H
