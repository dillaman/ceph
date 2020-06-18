// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_ASIO_ENGINE_H
#define CEPH_LIBRBD_ASIO_ENGINE_H

#include "include/common_fwd.h"
#include <memory>
#include <boost/asio/io_context.hpp>

namespace ceph { namespace async { struct io_context_pool; }}

namespace librbd {

namespace asio { struct ContextWQ; }

class AsioEngine {
public:
  explicit AsioEngine(CephContext* cct);
  ~AsioEngine();

  AsioEngine(const AsioEngine&) = delete;
  AsioEngine& operator=(const AsioEngine&) = delete;

  inline boost::asio::io_context& get_io_context() {
    return m_io_context;
  }
  inline operator boost::asio::io_context&() {
    return m_io_context;
  }
  inline boost::asio::io_context::executor_type get_executor() {
    return m_io_context.get_executor();
  }

  inline asio::ContextWQ* get_work_queue() {
    return m_context_wq.get();
  }

private:
  CephContext* m_cct;
  std::unique_ptr<ceph::async::io_context_pool> m_io_context_pool;

  boost::asio::io_context& m_io_context;
  std::unique_ptr<asio::ContextWQ> m_context_wq;

};

} // namespace librbd

#endif // CEPH_LIBRBD_ASIO_ENGINE_H
