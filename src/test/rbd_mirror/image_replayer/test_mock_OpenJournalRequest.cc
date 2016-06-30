// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/rbd_mirror/test_mock_fixture.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "test/librados_test_stub/MockTestMemRadosClient.h"
#include "test/journal/mock/MockJournaler.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "cls/journal/cls_journal_types.h"
#include "journal/Journaler.h"
#include "tools/rbd_mirror/image_replayer/OpenJournalRequest.h"
#include "tools/rbd_mirror/image_replayer/Utils.h"
#include "tools/rbd_mirror/Threads.h"

namespace journal {
namespace {

struct MockTestJournaler : public journal::MockJournaler {
  static MockTestJournaler *s_instance;
  static MockTestJournaler *get_instance() {
    assert(s_instance != nullptr);
    return s_instance;
  }

  MockTestJournaler() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~MockTestJournaler() {
    s_instance = nullptr;
  }
};

MockTestJournaler *MockTestJournaler::s_instance = nullptr;

} // anonymous namespace
} // namespace journal

namespace librbd {
namespace {

struct MockTestImageCtx : public librbd::MockImageCtx {
  MockTestImageCtx(librbd::ImageCtx &image_ctx)
    : librbd::MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace

namespace journal {

template <>
struct TypeTraits<librbd::MockTestImageCtx> {
  typedef ::journal::MockTestJournaler Journaler;
};

} // namespace journal
} // namespace librbd

namespace rbd {
namespace mirror {
namespace image_replayer {
namespace utils {

template <>
typename librbd::journal::TypeTraits<librbd::MockTestImageCtx>::Journaler *
create_journaler<librbd::MockTestImageCtx>(
    ContextWQ *work_queue, SafeTimer *timer, Mutex *timer_lock,
    librados::IoCtx &header_ioctx, librbd::MockTestImageCtx *dummy_image_ctx,
    const std::string &journal_id, const std::string &client_id,
    double commit_interval) {
  return journal::MockTestJournaler::get_instance();
}

} // namespace utils
} // namespace image_replayer
} // namespace mirror
} // namespace rbd

// template definitions
#include "tools/rbd_mirror/image_replayer/OpenJournalRequest.cc"
template class rbd::mirror::image_replayer::OpenJournalRequest<librbd::MockTestImageCtx>;

namespace rbd {
namespace mirror {
namespace image_replayer {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::WithArg;

class TestMockImageReplayerOpenJournalRequest : public TestMockFixture {
public:
  typedef OpenJournalRequest<librbd::MockTestImageCtx> MockOpenJournalRequest;

  virtual void SetUp() {
    TestMockFixture::SetUp();

    librbd::RBD rbd;
    ASSERT_EQ(0, create_image(rbd, m_remote_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_remote_io_ctx, m_image_name, &m_remote_image_ctx));
  }

  void expect_init_journaler(journal::MockTestJournaler &mock_journaler,
                             int r) {
    EXPECT_CALL(mock_journaler, init(_))
      .WillOnce(Invoke([this, r](Context *ctx) {
                  m_threads->work_queue->queue(ctx, r);
                }));
  }

  void expect_shut_down_journaler(journal::MockTestJournaler &mock_journaler,
                                  int r) {
    EXPECT_CALL(mock_journaler, shut_down(_))
      .WillOnce(Invoke([this, r](Context *ctx) {
                  m_threads->work_queue->queue(ctx, r);
                }));
  }

  void expect_get_cached_client(journal::MockTestJournaler &mock_journaler,
                                const std::string &client_id,
                                const cls::journal::Client &client, int r) {
    EXPECT_CALL(mock_journaler, get_cached_client(client_id, _))
      .WillOnce(DoAll(WithArg<1>(Invoke([client](cls::journal::Client *out_client) {
                                   *out_client = client;
                                 })),
                      Return(r)));
  }

  void expect_tag_list(journal::MockTestJournaler &mock_journaler,
                       uint64_t tag_class, uint64_t start_after_tag_tid,
                       const MockOpenJournalRequest::TagDataList &tag_data_list,
                       int r) {
    std::list<cls::journal::Tag> tags;
    for (auto &tag_data : tag_data_list) {
      bufferlist tag_data_bl;
      ::encode(tag_data.second, tag_data_bl);

      cls::journal::Tag tag{tag_data.first, tag_class, tag_data_bl};
      tags.push_back(tag);
    }

    EXPECT_CALL(mock_journaler, get_tags(tag_class, start_after_tag_tid, _, _))
      .WillOnce(DoAll(WithArg<2>(Invoke([tags](std::list<cls::journal::Tag> *out_tags) {
                                   *out_tags = tags;
                                 })),
                      WithArg<3>(Invoke([this, r](Context *ctx) {
                                   m_threads->work_queue->queue(ctx, r);
                                 }))));
  }

  MockOpenJournalRequest *create_request(const std::string &client_id,
                                         journal::MockTestJournaler **journaler,
                                         MockOpenJournalRequest::TagDataList *tag_data_list,
                                         Context *on_finish) {
    return new MockOpenJournalRequest(m_remote_io_ctx, m_remote_image_ctx->id,
                                      client_id, m_threads->work_queue,
                                      m_threads->timer, &m_threads->timer_lock,
                                      tag_data_list,
                                      &m_mirror_peer_client_meta,
                                      journaler, on_finish);
  }

