// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_UTILS_H
#define CEPH_LIBRBD_IO_UTILS_H

#include "include/int_types.h"

struct CephContext;
struct file_layout_t;

namespace librbd {
namespace io {

struct ImageExtent;
struct ObjectExtent;

namespace util {

class Striper {
public:
  Striper(CephContext *cct, const file_layout_t &file_layout,
          const ImageExtent &image_extent);

  bool next_object_extent(ObjectExtent *object_extent);

private:
  CephContext *m_cct;
  uint32_t m_object_size;
  uint32_t m_stripe_unit;
  uint32_t m_stripe_count;
  uint32_t m_stripes_per_object;

  uint64_t m_current_offset;
  uint64_t m_left;

};

class Destriper {
public:
  Destriper(CephContext *cct, const file_layout_t &file_layout,
            const ObjectExtent &object_extent)
    : m_cct(cct), m_file_layout(file_layout),
      m_object_extent(object_extent) {
  }

  bool next_image_extent(ImageExtent *image_extent);

private:
  CephContext *m_cct;
  const file_layout_t &m_file_layout;
  const ObjectExtent &m_object_extent;
};

} // namespace util
} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_UTILS_H
