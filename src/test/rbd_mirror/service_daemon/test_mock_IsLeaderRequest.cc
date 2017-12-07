// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/rbd_mirror/test_mock_fixture.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "test/librados_test_stub/MockTestMemRadosClient.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "tools/rbd_mirror/service_daemon/IsLeaderRequest.h"

namespace librbd {
namespace {

struct MockTestImageCtx : public librbd::MockImageCtx {
  MockTestImageCtx(librbd::ImageCtx &image_ctx)
    : librbd::MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace
} // namespace librbd

#include "tools/rbd_mirror/service_daemon/IsLeaderRequest.cc"

namespace rbd {
namespace mirror {
namespace service_daemon {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArg;

class TestMockServiceDaemonIsLeaderRequest : public TestMockFixture {
public:
  typedef IsLeaderRequest<librbd::MockTestImageCtx> MockIsLeaderRequest;

  void expect_get_instance_id(librados::IoCtx& io_ctx, uint64_t id) {
    EXPECT_CALL(*get_mock_io_ctx(io_ctx).get_mock_rados_client(),
                get_instance_id())
      .WillOnce(Return(id));
  }

  void expect_mgr_command(librados::IoCtx& io_ctx, const std::string& response,
                          int r) {
    EXPECT_CALL(*get_mock_io_ctx(io_ctx).get_mock_rados_client(),
                mgr_command(_, _, _, _))
      .WillOnce(WithArg<2>(Invoke([response, r](bufferlist *outbl) {
                             outbl->append(response);
                             return r;
                           })));
  }
};

TEST_F(TestMockServiceDaemonIsLeaderRequest, Leader) {
  std::stringstream response;
  response << "{\"services\": {\"rbd-mirror\": {\"daemons\": {"
           << "\"234\": {\"start_epoch\": 123, \"gid\": 234}"
           << "}}}}";

  InSequence seq;
  expect_mgr_command(m_local_io_ctx, response.str(), 0);
  expect_get_instance_id(m_local_io_ctx, 234);

  bool is_leader;
  C_SaferCond ctx;
  auto req = MockIsLeaderRequest::create(RadosRef{new librados::Rados(m_local_io_ctx)},
                                         &is_leader, &ctx);
  req->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_TRUE(is_leader);
}

TEST_F(TestMockServiceDaemonIsLeaderRequest, NonLeader) {
  std::stringstream response;
  response << "{\"services\": {\"rbd-mirror\": {\"daemons\": {"
           << "\"345\": {\"start_epoch\": 123, \"gid\": 345},"
           << "\"234\": {\"start_epoch\": 124, \"gid\": 234}"
           << "}}}}";

  InSequence seq;
  expect_mgr_command(m_local_io_ctx, response.str(), 0);
  expect_get_instance_id(m_local_io_ctx, 234);

  bool is_leader;
  C_SaferCond ctx;
  auto req = MockIsLeaderRequest::create(RadosRef{new librados::Rados(m_local_io_ctx)},
                                         &is_leader, &ctx);
  req->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_FALSE(is_leader);
}

TEST_F(TestMockServiceDaemonIsLeaderRequest, NotRegistered) {
  bool is_leader;
  C_SaferCond ctx;
  auto req = MockIsLeaderRequest::create(RadosRef{new librados::Rados(m_local_io_ctx)},
                                         &is_leader, &ctx);
  req->send();
  ASSERT_EQ(-ENOENT, ctx.wait());
}

} // namespace service_daemon
} // namespace mirror
} // namespace rbd
