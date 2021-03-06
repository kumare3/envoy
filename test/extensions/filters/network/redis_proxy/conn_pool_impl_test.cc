#include <memory>
#include <string>

#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

#include "extensions/filters/network/redis_proxy/conn_pool_impl.h"

#include "test/extensions/filters/network/common/redis/mocks.h"
#include "test/extensions/filters/network/redis_proxy/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::InSequence;
using testing::Invoke;
using testing::Ref;
using testing::Return;
using testing::ReturnNew;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RedisProxy {
namespace ConnPool {

envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings
createConnPoolSettings() {
  envoy::config::filter::network::redis_proxy::v2::RedisProxy::ConnPoolSettings setting{};
  setting.mutable_op_timeout()->CopyFrom(Protobuf::util::TimeUtil::MillisecondsToDuration(20));
  setting.set_enable_hashtagging(true);
  return setting;
}

class RedisClientImplTest : public testing::Test, public Common::Redis::DecoderFactory {
public:
  // Commmon::Redis::DecoderFactory
  Common::Redis::DecoderPtr create(Common::Redis::DecoderCallbacks& callbacks) override {
    callbacks_ = &callbacks;
    return Common::Redis::DecoderPtr{decoder_};
  }

  ~RedisClientImplTest() {
    client_.reset();

    // Make sure all gauges are 0.
    for (const Stats::GaugeSharedPtr& gauge : host_->cluster_.stats_store_.gauges()) {
      EXPECT_EQ(0U, gauge->value());
    }
    for (const Stats::GaugeSharedPtr& gauge : host_->stats_store_.gauges()) {
      EXPECT_EQ(0U, gauge->value());
    }
  }

  void setup() {
    config_ = std::make_unique<ConfigImpl>(createConnPoolSettings());
    finishSetup();
  }

  void setup(std::unique_ptr<Config>&& config) {
    config_ = std::move(config);
    finishSetup();
  }

  void finishSetup() {
    upstream_connection_ = new NiceMock<Network::MockClientConnection>();
    Upstream::MockHost::MockCreateConnectionData conn_info;
    conn_info.connection_ = upstream_connection_;
    EXPECT_CALL(*connect_or_op_timer_, enableTimer(_));
    EXPECT_CALL(*host_, createConnection_(_, _)).WillOnce(Return(conn_info));
    EXPECT_CALL(*upstream_connection_, addReadFilter(_))
        .WillOnce(SaveArg<0>(&upstream_read_filter_));
    EXPECT_CALL(*upstream_connection_, connect());
    EXPECT_CALL(*upstream_connection_, noDelay(true));

    client_ = ClientImpl::create(host_, dispatcher_, Common::Redis::EncoderPtr{encoder_}, *this,
                                 *config_);
    EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_total_.value());
    EXPECT_EQ(1UL, host_->stats_.cx_total_.value());

    // NOP currently.
    upstream_connection_->runHighWatermarkCallbacks();
    upstream_connection_->runLowWatermarkCallbacks();
  }

  void onConnected() {
    EXPECT_CALL(*connect_or_op_timer_, enableTimer(_));
    upstream_connection_->raiseEvent(Network::ConnectionEvent::Connected);
  }

  const std::string cluster_name_{"foo"};
  std::shared_ptr<Upstream::MockHost> host_{new NiceMock<Upstream::MockHost>()};
  Event::MockDispatcher dispatcher_;
  Event::MockTimer* connect_or_op_timer_{new Event::MockTimer(&dispatcher_)};
  MockEncoder* encoder_{new MockEncoder()};
  MockDecoder* decoder_{new MockDecoder()};
  Common::Redis::DecoderCallbacks* callbacks_{};
  NiceMock<Network::MockClientConnection>* upstream_connection_{};
  Network::ReadFilterSharedPtr upstream_read_filter_;
  std::unique_ptr<Config> config_;
  ClientPtr client_;
};

TEST_F(RedisClientImplTest, Basic) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();

  Common::Redis::RespValue request2;
  MockPoolCallbacks callbacks2;
  EXPECT_CALL(*encoder_, encode(Ref(request2), _));
  PoolRequest* handle2 = client_->makeRequest(request2, callbacks2);
  EXPECT_NE(nullptr, handle2);

  EXPECT_EQ(2UL, host_->cluster_.stats_.upstream_rq_total_.value());
  EXPECT_EQ(2UL, host_->cluster_.stats_.upstream_rq_active_.value());
  EXPECT_EQ(2UL, host_->stats_.rq_total_.value());
  EXPECT_EQ(2UL, host_->stats_.rq_active_.value());

  Buffer::OwnedImpl fake_data;
  EXPECT_CALL(*decoder_, decode(Ref(fake_data))).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    InSequence s;
    Common::Redis::RespValuePtr response1(new Common::Redis::RespValue());
    EXPECT_CALL(callbacks1, onResponse_(Ref(response1)));
    EXPECT_CALL(*connect_or_op_timer_, enableTimer(_));
    EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SUCCESS));
    callbacks_->onRespValue(std::move(response1));

    Common::Redis::RespValuePtr response2(new Common::Redis::RespValue());
    EXPECT_CALL(callbacks2, onResponse_(Ref(response2)));
    EXPECT_CALL(*connect_or_op_timer_, disableTimer());
    EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SUCCESS));
    callbacks_->onRespValue(std::move(response2));
  }));
  upstream_read_filter_->onData(fake_data, false);

  EXPECT_CALL(*upstream_connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  client_->close();
}