  librbd::ImageCtx *m_remote_image_ctx;
  librbd::journal::MirrorPeerClientMeta m_mirror_peer_client_meta;
};

TEST_F(TestMockImageReplayerOpenJournalRequest, SuccessImage) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_tag_list(*mock_journaler, 123, 0, {}, 0);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  MockOpenJournalRequest::TagDataList expected_tag_data_list;
  ASSERT_EQ(expected_tag_data_list, tag_data_list);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, SuccessMirrorPeer) {
  journal::MockTestJournaler mock_journaler;

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    "image id", librbd::journal::MIRROR_PEER_STATE_REPLAYING, 123,
    {}, {}};
  librbd::journal::ClientData peer_client_data{mirror_peer_client_meta};
  bufferlist mirror_client_data_bl;
  ::encode(peer_client_data, mirror_client_data_bl);
  cls::journal::Client peer_client{"mirror uuid", mirror_client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(mock_journaler, 0);
  expect_get_cached_client(mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_get_cached_client(mock_journaler, "mirror uuid", peer_client, 0);
  expect_tag_list(mock_journaler, 123, 0, {}, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request("mirror uuid",
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_EQ(&mock_journaler, opened_mock_journaler);

  MockOpenJournalRequest::TagDataList expected_tag_data_list;
  ASSERT_EQ(expected_tag_data_list, tag_data_list);
  ASSERT_EQ(mirror_peer_client_meta, m_mirror_peer_client_meta);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, StartAtCommitPosition) {
  journal::MockTestJournaler mock_journaler;

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {{{1, 234, 0}, {0, 1, 2}}}};

  InSequence seq;
  expect_init_journaler(mock_journaler, 0);
  expect_get_cached_client(mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_tag_list(mock_journaler, 123, 233,
                  {{234, {"mirror uuid 1"}}, {235, {"mirror uuid 2"}}}, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_EQ(&mock_journaler, opened_mock_journaler);

  MockOpenJournalRequest::TagDataList expected_tag_data_list = {
    {234, {"mirror uuid 1"}}, {235, {"mirror uuid 2"}}};
  ASSERT_EQ(expected_tag_data_list, tag_data_list);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, InitError) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  InSequence seq;
  expect_init_journaler(*mock_journaler, -EINVAL);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
  ASSERT_FALSE(opened_mock_journaler);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, ImageClientDataError) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, -EINVAL);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageReplayerOpenJournalRequest, ImageClientDataCorrupt) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           {}, 0);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(-EBADMSG, ctx.wait());
}

TEST_F(TestMockImageReplayerOpenJournalRequest, ImageClientDataInvalid) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::MirrorPeerClientMeta{}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageReplayerOpenJournalRequest, MirrorPeerNotRegistered) {
  journal::MockTestJournaler mock_journaler;

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(mock_journaler, 0);
  expect_get_cached_client(mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_get_cached_client(mock_journaler, "mirror uuid", {}, -ENOENT);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request("mirror uuid",
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_EQ(librbd::journal::MIRROR_PEER_STATE_UNREGISTERED,
            m_mirror_peer_client_meta.state);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, MirrorPeerClientDataError) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_get_cached_client(*mock_journaler, "mirror uuid", {}, -EINVAL);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request("mirror uuid",
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
  ASSERT_FALSE(opened_mock_journaler);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, MirrorPeerClientDataCorrupt) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_get_cached_client(*mock_journaler, "mirror uuid", {}, 0);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request("mirror uuid",
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(-EBADMSG, ctx.wait());
  ASSERT_FALSE(opened_mock_journaler);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, MirrorPeerClientDataInvalid) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_get_cached_client(*mock_journaler, "mirror uuid", client, 0);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  journal::MockTestJournaler *opened_mock_journaler;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request("mirror uuid",
                                                   &opened_mock_journaler,
                                                   &tag_data_list, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
  ASSERT_FALSE(opened_mock_journaler);
}

TEST_F(TestMockImageReplayerOpenJournalRequest, TagListError) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  expect_tag_list(*mock_journaler, 123, 0, {}, -EINVAL);
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageReplayerOpenJournalRequest, TagDataCorrupt) {
  journal::MockTestJournaler *mock_journaler = new journal::MockTestJournaler();

  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);
  cls::journal::Client client{librbd::Journal<>::IMAGE_CLIENT_ID,
                              client_data_bl, {}};

  InSequence seq;
  expect_init_journaler(*mock_journaler, 0);
  expect_get_cached_client(*mock_journaler, librbd::Journal<>::IMAGE_CLIENT_ID,
                           client, 0);
  EXPECT_CALL(*mock_journaler, get_tags(123, 0,  _, _))
    .WillOnce(DoAll(WithArg<2>(Invoke([](std::list<cls::journal::Tag> *out_tags) {
                                 *out_tags = {{234, 123, {}}};
                               })),
                    WithArg<3>(Invoke([this](Context *ctx) {
                                 m_threads->work_queue->queue(ctx, 0);
                               }))));
  expect_shut_down_journaler(*mock_journaler, 0);

  C_SaferCond ctx;
  MockOpenJournalRequest::TagDataList tag_data_list;
  MockOpenJournalRequest *request = create_request(librbd::Journal<>::IMAGE_CLIENT_ID,
                                                   nullptr, &tag_data_list,
                                                   &ctx);
  request->send();
  ASSERT_EQ(-EBADMSG, ctx.wait());
}

} // namespace image_replayer
} // namespace mirror
} // namespace rbd
