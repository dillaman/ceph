// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/rbd_mirror/test_mock_fixture.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "test/librados_test_stub/MockTestMemRadosClient.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "cls/journal/cls_journal_types.h"
#include "journal/Journaler.h"
#include "tools/rbd_mirror/image_replayer/OpenJournalRequest.h"
#include "tools/rbd_mirror/image_replayer/OpenRemoteJournalRequest.h"

namespace librbd {
namespace {

struct MockTestImageCtx : public librbd::MockImageCtx {
  MockTestImageCtx(librbd::ImageCtx &image_ctx)
    : librbd::MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace
} // namespace librbd

namespace rbd {
namespace mirror {
namespace image_replayer {

template <>
struct OpenJournalRequest<librbd::MockTestImageCtx> {
};

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

// template definitions
#include "tools/rbd_mirror/image_replayer/OpenRemoteJournalRequest.cc"
template class rbd::mirror::image_replayer::OpenRemoteJournalRequest<librbd::MockTestImageCtx>;

namespace rbd {
namespace mirror {
namespace image_replayer {

class TestMockImageReplayerOpenRemoteJournalRequest : public TestMockFixture {
public:
  typedef OpenRemoteJournalRequest<librbd::MockTestImageCtx> MockOpenRemoteJournalRequest;

  virtual void SetUp() {
    TestMockFixture::SetUp();

    librbd::RBD rbd;
    ASSERT_EQ(0, create_image(rbd, m_remote_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_remote_io_ctx, m_image_name, &m_remote_image_ctx));
  }

  librbd::ImageCtx *m_remote_image_ctx;
};

TEST_F(TestMockImageReplayerOpenRemoteJournalRequest, Success) {
}

} // namespace image_replayer
} // namespace mirror
} // namespace rbd
