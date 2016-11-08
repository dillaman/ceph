// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_TYPES_H
#define CEPH_LIBRBD_IO_TYPES_H

#include "include/int_types.h"
#include "include/buffer.h"
#include <sys/uio.h>
#include <deque>
#include <map>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/unordered_set.hpp>
#include <boost/noncopyable.hpp>
#include <boost/variant.hpp>
#include "include/assert.h"

namespace librbd {
namespace io {

struct AioCompletion;

typedef std::vector<std::pair<uint64_t, uint64_t> > Extents;
typedef std::map<uint64_t, uint64_t> ExtentMap;

/// Memory stable allocator with support for a small number of inlined objects
template <typename Type, size_t InlineCount>
class InlinePool : private boost::noncopyable {
public:
  inline ~InlinePool() {
    // invoke the destructor for all allocated, inlined types
    for (size_t idx = 0; idx < m_inline_count; ++idx) {
      Type *type = get_inline_type(m_inline_count++);
      type->~Type();
    }
  }

  template <typename... Arguments>
  inline Type *allocate(Arguments&&... args) {
    // allocate from the inlined space if possible, otherwise fall back
    // to the stable deque heap space
    if (m_inline_count < InlineCount) {
      Type *type = get_inline_type(m_inline_count++);
      new (type) Type(std::forward<Arguments>(args)...);
      return type;
    }

    m_heap_types.emplace_back(std::forward<Arguments>(args)...);
    return &m_heap_types.back();
  }

private:
  typedef char TypeBuffer[sizeof(Type)];
  typedef std::deque<Type> HeapTypes;

  inline Type *get_inline_type(size_t index) {
    return reinterpret_cast<Type*>(&m_inline_types[index]);
  }

  TypeBuffer m_inline_types[InlineCount];
  size_t m_inline_count = 0;

  HeapTypes m_heap_types;
};

/// Represents a continuous extent within an object
class ObjectExtent {
public:
  ObjectExtent() {
  }
  ObjectExtent(uint64_t object_number, uint32_t object_offset,
               uint32_t object_length)
    : m_object_number(object_number), m_object_offset(object_offset),
      m_object_length(object_length) {
  }

  inline uint64_t get_object_number() const {
    return m_object_number;
  }
  inline uint32_t get_object_offset() const {
    return m_object_offset;
  }
  inline uint32_t get_object_length() const {
    return m_object_length;
  }

  inline void set_object_length(uint32_t object_length) {
    m_object_length = object_length;
  }

private:
  uint64_t m_object_number = 0;
  uint32_t m_object_offset = 0;
  uint32_t m_object_length = 0;
};

/// Represents a continous extent within an image
class ImageExtent {
public:
  ImageExtent() {
  }
  ImageExtent(uint64_t image_offset, uint64_t image_length)
    : m_image_offset(image_offset), m_image_length(image_length) {
  }

  inline uint64_t get_image_offset() const {
    return m_image_offset;
  }
  inline uint64_t get_image_length() const {
    return m_image_length;
  }

private:
  uint64_t m_image_offset = 0;
  uint64_t m_image_length = 0;
};

/// Vector of ImageExtents with support for a small number of inlined objects
typedef boost::container::small_vector<ImageExtent, 2> ImageExtents;

/// Destination buffer adapters for read requests
class LinearReadDestination {
public:
  LinearReadDestination(char *buffer, uint64_t buffer_length)
    : m_buffer(buffer), m_buffer_length(buffer_length) {
  }

private:
  char *m_buffer;
  uint64_t m_buffer_length;
};

class VectorReadDestination {
public:
  VectorReadDestination(const struct iovec *iov, int iov_count)
    : m_iov(iov), m_iov_count(iov_count) {
  }

private:
  const struct iovec *m_iov;
  int m_iov_count;
};

class BufferlistReadDestination {
public:
  BufferlistReadDestination(ceph::bufferlist *bl) : m_bl(bl) {
  }

private:
  ceph::bufferlist *m_bl;
};

class ReadDestination : public boost::variant<LinearReadDestination,
                                              VectorReadDestination,
                                              BufferlistReadDestination> {
public:
  template <typename T>
  ReadDestination(T &&t)
    : boost::variant<LinearReadDestination,
                     VectorReadDestination,
                     BufferlistReadDestination>(std::forward(t)) {
  }
};

/// Wrappers for describing the buffer offset for object IO requests
template <typename T, uint32_t MaxInline>
class BufferExtentBase : public boost::intrusive::slist_base_hook<> {
public:
  typedef InlinePool<T, MaxInline> Pool;
  typedef boost::intrusive::slist<
    T,
    boost::intrusive::cache_last<true>,
    boost::intrusive::constant_time_size<false>,
    boost::intrusive::linear<true> > List;
};

class ReadBufferExtent : public BufferExtentBase<ReadBufferExtent, 4> {
public:
  ReadBufferExtent(uint64_t buffer_offset, uint64_t buffer_length)
    : m_buffer_offset(buffer_offset), m_buffer_length(buffer_length) {
  }

