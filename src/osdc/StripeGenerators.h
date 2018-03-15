// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/types.h"
#include "osd/osd_types.h"
#include <utility>
#include <vector>

namespace StripeGenerator {

template <typename T>
void file_to_extents(
    CephContext *cct, const file_layout_t *layout, uint64_t offset,
    uint64_t len, uint64_t trunc_size, uint64_t buffer_offset,
    T& lambda) {
  // we want only one extent per object!  this means that each extent
  // we read may map into different bits of the final read
  // buffer.. hence ObjectExtent.buffer_extents

  __u32 object_size = layout->object_size;
  __u32 stripe_unit = layout->stripe_unit;
  __u32 stripe_count = layout->stripe_count;

  if (stripe_count == 1) {
    stripe_unit = object_size;
  }

  uint64_t stripes_per_object = object_size / stripe_unit;

  std::vector<std::pair<uint64_t, uint64_t>> buffer_extents;

  uint64_t cur = offset;
  uint64_t left = len;
  while (left > 0) {
    // layout into objects
    uint64_t blockno = cur / stripe_unit; // which block
    // which horizontal stripe (Y)
    uint64_t stripeno = blockno / stripe_count;
    // which object in the object set (X)
    uint64_t stripepos = blockno % stripe_count;
    // which object set
    uint64_t objectsetno = stripeno / stripes_per_object;
    // object id
    uint64_t objectno = objectsetno * stripe_count + stripepos;

    // map range into object
    uint64_t block_start = (stripeno % stripes_per_object) * stripe_unit;
    uint64_t block_off = cur % stripe_unit;
    uint64_t max = stripe_unit - block_off;

    uint64_t x_offset = block_start + block_off;
    uint64_t x_len;
    if (left > max)
      x_len = max;
    else
      x_len = left;

    buffer_extents.push_back({cur - offset + buffer_offset,
                              x_len});

    lambda(objectno, x_offset, x_len, std::move(buffer_extents));
    buffer_extents.clear();


    left -= x_len;
    cur += x_len;
  }
}

} // namespace StripeGenerator
