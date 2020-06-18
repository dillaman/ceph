// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/AsioEngine.h"
#include "common/dout.h"
#include "common/async/context_pool.h"
#include "librbd/asio/ContextWQ.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::AsioEngine: " \
                           << this << " " << __func__ << ": "

namespace librbd {

AsioEngine::AsioEngine(CephContext* cct)
  : m_cct(cct),
    m_io_context_pool(std::make_unique<ceph::async::io_context_pool>(
      m_cct->_conf.get_val<uint64_t>("rbd_op_threads"))),
    m_io_context(m_io_context_pool->get_io_context()),
    m_context_wq(std::make_unique<asio::ContextWQ>(m_io_context)) {
}

AsioEngine::~AsioEngine() {
}

} // namespace librbd
