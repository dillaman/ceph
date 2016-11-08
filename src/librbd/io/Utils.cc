// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/Utils.h"
#include "include/fs_types.h"
#include "common/dout.h"
#include "librbd/io/Types.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::util::"

namespace librbd {
namespace io {
namespace util {

Striper::Striper(CephContext *cct, const file_layout_t &file_layout,
                 const ImageExtent &image_extent)
  : m_cct(cct), m_object_size(file_layout.object_size),
    m_stripe_unit(file_layout.stripe_unit),
    m_stripe_count(file_layout.stripe_count),
    m_current_offset(image_extent.get_image_offset()),
    m_left(image_extent.get_image_length()) {
  assert(m_object_size >= m_stripe_unit);
  if (m_stripe_count == 1) {
    m_stripe_unit = m_object_size;
  }

  m_stripes_per_object = m_object_size / m_stripe_unit;
  ldout(cct, 20) << "Striper::" << __func__ << ": "
                 << "os=" << m_object_size << ", "
                 << "su=" << m_stripe_unit << ", "
                 << "sc=" << m_stripe_count << ", "
                 << "spo=" << m_stripes_per_object << dendl;
}

bool Striper::next_object_extent(ObjectExtent *object_extent) {
  if (m_left == 0) {
    return false;
  }

  // map extent to object
  uint64_t block_number = m_current_offset / m_stripe_unit;
  uint64_t stripe_number = block_number / m_stripe_count;
  uint64_t stripe_position = block_number % m_stripe_count;
  uint64_t object_set_number = stripe_number / m_stripes_per_object;
  uint64_t object_number = (object_set_number * m_stripe_count) +
                           stripe_position;

  // map extent into object (stripe)
  uint64_t block_start = (stripe_number % m_stripes_per_object) * m_stripe_unit;
  uint64_t block_offset = m_current_offset % m_stripe_unit;
  uint64_t max_length = m_stripe_unit - block_offset;

  uint64_t x_offset = block_start + block_offset;
  uint64_t x_length;
  if (m_left > max_length) {
    x_length = max_length;
  } else {
    x_length = m_left;
  }

  ldout(m_cct, 20) << "ImageIO::" << __func__ << ": "
                   << "offset=" << m_current_offset << ", "
                   << "block_number=" << block_number << ", "
                   << "stripe_number=" << stripe_number << ", "
                   << "stripe_position=" << stripe_position << ", "
                   << "object_set_number=" << object_set_number << ", "
                   << "object_number=" << object_number << ", "
                   << "block_start=" << block_start << ", "
                   << "block_offset=" << block_offset << " "
                   << x_offset << "~" << x_length << dendl;

  // advance to the next object (stripe) extent
  m_current_offset += x_length;
  m_left -= x_length;
  *object_extent = ObjectExtent(object_number, x_offset, x_length);
  return true;
}

bool Destriper::next_image_extent(ImageExtent *image_extent) {
  return false;
}

} // namespace util
} // namespace io
} // namespace librbd