TEST_F(RedisClientImplTest, Cancel) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();

  Common::Redis::RespValue request2;
  MockPoolCallbacks callbacks2;
  EXPECT_CALL(*encoder_, encode(Ref(request2), _));
  PoolRequest* handle2 = client_->makeRequest(request2, callbacks2);
  EXPECT_NE(nullptr, handle2);

  handle1->cancel();

  Buffer::OwnedImpl fake_data;
  EXPECT_CALL(*decoder_, decode(Ref(fake_data))).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    InSequence s;

    Common::Redis::RespValuePtr response1(new Common::Redis::RespValue());
    EXPECT_CALL(callbacks1, onResponse_(_)).Times(0);
    EXPECT_CALL(*connect_or_op_timer_, enableTimer(_));
    EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SUCCESS));
    callbacks_->onRespValue(std::move(response1));

    Common::Redis::RespValuePtr response2(new Common::Redis::RespValue());
    EXPECT_CALL(callbacks2, onResponse_(Ref(response2)));
    EXPECT_CALL(*connect_or_op_timer_, disableTimer());
    EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SUCCESS));
    callbacks_->onRespValue(std::move(response2));
  }));
  upstream_read_filter_->onData(fake_data, false);

  EXPECT_CALL(*upstream_connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  client_->close();

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_rq_cancelled_.value());
}

TEST_F(RedisClientImplTest, FailAll) {
  InSequence s;

  setup();

  NiceMock<Network::MockConnectionCallbacks> connection_callbacks;
  client_->addConnectionCallbacks(connection_callbacks);

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();

  EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SERVER_FAILURE));
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  EXPECT_CALL(connection_callbacks, onEvent(Network::ConnectionEvent::RemoteClose));
  upstream_connection_->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_destroy_with_active_rq_.value());
  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_destroy_remote_with_active_rq_.value());
}

TEST_F(RedisClientImplTest, FailAllWithCancel) {
  InSequence s;

  setup();

  NiceMock<Network::MockConnectionCallbacks> connection_callbacks;
  client_->addConnectionCallbacks(connection_callbacks);

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();
  handle1->cancel();

  EXPECT_CALL(callbacks1, onFailure()).Times(0);
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  EXPECT_CALL(connection_callbacks, onEvent(Network::ConnectionEvent::LocalClose));
  upstream_connection_->raiseEvent(Network::ConnectionEvent::LocalClose);

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_destroy_with_active_rq_.value());
  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_destroy_local_with_active_rq_.value());
  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_rq_cancelled_.value());
}

