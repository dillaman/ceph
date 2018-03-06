// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/AioCompletion.h"
#include <errno.h>

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/perf_counters.h"
#include "common/WorkQueue.h"

#include "librbd/ImageCtx.h"
#include "librbd/internal.h"
#include "librbd/Journal.h"
#include "librbd/Types.h"

#ifdef WITH_LTTNG
#include "tracing/librbd.h"
#else
#define tracepoint(...)
#endif

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::AioCompletion: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace io {

int AioCompletion::wait_for_complete() {
  tracepoint(librbd, aio_wait_for_complete_enter, this);
  {
    std::unique_lock<std::mutex> locker(lock);
    while (state != AIO_STATE_COMPLETE) {
      cond.wait(locker);
    }
  }
  tracepoint(librbd, aio_wait_for_complete_exit, 0);
  return 0;
}

void AioCompletion::finalize() {
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  ssize_t r = (error_rval < 0 ? error_rval : rval);
  ldout(cct, 20) << "r=" << r << dendl;
  if (r >= 0 && aio_type == AIO_TYPE_READ) {
    read_result.assemble_result(cct);
  }
}

void AioCompletion::complete() {
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  ssize_t r = (error_rval < 0 ? error_rval : rval);
  tracepoint(librbd, aio_complete_enter, this, rval);
  if (ictx->perfcounter != nullptr) {
    ceph::timespan elapsed = coarse_mono_clock::now() - start_time;
    switch (aio_type) {
    case AIO_TYPE_GENERIC:
    case AIO_TYPE_OPEN:
    case AIO_TYPE_CLOSE:
      break;
    case AIO_TYPE_READ:
      ictx->perfcounter->tinc(l_librbd_rd_latency, elapsed); break;
    case AIO_TYPE_WRITE:
      ictx->perfcounter->tinc(l_librbd_wr_latency, elapsed); break;
    case AIO_TYPE_DISCARD:
      ictx->perfcounter->tinc(l_librbd_discard_latency, elapsed); break;
    case AIO_TYPE_FLUSH:
      ictx->perfcounter->tinc(l_librbd_flush_latency, elapsed); break;
    case AIO_TYPE_WRITESAME:
      ictx->perfcounter->tinc(l_librbd_ws_latency, elapsed); break;
    case AIO_TYPE_COMPARE_AND_WRITE:
      ictx->perfcounter->tinc(l_librbd_cmp_latency, elapsed); break;
    default:
      lderr(cct) << "completed invalid aio_type: " << aio_type << dendl;
      break;
    }
  }

  state = AIO_STATE_CALLBACK;
  if (complete_cb) {
    complete_cb(rbd_comp, complete_arg);
  }
  state = AIO_STATE_COMPLETE;

  if (event_notify && ictx->event_socket.is_valid()) {
    ictx->completed_reqs.push(this);
    ictx->event_socket.notify();
  }

  {
    std::unique_lock<std::mutex> locker(lock);
    cond.notify_all();
  }

  // note: possible for image to be closed after op marked finished
  if (async_op.started()) {
    async_op.finish_op();
  }
  tracepoint(librbd, aio_complete_exit);
}

void AioCompletion::init_time(ImageCtx *i, aio_type_t t) {
  if (ictx == nullptr) {
    ictx = i;
    aio_type = t;
    start_time = coarse_mono_clock::now();
  }
}

void AioCompletion::start_op(bool ignore_type) {
  assert(ictx != nullptr);
  assert(!async_op.started());
  if (state == AIO_STATE_PENDING &&
      (ignore_type || aio_type != AIO_TYPE_FLUSH)) {
    async_op.start_op(*ictx);
  }
}

void AioCompletion::fail(int r)
{
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;
  lderr(cct) << cpp_strerror(r) << dendl;

  assert(pending_count == 0);

  error_rval = r;
  complete();
  put();
}

void AioCompletion::set_request_count(uint32_t count) {
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;
  ldout(cct, 20) << "pending=" << count << dendl;

  if (count == 0) {
    finalize();
    complete();
    return;
  }

  uint32_t previous_pending_count = pending_count.exchange(count);
  assert(previous_pending_count == 0);
}

void AioCompletion::complete_request(ssize_t r)
{
  uint32_t previous_pending_count = pending_count--;
  assert(previous_pending_count > 0);

  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  if (r > 0) {
    rval += r;
  } else if (r != -EEXIST) {
    // might race w/ another thread setting an error code but
    // first one wins
    int zero = 0;
    error_rval.compare_exchange_weak(zero, r);
  }

  ldout(cct, 20) << "cb=" << complete_cb << ", "
                 << "pending=" << (previous_pending_count - 1) << dendl;
  if (previous_pending_count == 1) {
    finalize();
    complete();
  }
  put();
}

bool AioCompletion::is_complete() {
  tracepoint(librbd, aio_is_complete_enter, this);
  bool done = (this->state == AIO_STATE_COMPLETE);
  tracepoint(librbd, aio_is_complete_exit, done);
  return done;
}

ssize_t AioCompletion::get_return_value() {
  tracepoint(librbd, aio_get_return_value_enter, this);
  ssize_t r = (error_rval < 0 ? error_rval : rval);
  tracepoint(librbd, aio_get_return_value_exit, r);
  return r;
}

} // namespace io
} // namespace librbd
