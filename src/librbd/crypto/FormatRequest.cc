// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "FormatRequest.h"

#include "common/dout.h"
#include "common/errno.h"
#include "librbd/ImageCtx.h"
#include "librbd/Utils.h"
#include "librbd/crypto/ShutDownCryptoRequest.h"
#include "librbd/crypto/Utils.h"
#include "librbd/io/ObjectDispatcherInterface.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::crypto::FormatRequest: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace crypto {

using librbd::util::create_context_callback;

template <typename I>
FormatRequest<I>::FormatRequest(
        I* image_ctx, std::unique_ptr<EncryptionFormat<I>> format,
        Context* on_finish) : m_image_ctx(image_ctx),
                              m_format(std::move(format)),
                              m_on_finish(on_finish) {
}

template <typename I>
void FormatRequest<I>::send() {
  if (m_image_ctx->test_features(RBD_FEATURE_JOURNALING)) {
    lderr(m_image_ctx->cct) << "cannot use encryption with journal" << dendl;
    finish(-ENOTSUP);
    return;
  }

  auto ctx = create_context_callback<
          FormatRequest<I>, &FormatRequest<I>::handle_format>(this);
  m_format->format(m_image_ctx, ctx);
}

template <typename I>
void FormatRequest<I>::handle_format(int r) {
  if (r != 0) {
    lderr(m_image_ctx->cct) << "unable to format image: " << cpp_strerror(r)
                            << dendl;
    finish(r);
    return;
  }

  if (m_image_ctx->crypto == nullptr) {
    finish(0);
    return;
  }

  auto ctx = create_context_callback<
        FormatRequest<I>, &FormatRequest<I>::finish>(this);
  auto *req = ShutDownCryptoRequest<I>::create(m_image_ctx, ctx);
  req->send();
}

template <typename I>
void FormatRequest<I>::finish(int r) {
  if (r == 0) {
    util::set_crypto(m_image_ctx, m_format->get_crypto());
  }
  m_on_finish->complete(r);
  delete this;
}

} // namespace crypto
} // namespace librbd

template class librbd::crypto::FormatRequest<librbd::ImageCtx>;