  ReadBufferExtent *split_left(uint64_t length, Pool *pool);

private:
  uint64_t m_buffer_offset;
  uint64_t m_buffer_length;
};

class WriteBufferExtent : public BufferExtentBase<WriteBufferExtent, 2> {
public:
  WriteBufferExtent(const bufferlist &bl)
    : m_bl_iter(bl.begin()), m_buffer_length(bl.length()) {
  }
  WriteBufferExtent(bufferlist::const_iterator &&iter, uint64_t buffer_length)
    : m_bl_iter(std::move(iter)), m_buffer_length(buffer_length) {
  }

  WriteBufferExtent *split_left(uint64_t length, Pool *pool);

private:
  bufferlist::const_iterator m_bl_iter;
  uint64_t m_buffer_length;
};

/// Container for all IO operations associated with a single backing object
template <typename ObjectIOT,
          uint32_t MaxInline = 2,
          uint32_t MaxBucketInline = 4>
class ObjectIOBase : public boost::intrusive::unordered_set_base_hook<>,
                     public boost::intrusive::slist_base_hook<>,
                     private boost::noncopyable {
public:
  typedef InlinePool<ObjectIOT, MaxInline> Pool;

  typedef boost::intrusive::slist<
    ObjectIOT,
    boost::intrusive::cache_last<false>,
    boost::intrusive::constant_time_size<false>,
    boost::intrusive::linear<true> > List;

  struct ObjectNumberKey {
    typedef uint64_t type;

    type operator()(const ObjectIOT &object_io) const {
      return object_io.m_object_extent.get_object_number();
    }
  };

  typedef boost::intrusive::base_hook<
    boost::intrusive::unordered_set_base_hook<> >  UnorderedSetBaseHookOption;
  typedef boost::intrusive::unordered_bucket<
    UnorderedSetBaseHookOption>::type UnorderedSetBucketType;

  /// hash buckets w/ inline memory for small requests
  struct UnorderedMapBucketTraits {
    mutable boost::container::small_vector<
      UnorderedSetBucketType, MaxBucketInline> buckets;

    void init(size_t count) {
      buckets.resize(count);
    }

    UnorderedSetBucketType *bucket_begin() const {
      return &buckets.front();
    }
    size_t bucket_count() const {
      return buckets.size();
    }
  };

  typedef boost::intrusive::unordered_set<
    ObjectIOT,
    boost::intrusive::key_of_value<ObjectNumberKey>,
    boost::intrusive::bucket_traits<UnorderedMapBucketTraits> > UnorderedMap;

  ObjectIOBase(ObjectExtent &&object_extent)
    : m_object_extent(std::move(object_extent)) {
  }
  virtual ~ObjectIOBase() {
  }

  inline const ObjectExtent &get_object_extent() const {
    return m_object_extent;
  }

  void append_extent(uint32_t object_offset, uint32_t object_length) {
    assert(m_object_extent.get_object_offset() +
           m_object_extent.get_object_length() == object_offset);
    m_object_extent.set_object_length(m_object_extent.get_object_length() +
                                      object_length);
  }

private:
  ObjectExtent m_object_extent;
};

template <typename ObjectIOT, typename BufferExtentT,
          uint32_t MaxInline = 2,
          uint32_t MaxBucketInline = 4>
class BufferExtentObjectIO : public ObjectIOBase<ObjectIOT, MaxInline,
                                                 MaxBucketInline> {
public:
  BufferExtentObjectIO(ObjectExtent &&object_extent)
    : ObjectIOBase<ObjectIOT, MaxInline, MaxBucketInline>(
        std::move(object_extent)) {
  }

  void append_buffer_extent(BufferExtentT *buffer_extent) {
    assert(buffer_extent != nullptr);
    m_buffer_extents.push_back(*buffer_extent);
  }

private:
  typename BufferExtentT::List m_buffer_extents;
};

class ReadObjectIO : public BufferExtentObjectIO<ReadObjectIO,
                                                 ReadBufferExtent> {
public:
  ReadObjectIO(ObjectExtent &&object_extent)
    : BufferExtentObjectIO<ReadObjectIO, ReadBufferExtent>(
        std::move(object_extent)) {
  }

};

class WriteObjectIO : public BufferExtentObjectIO<WriteObjectIO,
                                                  WriteBufferExtent> {
public:
  WriteObjectIO(ObjectExtent &&object_extent)
    : BufferExtentObjectIO<WriteObjectIO, WriteBufferExtent>(
        std::move(object_extent)) {
  }

};

class DiscardObjectIO : public ObjectIOBase<DiscardObjectIO> {
public:
  DiscardObjectIO(ObjectExtent &&object_extent)
    : ObjectIOBase<DiscardObjectIO>(std::move(object_extent)) {
  }

};

enum {
  IMAGE_IO_FLAG_JOURNALED = 1U << 0
};

/// Containers for mapping image extent IO request to individual object IO ops
class ImageIOBase : private boost::noncopyable {
public:
  ImageIOBase(AioCompletion *aio_completion)
    : m_aio_completion(aio_completion) {
  }

