// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "SetHeadRequest.h"
#include "common/errno.h"
#include "cls/rbd/cls_rbd_client.h"
#include "cls/rbd/cls_rbd_types.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/Utils.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::deep_copy::SetHeadRequest: " \
                           << this << " " << __func__ << ": "

namespace librbd {
namespace deep_copy {

using librbd::util::create_context_callback;
using librbd::util::create_rados_callback;

template <typename I>
SetHeadRequest<I>::SetHeadRequest(I *image_ctx, uint64_t size,
                                  const ParentImageInfo &info,
                                  Context *on_finish)
  : m_image_ctx(image_ctx), m_size(size), m_parent_image_info(info),
    m_on_finish(on_finish), m_cct(image_ctx->cct) {
  assert(m_parent_image_info.overlap <= m_size);
}

template <typename I>
void SetHeadRequest<I>::send() {
  send_set_size();
}

template <typename I>
void SetHeadRequest<I>::send_set_size() {
  m_image_ctx->snap_lock.get_read();
  if (m_image_ctx->size == m_size) {
    m_image_ctx->snap_lock.put_read();
    send_remove_parent();
    return;
  }
  m_image_ctx->snap_lock.put_read();

  ldout(m_cct, 20) << dendl;

  // Change the image size on disk so that the snapshot picks up
  // the expected size.  We can do this because the last snapshot
  // we process is the sync snapshot which was created to match the
  // image size. We also don't need to worry about trimming because
  // we track the highest possible object number within the sync record
  librados::ObjectWriteOperation op;
  librbd::cls_client::set_size(&op, m_size);

  auto finish_op_ctx = start_lock_op();
  if (finish_op_ctx == nullptr) {
    lderr(m_cct) << "lost exclusive lock" << dendl;
    finish(-EROFS);
    return;
  }

  auto ctx = new FunctionContext([this, finish_op_ctx](int r) {
      handle_set_size(r);
      finish_op_ctx->complete(0);
    });
  librados::AioCompletion *comp = create_rados_callback(ctx);
  int r = m_image_ctx->md_ctx.aio_operate(m_image_ctx->header_oid, comp, &op);
  assert(r == 0);
  comp->release();
}

template <typename I>
void SetHeadRequest<I>::handle_set_size(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    lderr(m_cct) << "failed to update image size: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  {
    // adjust in-memory image size now that it's updated on disk
    RWLock::WLocker snap_locker(m_image_ctx->snap_lock);
    if (m_image_ctx->size > m_size) {
      RWLock::WLocker parent_locker(m_image_ctx->parent_lock);
      if (m_image_ctx->head_parent_overlap > m_size) {
        assert(m_image_ctx->parent_image_spec.exists());
        m_image_ctx->head_parent_overlap = m_size;
      }
      m_image_ctx->size = m_size;
    }
  }

  send_remove_parent();
}

template <typename I>
void SetHeadRequest<I>::send_remove_parent() {
  ParentImageInfo parent_image_info;
  {
    RWLock::RLocker snap_locker(m_image_ctx->snap_lock);
    RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
    int r = m_image_ctx->get_parent_image_info(CEPH_NOSNAP, &parent_image_info);
    assert(r == 0);
  }

  if (!parent_image_info.exists() || parent_image_info == m_parent_image_info) {
    send_set_parent();
    return;
  }

  ldout(m_cct, 20) << dendl;

  librados::ObjectWriteOperation op;
  librbd::cls_client::remove_parent(&op);

  auto finish_op_ctx = start_lock_op();
  if (finish_op_ctx == nullptr) {
    lderr(m_cct) << "lost exclusive lock" << dendl;
    finish(-EROFS);
    return;
  }

  auto ctx = new FunctionContext([this, finish_op_ctx](int r) {
      handle_remove_parent(r);
      finish_op_ctx->complete(0);
    });
  librados::AioCompletion *comp = create_rados_callback(ctx);
  int r = m_image_ctx->md_ctx.aio_operate(m_image_ctx->header_oid, comp, &op);
  assert(r == 0);
  comp->release();
}

template <typename I>
void SetHeadRequest<I>::handle_remove_parent(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    lderr(m_cct) << "failed to remove parent: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  {
    // adjust in-memory parent now that it's updated on disk
    RWLock::RLocker snap_locker(m_image_ctx->snap_lock);
    RWLock::WLocker parent_locker(m_image_ctx->parent_lock);
    m_image_ctx->head_parent_overlap = 0;

    cls::rbd::ParentImageSpec parent_image_spec;
    m_image_ctx->get_parent_image_spec(&parent_image_spec);
    if (!parent_image_spec.exists()) {
      // no snapshot depends on the parent image spec so we can clear it
      m_image_ctx->parent_image_spec = {};
    }
  }

  send_set_parent();
}

template <typename I>
void SetHeadRequest<I>::send_set_parent() {
  ParentImageInfo parent_image_info;
  {
    RWLock::RLocker parent_locker(m_image_ctx->parent_lock);
    parent_image_info.spec = m_image_ctx->parent_image_spec;
    parent_image_info.overlap = m_image_ctx->head_parent_overlap;
  }

  if (parent_image_info == m_parent_image_info) {
    finish(0);
    return;
  } else if (parent_image_info.spec.exists() &&
             parent_image_info.spec != m_parent_image_info.spec) {
    lderr(m_cct) << "attempting to change parent image spec" << dendl;
    finish(-EINVAL);
    return;
  }
  assert(m_parent_image_info.exists());

  ldout(m_cct, 20) << dendl;

  librados::ObjectWriteOperation op;
  librbd::cls_client::set_parent(&op, m_parent_image_info.spec,
                                 m_parent_image_info.overlap);

  auto finish_op_ctx = start_lock_op();
  if (finish_op_ctx == nullptr) {
    lderr(m_cct) << "lost exclusive lock" << dendl;
    finish(-EROFS);
    return;
  }

  auto ctx = new FunctionContext([this, finish_op_ctx](int r) {
      handle_set_parent(r);
      finish_op_ctx->complete(0);
    });
  librados::AioCompletion *comp = create_rados_callback(ctx);
  int r = m_image_ctx->md_ctx.aio_operate(m_image_ctx->header_oid, comp, &op);
  assert(r == 0);
  comp->release();
}

template <typename I>
void SetHeadRequest<I>::handle_set_parent(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    lderr(m_cct) << "failed to set parent: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  {
    // adjust in-memory parent now that it's updated on disk
    RWLock::WLocker parent_locker(m_image_ctx->parent_lock);
    m_image_ctx->head_parent_overlap = m_parent_image_info.overlap;
  }

  finish(0);
}

template <typename I>
Context *SetHeadRequest<I>::start_lock_op() {
  RWLock::RLocker owner_locker(m_image_ctx->owner_lock);
  if (m_image_ctx->exclusive_lock == nullptr) {
    return new FunctionContext([](int r) {});
  }
  return m_image_ctx->exclusive_lock->start_op();
}

template <typename I>
void SetHeadRequest<I>::finish(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  m_on_finish->complete(r);
  delete this;
}

} // namespace deep_copy
} // namespace librbd

template class librbd::deep_copy::SetHeadRequest<librbd::ImageCtx>;
