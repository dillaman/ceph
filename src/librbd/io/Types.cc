// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/Types.h"
#include "include/assert.h"
#include "include/fs_types.h"
#include "common/dout.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/Utils.h"
#include <boost/variant/apply_visitor.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::"

namespace librbd {
namespace io {

namespace {

struct ImageIOMapObjectIOVisitor : public boost::static_visitor<void> {
  template <typename T>
  void operator()(T &t) const {
    t.map_object_io();
  }
};

} // anonymous namespace

ReadBufferExtent *ReadBufferExtent::split_left(uint64_t lhs_length,
                                               Pool *pool) {
  assert(lhs_length <= m_buffer_length);

  uint64_t lhs_offset = m_buffer_offset;
  m_buffer_offset += lhs_length;
  m_buffer_length -= lhs_length;
  return pool->allocate(lhs_offset, lhs_length);
}

WriteBufferExtent *WriteBufferExtent::split_left(uint64_t lhs_length,
                                                 Pool *pool) {
  assert(lhs_length <= m_buffer_length);

  bufferlist::const_iterator lhs_bl_iter(m_bl_iter);
  m_bl_iter.advance(lhs_length);
  m_buffer_length -= lhs_length;
  return pool->allocate(std::move(lhs_bl_iter), lhs_length);
}

void ExtentImageIOBase::map_object_io() {
  // pre-allocate a hash bucket sized to number of objects we might
  // hit given our request length
  uint64_t total_length = 0;
  for (auto &image_extent : m_image_extents) {
    total_length += image_extent.get_image_length();
  }

  const file_layout_t &file_layout = m_aio_completion->ictx->layout;
  uint32_t object_size = file_layout.object_size;
  set_estimated_object_count(total_length / object_size);

  for (auto &image_extent : m_image_extents) {
    if (image_extent.get_image_length() == 0) {
      continue;
    }

    // map the image extents to object extents
    util::Striper striper(m_aio_completion->ictx->cct, file_layout,
                          image_extent);
    ObjectExtent object_extent;
    while (striper.next_object_extent(&object_extent)) {
      append_extent(std::move(object_extent));
    }
  }
}

void FlushImageIO::map_object_io() {
  // do nothing -- flush doesn't have extents
}

void InvalidImageIO::map_object_io() {
  assert(false);
}

void ImageIO::map_object_io() {
  boost::apply_visitor(ImageIOMapObjectIOVisitor(), *this);
}

} // namespace io
} // namespace librbd