TEST_F(RedisClientImplTest, ProtocolError) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();

  Buffer::OwnedImpl fake_data;
  EXPECT_CALL(*decoder_, decode(Ref(fake_data))).WillOnce(Invoke([&](Buffer::Instance&) -> void {
    throw Common::Redis::ProtocolError("error");
  }));
  EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::REQUEST_FAILED));
  EXPECT_CALL(*upstream_connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  upstream_read_filter_->onData(fake_data, false);

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_protocol_error_.value());
  EXPECT_EQ(1UL, host_->stats_.rq_error_.value());
}

TEST_F(RedisClientImplTest, ConnectFail) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::SERVER_FAILURE));
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  upstream_connection_->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_connect_fail_.value());
  EXPECT_EQ(1UL, host_->stats_.cx_connect_fail_.value());
}

class ConfigOutlierDisabled : public Config {
  bool disableOutlierEvents() const override { return true; }
  std::chrono::milliseconds opTimeout() const override { return std::chrono::milliseconds(25); }
  bool enableHashtagging() const override { return false; }
};

TEST_F(RedisClientImplTest, OutlierDisabled) {
  InSequence s;

  setup(std::make_unique<ConfigOutlierDisabled>());

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  EXPECT_CALL(host_->outlier_detector_, putResult(_)).Times(0);
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  upstream_connection_->raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_connect_fail_.value());
  EXPECT_EQ(1UL, host_->stats_.cx_connect_fail_.value());
}

TEST_F(RedisClientImplTest, ConnectTimeout) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::TIMEOUT));
  EXPECT_CALL(*upstream_connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  connect_or_op_timer_->callback_();

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_cx_connect_timeout_.value());
  EXPECT_EQ(1UL, host_->stats_.cx_connect_fail_.value());
}

TEST_F(RedisClientImplTest, OpTimeout) {
  InSequence s;

  setup();

  Common::Redis::RespValue request1;
  MockPoolCallbacks callbacks1;
  EXPECT_CALL(*encoder_, encode(Ref(request1), _));
  PoolRequest* handle1 = client_->makeRequest(request1, callbacks1);
  EXPECT_NE(nullptr, handle1);

  onConnected();

  EXPECT_CALL(host_->outlier_detector_, putResult(Upstream::Outlier::Result::TIMEOUT));
  EXPECT_CALL(*upstream_connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(callbacks1, onFailure());
  EXPECT_CALL(*connect_or_op_timer_, disableTimer());
  connect_or_op_timer_->callback_();

  EXPECT_EQ(1UL, host_->cluster_.stats_.upstream_rq_timeout_.value());
  EXPECT_EQ(1UL, host_->stats_.rq_timeout_.value());
}

TEST(RedisClientFactoryImplTest, Basic) {
  ClientFactoryImpl factory;
  Upstream::MockHost::MockCreateConnectionData conn_info;
  conn_info.connection_ = new NiceMock<Network::MockClientConnection>();
  std::shared_ptr<Upstream::MockHost> host(new NiceMock<Upstream::MockHost>());
  EXPECT_CALL(*host, createConnection_(_, _)).WillOnce(Return(conn_info));
  NiceMock<Event::MockDispatcher> dispatcher;
  ConfigImpl config(createConnPoolSettings());
  ClientPtr client = factory.create(host, dispatcher, config);
  client->close();
}

class RedisConnPoolImplTest : public testing::Test, public ClientFactory {
public:
  void setup(bool cluster_exists = true) {
    EXPECT_CALL(cm_, addThreadLocalClusterUpdateCallbacks_(_))
        .WillOnce(DoAll(SaveArgAddress(&update_callbacks_),
                        ReturnNew<Upstream::MockClusterUpdateCallbacksHandle>()));
    if (!cluster_exists) {
      EXPECT_CALL(cm_, get("fake_cluster")).WillOnce(Return(nullptr));
    }
    conn_pool_ =
        std::make_unique<InstanceImpl>(cluster_name_, cm_, *this, tls_, createConnPoolSettings());
  }

  void makeSimpleRequest(bool create_client) {
    EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_));
    if (create_client) {
      client_ = new NiceMock<MockClient>();
      EXPECT_CALL(*this, create_(_)).WillOnce(Return(client_));
    }
    Common::Redis::RespValue value;
    MockPoolCallbacks callbacks;
    MockPoolRequest active_request;
    EXPECT_CALL(*client_, makeRequest(Ref(value), Ref(callbacks)))
        .WillOnce(Return(&active_request));
    PoolRequest* request = conn_pool_->makeRequest("hash_key", value, callbacks);
    EXPECT_EQ(&active_request, request);
  }

  // RedisProxy::ConnPool::ClientFactory
  ClientPtr create(Upstream::HostConstSharedPtr host, Event::Dispatcher&, const Config&) override {
    return ClientPtr{create_(host)};
  }

  MOCK_METHOD1(create_, Client*(Upstream::HostConstSharedPtr host));

  const std::string cluster_name_{"fake_cluster"};
  NiceMock<Upstream::MockClusterManager> cm_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  InstancePtr conn_pool_;
  Upstream::ClusterUpdateCallbacks* update_callbacks_{};
  MockClient* client_{};
};