  virtual ~ImageIOBase() {
  }

protected:
  AioCompletion *m_aio_completion;
  uint8_t m_flags = 0;

};

class ExtentImageIOBase : public ImageIOBase {
public:
  ExtentImageIOBase(ImageExtent &&image_extent, uint8_t fadvise_flags,
                    AioCompletion *aio_completion)
    : ImageIOBase(aio_completion), m_fadvise_flags(fadvise_flags) {
    m_image_extents.push_back(image_extent);
  }

  ExtentImageIOBase(ImageExtents &&image_extents, uint8_t fadvise_flags,
                    AioCompletion *aio_completion)
    : ImageIOBase(aio_completion), m_image_extents(std::move(image_extents)),
      m_fadvise_flags(fadvise_flags) {
  }

  void map_object_io();

protected:
  ImageExtents m_image_extents;
  uint8_t m_fadvise_flags;

  uint64_t calculate_image_length() const {
    uint64_t image_length = 0;
    for (auto &image_extent : m_image_extents) {
      image_length += image_extent.get_image_length();
    }
    return image_length;
  }

  virtual void set_estimated_object_count(size_t count) = 0;
  virtual void append_extent(ObjectExtent &&object_extent) = 0;

};

template <typename ObjectIOT>
class ExtentImageIO : public ExtentImageIOBase {
public:
  ExtentImageIO(ImageExtent &&image_extent, uint8_t fadvise_flags,
                AioCompletion *aio_completion)
    : ExtentImageIOBase(std::move(image_extent), fadvise_flags, aio_completion),
      m_object_io_map(m_object_io_map_bucket_traits) {
  }

  ExtentImageIO(ImageExtents &&image_extents, uint8_t fadvise_flags,
                AioCompletion *aio_completion)
    : ExtentImageIOBase(std::move(image_extents), fadvise_flags,
                        aio_completion),
      m_object_io_map(m_object_io_map_bucket_traits) {
  }

protected:
  typename ObjectIOT::UnorderedMapBucketTraits m_object_io_map_bucket_traits;
  typename ObjectIOT::UnorderedMap m_object_io_map;
  typename ObjectIOT::List m_object_io_list;

  /// keep a small number of ObjectIO requests inline with this object
  typename ObjectIOT::Pool m_object_io_pool;

  virtual void set_estimated_object_count(size_t count) override {
    if (count > 0) {
      m_object_io_map_bucket_traits.init(count);
    }
  }

