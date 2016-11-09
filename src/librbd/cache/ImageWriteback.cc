// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ImageWriteback.h"
#include "include/buffer.h"
#include "common/dout.h"
#include "librbd/ImageCtx.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/AioImageRequest.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::ImageWriteback: " << __func__ << ": "

namespace librbd {
namespace cache {

template <typename I>
ImageWriteback<I>::ImageWriteback(I &image_ctx) : m_image_ctx(image_ctx) {
}

template <typename I>
void ImageWriteback<I>::aio_read(Extents &&image_extents, bufferlist *bl,
                                 int fadvise_flags, Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "image_extents=" << image_extents << ", "
                 << "on_finish=" << on_finish << dendl;

  auto aio_comp = io::AioCompletion::create_and_start(on_finish, &m_image_ctx,
                                                      io::AIO_TYPE_READ);
  io::AioImageRead<I> req(m_image_ctx, aio_comp, std::move(image_extents),
                          nullptr, bl, fadvise_flags);
  req.set_bypass_image_cache();
  req.send();
}

template <typename I>
void ImageWriteback<I>::aio_write(Extents &&image_extents,
                                  ceph::bufferlist&& bl,
                                  int fadvise_flags, Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "image_extents=" << image_extents << ", "
                 << "on_finish=" << on_finish << dendl;

  auto aio_comp = io::AioCompletion::create_and_start(on_finish, &m_image_ctx,
                                                      io::AIO_TYPE_WRITE);
  io::AioImageWrite<I> req(m_image_ctx, aio_comp, std::move(image_extents),
                       std::move(bl), fadvise_flags);
  req.set_bypass_image_cache();
  req.send();
}

template <typename I>
void ImageWriteback<I>::aio_discard(uint64_t offset, uint64_t length,
                                    Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "offset=" << offset << ", "
                 << "length=" << length << ", "
                << "on_finish=" << on_finish << dendl;

  auto aio_comp = io::AioCompletion::create_and_start(on_finish, &m_image_ctx,
                                                      io::AIO_TYPE_DISCARD);
  io::AioImageDiscard<I> req(m_image_ctx, aio_comp, offset, length);
  req.set_bypass_image_cache();
  req.send();
}

template <typename I>
void ImageWriteback<I>::aio_flush(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << "on_finish=" << on_finish << dendl;

  auto aio_comp = io::AioCompletion::create_and_start(on_finish, &m_image_ctx,
                                                      io::AIO_TYPE_FLUSH);
  io::AioImageFlush<I> req(m_image_ctx, aio_comp);
  req.set_bypass_image_cache();
  req.send();
}

} // namespace cache
} // namespace librbd

template class librbd::cache::ImageWriteback<librbd::ImageCtx>;

