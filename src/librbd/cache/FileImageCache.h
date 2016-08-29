// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_CACHE_FILE_IMAGE_CACHE
#define CEPH_LIBRBD_CACHE_FILE_IMAGE_CACHE

#include "ImageCache.h"
#include "ImageWriteback.h"

namespace librbd {

struct ImageCtx;

namespace cache {

namespace file {

class Policy;
template <typename> class ImageStore;
template <typename> class JournalStore;
template <typename> class MetaStore;

} // namespace file

/**
 * Prototype file-based, client-side, image extent cache
 */
template <typename ImageCtxT = librbd::ImageCtx>
class FileImageCache : public ImageCache {
public:
  FileImageCache(ImageCtx &image_ctx);
  ~FileImageCache();

  /// client AIO methods
  virtual void aio_read(Extents&& image_extents, ceph::bufferlist *bl,
                        int fadvise_flags, Context *on_finish);
  virtual void aio_write(Extents&& image_extents, ceph::bufferlist&& bl,
                         int fadvise_flags, Context *on_finish);
  virtual void aio_discard(uint64_t offset, uint64_t length,
                           Context *on_finish);
  virtual void aio_flush(Context *on_finish);

  /// internal state methods
  virtual void init(Context *on_finish);
  virtual void shut_down(Context *on_finish);

  virtual void invalidate(Context *on_finish);
  virtual void flush(Context *on_finish);

private:
  ImageCtxT &m_image_ctx;
  ImageWriteback<ImageCtxT> m_image_writeback;

  file::Policy *m_policy = nullptr;
  file::MetaStore<ImageCtx> *m_meta_store = nullptr;
  file::JournalStore<ImageCtx> *m_journal_store = nullptr;
  file::ImageStore<ImageCtx> *m_image_store = nullptr;

  void invalidate(Extents&& image_extents, Context *on_finish);

};

} // namespace cache
} // namespace librbd

extern template class librbd::cache::FileImageCache<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_CACHE_FILE_IMAGE_CACHE