TEST_F(RedisConnPoolImplTest, Basic) {
  InSequence s;

  setup();

  Common::Redis::RespValue value;
  MockPoolRequest active_request;
  MockPoolCallbacks callbacks;
  MockClient* client = new NiceMock<MockClient>();

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_))
      .WillOnce(Invoke([&](Upstream::LoadBalancerContext* context) -> Upstream::HostConstSharedPtr {
        EXPECT_EQ(context->computeHashKey().value(), MurmurHash::murmurHash2_64("hash_key"));
        EXPECT_EQ(context->metadataMatchCriteria(), nullptr);
        EXPECT_EQ(context->downstreamConnection(), nullptr);
        return cm_.thread_local_cluster_.lb_.host_;
      }));
  EXPECT_CALL(*this, create_(_)).WillOnce(Return(client));
  EXPECT_CALL(*client, makeRequest(Ref(value), Ref(callbacks))).WillOnce(Return(&active_request));
  PoolRequest* request = conn_pool_->makeRequest("hash_key", value, callbacks);
  EXPECT_EQ(&active_request, request);

  EXPECT_CALL(*client, close());
  tls_.shutdownThread();
};

TEST_F(RedisConnPoolImplTest, Hashtagging) {
  InSequence s;

  setup();

  Common::Redis::RespValue value;
  MockPoolCallbacks callbacks;

  auto expectHashKey = [](const std::string& s) {
    return [s](Upstream::LoadBalancerContext* context) -> Upstream::HostConstSharedPtr {
      EXPECT_EQ(context->computeHashKey().value(), MurmurHash::murmurHash2_64(s));
      return nullptr;
    };
  };

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Invoke(expectHashKey("foo")));
  conn_pool_->makeRequest("{foo}.bar", value, callbacks);

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_))
      .WillOnce(Invoke(expectHashKey("foo{}{bar}")));
  conn_pool_->makeRequest("foo{}{bar}", value, callbacks);

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Invoke(expectHashKey("{bar")));
  conn_pool_->makeRequest("foo{{bar}}zap", value, callbacks);

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Invoke(expectHashKey("bar")));
  conn_pool_->makeRequest("foo{bar}{zap}", value, callbacks);

  tls_.shutdownThread();
};

// Conn pool created when no cluster exists at creation time. Dynamic cluster creation and removal
// work correctly.
TEST_F(RedisConnPoolImplTest, NoClusterAtConstruction) {
  InSequence s;

  setup(false);

  Common::Redis::RespValue value;
  MockPoolCallbacks callbacks;
  PoolRequest* request = conn_pool_->makeRequest("hash_key", value, callbacks);
  EXPECT_EQ(nullptr, request);

  // Now add the cluster. Request to the cluster should succeed.
  update_callbacks_->onClusterAddOrUpdate(cm_.thread_local_cluster_);
  makeSimpleRequest(true);

  // Remove the cluster. Request to the cluster should fail.
  EXPECT_CALL(*client_, close());
  update_callbacks_->onClusterRemoval("fake_cluster");
  request = conn_pool_->makeRequest("hash_key", value, callbacks);
  EXPECT_EQ(nullptr, request);

  // Add a cluster we don't care about.
  NiceMock<Upstream::MockThreadLocalCluster> cluster2;
  cluster2.cluster_.info_->name_ = "cluster2";
  update_callbacks_->onClusterAddOrUpdate(cluster2);

  // Add the cluster back. Request to the cluster should succeed.
  update_callbacks_->onClusterAddOrUpdate(cm_.thread_local_cluster_);
  makeSimpleRequest(true);

  // Remove a cluster we don't care about. Request to the cluster should succeed.
  update_callbacks_->onClusterRemoval("some_other_cluster");
  makeSimpleRequest(false);

  // Update the cluster. This should count as a remove followed by an add. Request to the cluster
  // should succeed.
  EXPECT_CALL(*client_, close());
  update_callbacks_->onClusterAddOrUpdate(cm_.thread_local_cluster_);
  makeSimpleRequest(true);

  // Remove the cluster to make sure we safely destruct with no cluster.
  EXPECT_CALL(*client_, close());
  update_callbacks_->onClusterRemoval("fake_cluster");
}

