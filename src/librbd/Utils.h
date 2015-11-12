// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_UTILS_H
#define CEPH_LIBRBD_UTILS_H

#include "include/rados/librados.hpp"
#include "include/Context.h"

class Context;

namespace librbd {
namespace util {

namespace detail {

template <typename T>
void rados_callback(rados_completion_t c, void *arg) {
  reinterpret_cast<T*>(arg)->complete(rados_aio_get_return_value(c));
}

template <typename T, Context*(T::*MF)(int*), bool destroy>
void rados_state_callback(rados_completion_t c, void *arg) {
  T *obj = reinterpret_cast<T*>(arg);
  int r = rados_aio_get_return_value(c);
  Context *on_finish = (obj->*MF)(&r);
  if (on_finish != nullptr) {
    on_finish->complete(r);
    if (destroy) {
      delete obj;
    }
  }
}

template <typename T, void (T::*MF)(int)>
class C_CallbackAdapter : public Context {
  T *obj;
public:
  C_CallbackAdapter(T *obj) : obj(obj) {
  }

protected:
  virtual void finish(int r) {
    (obj->*MF)(r);
  }
};

template <typename T, Context*(T::*MF)(int*), bool destroy>
class C_StateCallbackAdapter : public Context {
  T *obj;
public:
  C_StateCallbackAdapter(T *obj) : obj(obj){
  }

protected:
  virtual void complete(int r) override {
    Context *on_finish = (obj->*MF)(&r);
    if (on_finish != nullptr) {
      on_finish->complete(r);
      if (destroy) {
        delete obj;
      }
      Context::complete(r);
    }
  }
  virtual void finish(int r) override {
  }
};

} // namespace detail

const std::string id_obj_name(const std::string &name);
const std::string header_name(const std::string &image_id);
const std::string old_header_name(const std::string &image_name);
std::string unique_lock_name(const std::string &name, void *address);

template <typename T>
librados::AioCompletion *create_rados_ack_callback(T *obj) {
  return librados::Rados::aio_create_completion(
    obj, &detail::rados_callback<T>, nullptr);
}

template <typename T, Context*(T::*MF)(int*), bool destroy=true>
librados::AioCompletion *create_rados_ack_callback(T *obj) {
  return librados::Rados::aio_create_completion(
    obj, &detail::rados_state_callback<T, MF, destroy>, nullptr);
}

template <typename T>
librados::AioCompletion *create_rados_safe_callback(T *obj) {
  return librados::Rados::aio_create_completion(
    obj, nullptr, &detail::rados_callback<T>);
}

template <typename T, Context*(T::*MF)(int*), bool destroy=true>
librados::AioCompletion *create_rados_safe_callback(T *obj) {
  return librados::Rados::aio_create_completion(
    obj, nullptr, &detail::rados_state_callback<T, MF, destroy>);
}

template <typename T, void(T::*MF)(int) = &T::complete>
Context *create_context_callback(T *obj) {
  return new detail::C_CallbackAdapter<T, MF>(obj);
}

template <typename T, Context*(T::*MF)(int*), bool destroy=true>
Context *create_context_callback(T *obj) {
  return new detail::C_StateCallbackAdapter<T, MF, destroy>(obj);
}

} // namespace util
} // namespace librbd

#endif // CEPH_LIBRBD_UTILS_H
