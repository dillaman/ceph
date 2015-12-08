// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/image/CloseRequest.h"
#include "common/dout.h"
#include "common/errno.h"
#include "librbd/AioImageRequestWQ.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageWatcher.h"
#include "librbd/ImageCtx.h"
#include "librbd/Utils.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::image::CloseRequest: "

namespace librbd {
namespace image {

using util::create_context_callback;

template <typename I>
CloseRequest<I>::CloseRequest(I *image_ctx, Context *on_finish)
  : m_image_ctx(image_ctx), m_on_finish(on_finish), m_error_result(0) {
  assert(image_ctx != nullptr);
}

template <typename I>
void CloseRequest<I>::send() {
  m_image_ctx->readahead.set_max_readahead_size(0);

  send_block_writes();
}

template <typename I>
void CloseRequest<I>::send_block_writes() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  // blocking writes will flush IO
  m_image_ctx->aio_work_queue->block_writes(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_block_writes>(this));
}

template <typename I>
void CloseRequest<I>::handle_block_writes(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to block write operations: " << r << dendl;
    m_error_result = r;
  }
  send_shut_down_exclusive_lock();
}

template <typename I>
void CloseRequest<I>::send_shut_down_exclusive_lock() {
  if (m_image_ctx->exclusive_lock == nullptr) {
    send_flush_readahead();
    return;
  }

  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->exclusive_lock->shut_down(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_shut_down_exclusive_lock>(this));
}

template <typename I>
void CloseRequest<I>::handle_shut_down_exclusive_lock(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to shut down exclusive lock: " << cpp_strerror(r)
               << dendl;
    m_error_result = r;
  }
  send_flush_readahead();
}

template <typename I>
void CloseRequest<I>::send_flush_readahead() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->readahead.wait_for_pending(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_flush_readahead>(this));
}

template <typename I>
void CloseRequest<I>::handle_flush_readahead(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  send_flush();
}

template <typename I>
void CloseRequest<I>::send_flush() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->flush(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_flush>(this));
}

template <typename I>
void CloseRequest<I>::handle_flush(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to flush IO: " << cpp_strerror(r) << dendl;
    m_error_result = r;
  }
  send_shut_down_cache();
}

template <typename I>
void CloseRequest<I>::send_shut_down_cache() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->shut_down_cache(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_shut_down_cache>(this));
}

template <typename I>
void CloseRequest<I>::handle_shut_down_cache(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << "failed to shut down cache: " << cpp_strerror(r) << dendl;
    m_error_result = r;
  }
  send_flush_copyup();
}

template <typename I>
void CloseRequest<I>::send_flush_copyup() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->flush_copyup(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_flush_copyup>(this));
}

template <typename I>
void CloseRequest<I>::handle_flush_copyup(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;
  send_flush_op_work_queue();
}

template <typename I>
void CloseRequest<I>::send_flush_op_work_queue() {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  m_image_ctx->op_work_queue->queue(create_context_callback<
    CloseRequest<I>, &CloseRequest<I>::handle_flush_op_work_queue>(this), 0);
}

template <typename I>
void CloseRequest<I>::handle_flush_op_work_queue(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;
  send_close_parent();
}

template <typename I>
void CloseRequest<I>::send_close_parent() {
  if (m_image_ctx->parent == nullptr) {
    finish();
    return;
  }

  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << dendl;

  // TODO
}

template <typename I>
void CloseRequest<I>::handle_close_parent(int r) {
  CephContext *cct = m_image_ctx->cct;
  ldout(cct, 10) << this << " " << __func__ << ": r=" << r << dendl;

  finish();
}

template <typename I>
void CloseRequest<I>::finish() {
  if (m_image_ctx->image_watcher) {
    m_image_ctx->image_watcher->unregister_watch();
  }
  delete m_image_ctx;

  m_on_finish->complete(m_error_result);
  delete this;
}

} // namespace image
} // namespace librbd

template class librbd::image::CloseRequest<librbd::ImageCtx>;