  virtual void append_extent(ObjectExtent &&object_extent) override {
    uint32_t object_length = object_extent.get_object_length();

    ObjectIOT *object_io;
    auto object_io_it = m_object_io_map.find(object_extent.get_object_number());
    if (object_io_it == m_object_io_map.end()) {
      object_io = m_object_io_pool.allocate(std::move(object_extent));
      assert(object_io != nullptr);

      m_object_io_map.insert(*object_io);
      m_object_io_list.push_front(*object_io);
    } else {
      object_io = &*(object_io_it);
      object_io->append_extent(object_extent.get_object_offset(),
                               object_extent.get_object_length());
    }

    append_buffer_extent(object_io, object_length);
  }

  virtual void append_buffer_extent(ObjectIOT *object_io, uint64_t length) = 0;

};

class ReadImageIO : public ExtentImageIO<ReadObjectIO> {
public:
  ReadImageIO(ImageExtent &&image_extent, ReadDestination &&read_destination,
              uint8_t fadvise_flags, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extent), fadvise_flags, aio_completion),
      m_read_destination(std::move(read_destination)),
      m_read_buffer_extent(0, image_extent.get_image_length()) {
  }

  ReadImageIO(ImageExtents &&image_extents, ReadDestination &&read_destination,
              uint8_t fadvise_flags, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extents), fadvise_flags, aio_completion),
      m_read_destination(std::move(read_destination)),
      m_read_buffer_extent(0, calculate_image_length()) {
  }

protected:
  virtual void append_buffer_extent(ReadObjectIO *object_io,
                                    uint64_t length) override {
    object_io->append_buffer_extent(m_read_buffer_extent.split_left(
      length, &m_read_buffer_extent_pool));
  }

private:
  ReadDestination m_read_destination;
  ReadBufferExtent m_read_buffer_extent;

  ReadBufferExtent::Pool m_read_buffer_extent_pool;
};

class WriteImageIO : public ExtentImageIO<WriteObjectIO> {
public:
  WriteImageIO(ImageExtent &&image_extent, const ceph::bufferlist &bl,
               uint8_t fadvise_flags, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extent), fadvise_flags, aio_completion),
      m_write_buffer_extent(bl) {
  }

  WriteImageIO(ImageExtents &&image_extents, const ceph::bufferlist &bl,
               uint8_t fadvise_flags, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extents), fadvise_flags, aio_completion),
      m_write_buffer_extent(bl) {
  }

protected:
  virtual void append_buffer_extent(WriteObjectIO *object_io,
                                    uint64_t length) override {
    object_io->append_buffer_extent(m_write_buffer_extent.split_left(
      length, &m_write_buffer_extent_pool));
  }

private:
  WriteBufferExtent m_write_buffer_extent;

  WriteBufferExtent::Pool m_write_buffer_extent_pool;
};

class DiscardImageIO : public ExtentImageIO<DiscardObjectIO> {
public:
  DiscardImageIO(ImageExtent &&image_extent, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extent), 0, aio_completion) {
  }

  DiscardImageIO(ImageExtents &&image_extents, AioCompletion *aio_completion)
    : ExtentImageIO(std::move(image_extents), 0, aio_completion) {
  }

protected:
  virtual void append_buffer_extent(DiscardObjectIO *object_io,
                                    uint64_t length) override {
    // do nothing -- discard doesn't have a buffer
  }
};

class FlushImageIO : public ImageIOBase {
public:
  FlushImageIO(AioCompletion *aio_completion) : ImageIOBase(aio_completion) {
  }

  void map_object_io();

};

class InvalidImageIO {
public:
  void map_object_io();

};

class ImageIO : public boost::variant<InvalidImageIO,
                                      ReadImageIO,
                                      WriteImageIO,
                                      DiscardImageIO,
                                      FlushImageIO>,
                private boost::noncopyable {
public:
  ImageIO() : ImageIO(std::move(InvalidImageIO())) {
  }

  template <typename T>
  ImageIO(T &&t)
    : boost::variant<InvalidImageIO,
                     ReadImageIO,
                     WriteImageIO,
                     DiscardImageIO,
                     FlushImageIO>(std::move(t)) {
  }

  /// map image extents to object extents
  void map_object_io();

};

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_TYPES_H