TEST_F(RedisConnPoolImplTest, HostRemove) {
  InSequence s;

  setup();

  MockPoolCallbacks callbacks;
  Common::Redis::RespValue value;
  std::shared_ptr<Upstream::Host> host1(new Upstream::MockHost());
  std::shared_ptr<Upstream::Host> host2(new Upstream::MockHost());
  MockClient* client1 = new NiceMock<MockClient>();
  MockClient* client2 = new NiceMock<MockClient>();

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Return(host1));
  EXPECT_CALL(*this, create_(Eq(host1))).WillOnce(Return(client1));

  MockPoolRequest active_request1;
  EXPECT_CALL(*client1, makeRequest(Ref(value), Ref(callbacks))).WillOnce(Return(&active_request1));
  PoolRequest* request1 = conn_pool_->makeRequest("hash_key", value, callbacks);
  EXPECT_EQ(&active_request1, request1);

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Return(host2));
  EXPECT_CALL(*this, create_(Eq(host2))).WillOnce(Return(client2));

  MockPoolRequest active_request2;
  EXPECT_CALL(*client2, makeRequest(Ref(value), Ref(callbacks))).WillOnce(Return(&active_request2));
  PoolRequest* request2 = conn_pool_->makeRequest("bar", value, callbacks);
  EXPECT_EQ(&active_request2, request2);

  EXPECT_CALL(*client2, close());
  cm_.thread_local_cluster_.cluster_.prioritySet().getMockHostSet(0)->runCallbacks({}, {host2});

  EXPECT_CALL(*client1, close());
  tls_.shutdownThread();
}

TEST_F(RedisConnPoolImplTest, DeleteFollowedByClusterUpdateCallback) {
  setup();
  conn_pool_.reset();

  std::shared_ptr<Upstream::Host> host(new Upstream::MockHost());
  cm_.thread_local_cluster_.cluster_.prioritySet().getMockHostSet(0)->runCallbacks({}, {host});
}

TEST_F(RedisConnPoolImplTest, NoHost) {
  InSequence s;

  setup();

  Common::Redis::RespValue value;
  MockPoolCallbacks callbacks;
  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_)).WillOnce(Return(nullptr));
  PoolRequest* request = conn_pool_->makeRequest("hash_key", value, callbacks);
  EXPECT_EQ(nullptr, request);

  tls_.shutdownThread();
}

TEST_F(RedisConnPoolImplTest, RemoteClose) {
  InSequence s;

  setup();

  Common::Redis::RespValue value;
  MockPoolRequest active_request;
  MockPoolCallbacks callbacks;
  MockClient* client = new NiceMock<MockClient>();

  EXPECT_CALL(cm_.thread_local_cluster_.lb_, chooseHost(_));
  EXPECT_CALL(*this, create_(_)).WillOnce(Return(client));
  EXPECT_CALL(*client, makeRequest(Ref(value), Ref(callbacks))).WillOnce(Return(&active_request));
  conn_pool_->makeRequest("hash_key", value, callbacks);

  EXPECT_CALL(tls_.dispatcher_, deferredDelete_(_));
  client->runHighWatermarkCallbacks();
  client->runLowWatermarkCallbacks();
  client->raiseEvent(Network::ConnectionEvent::RemoteClose);

  tls_.shutdownThread();
}

} // namespace ConnPool
} // namespace RedisProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
