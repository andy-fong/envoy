#include <tuple>

#include "envoy/config/core/v3/proxy_protocol.pb.h"
#include "envoy/extensions/transport_sockets/proxy_protocol/v3/upstream_proxy_protocol.pb.h"
#include "envoy/extensions/transport_sockets/proxy_protocol/v3/upstream_proxy_protocol.pb.validate.h"
#include "envoy/network/proxy_protocol.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/network/transport_socket_options_impl.h"
#include "source/extensions/common/proxy_protocol/proxy_protocol_header.h"
#include "source/extensions/transport_sockets/proxy_protocol/proxy_protocol.h"

#include "test/mocks/buffer/mocks.h"
#include "test/mocks/network/io_handle.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/network/transport_socket.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using envoy::config::core::v3::PerHostConfig;
using envoy::config::core::v3::ProxyProtocolConfig;
using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace ProxyProtocol {
namespace {

constexpr char FallbackRuntimeGuard[] =
    "envoy.reloadable_features.proxy_protocol_v2_custom_tlvs_without_downstream";

class ProxyProtocolFallbackTest : public testing::Test {
public:
  ProxyProtocolFallbackTest()
      : stats_(UpstreamProxyProtocolSocketFactory::generateUpstreamProxyProtocolStats(
            *stats_store_.rootScope())) {}
  void initialize(ProxyProtocolConfig& config,
                  Network::TransportSocketOptionsConstSharedPtr socket_options) {
    auto inner_socket = std::make_unique<testing::StrictMock<Network::MockTransportSocket>>();
    inner_socket_ = inner_socket.get();
    EXPECT_CALL(*inner_socket_, setTransportSocketCallbacks(_));
    EXPECT_CALL(*inner_socket_, onConnected());
    ON_CALL(transport_callbacks_, ioHandle()).WillByDefault(ReturnRef(io_handle_));
    proxy_protocol_socket_ = std::make_unique<UpstreamProxyProtocolSocket>(
        std::move(inner_socket), socket_options, config, stats_);
    proxy_protocol_socket_->setTransportSocketCallbacks(transport_callbacks_);
    proxy_protocol_socket_->onConnected();
  }

  Network::MockTransportSocket* inner_socket_;
  NiceMock<Network::MockIoHandle> io_handle_;
  std::unique_ptr<UpstreamProxyProtocolSocket> proxy_protocol_socket_;
  NiceMock<Network::MockTransportSocketCallbacks> transport_callbacks_;
  Stats::TestUtil::TestStore stats_store_;
  UpstreamProxyProtocolStats stats_;
};

// Connections such as active health checks have no downstream PROXY protocol options.
class ProxyProtocolWithoutDownstreamTest
    : public ProxyProtocolFallbackTest,
      public testing::WithParamInterface<
          std::tuple<Network::Address::IpVersion, bool, bool, bool>> {};

INSTANTIATE_TEST_SUITE_P(IpVersionsAndTlvSources, ProxyProtocolWithoutDownstreamTest,
                         testing::Combine(testing::Values(Network::Address::IpVersion::v4,
                                                          Network::Address::IpVersion::v6),
                                          testing::Bool(), testing::Bool(), testing::Bool()));

TEST_P(ProxyProtocolWithoutDownstreamTest, UsesCustomTLVsOrLegacyLocalAccordingToRuntimeGuard) {
  const auto [ip_version, null_options, host_tlvs, enabled] = GetParam();
  TestScopedRuntime runtime;
  runtime.mergeValues({{FallbackRuntimeGuard, enabled ? "true" : "false"}});
  const bool ipv4 = ip_version == Network::Address::IpVersion::v4;
  auto src_addr =
      *Network::Utility::resolveUrl(ipv4 ? "tcp://1.2.3.4:773" : "tcp://[1:2:3::4]:773");
  auto dst_addr =
      *Network::Utility::resolveUrl(ipv4 ? "tcp://0.1.1.2:513" : "tcp://[1:100:200:3::]:513");
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setLocalAddress(src_addr);
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setRemoteAddress(dst_addr);

  // These are the endpoint hostname and network TLVs required by the peering proxy.
  const std::string hostname = "echo.echo.mesh.internal:8080";
  const std::string network = "kind-cluster-2";
  PerHostConfig host_config;
  auto* hostname_tlv = host_config.add_added_tlvs();
  hostname_tlv->set_type(0xd3);
  hostname_tlv->set_value(hostname);
  auto* network_tlv = host_config.add_added_tlvs();
  network_tlv->set_type(0xd4);
  network_tlv->set_value(network);

  ProxyProtocolConfig config;
  config.set_version(ProxyProtocolConfig::V2);
  if (host_tlvs) {
    auto host = std::make_shared<testing::StrictMock<Upstream::MockHostDescription>>();
    auto metadata = std::make_shared<envoy::config::core::v3::Metadata>();
    auto& typed_metadata =
        (*metadata->mutable_typed_filter_metadata())[Config::MetadataFilters::get()
                                                         .ENVOY_TRANSPORT_SOCKETS_PROXY_PROTOCOL];
    ASSERT_TRUE(typed_metadata.PackFrom(host_config));
    EXPECT_CALL(*host, metadata()).Times(enabled ? 1 : 0).WillRepeatedly(Return(metadata));
    transport_callbacks_.connection_.streamInfo().upstreamInfo()->setUpstreamHost(host);
  } else {
    config.mutable_added_tlvs()->CopyFrom(host_config.added_tlvs());
  }

  initialize(config,
             null_options ? nullptr : std::make_shared<Network::TransportSocketOptionsImpl>());

  Buffer::OwnedImpl expected_buff;
  const std::vector<Network::ProxyProtocolTLV> custom_tlvs = {
      {0xd3, {hostname.begin(), hostname.end()}}, {0xd4, {network.begin(), network.end()}}};
  if (enabled) {
    ASSERT_TRUE(Common::ProxyProtocol::generateV2Header(
        Network::ProxyProtocolData{src_addr, dst_addr}, expected_buff, false, {}, custom_tlvs));
  } else {
    Common::ProxyProtocol::generateV2LocalHeader(expected_buff);
  }
  const auto expected_size = expected_buff.length();
  EXPECT_CALL(io_handle_, write(BufferString(expected_buff.toString())))
      .WillOnce(Invoke([](Buffer::Instance& buffer) -> Api::IoCallUint64Result {
        const auto length = buffer.length();
        buffer.drain(length);
        return {length, Api::IoError::none()};
      }));
  Buffer::OwnedImpl msg("some data");
  EXPECT_CALL(*inner_socket_, doWrite(BufferEqual(&msg), false));
  EXPECT_EQ(expected_size, proxy_protocol_socket_->doWrite(msg, false).bytes_processed_);
}

TEST_F(ProxyProtocolFallbackTest, RuntimeGuardChangesApplyToNewConnections) {
  TestScopedRuntime runtime;
  const auto source = *Network::Utility::resolveUrl("tcp://1.2.3.4:773");
  const auto destination = *Network::Utility::resolveUrl("tcp://0.1.1.2:513");
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setLocalAddress(source);
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setRemoteAddress(destination);
  ProxyProtocolConfig config;
  config.set_version(ProxyProtocolConfig::V2);
  auto* tlv = config.add_added_tlvs();
  tlv->set_type(0x05);
  tlv->set_value("id");
  for (const bool enabled : {false, true, false}) {
    runtime.mergeValues({{FallbackRuntimeGuard, enabled ? "true" : "false"}});
    initialize(config, nullptr);
    Buffer::OwnedImpl expected_header;
    if (enabled) {
      ASSERT_TRUE(Common::ProxyProtocol::generateV2Header(
          Network::ProxyProtocolData{source, destination}, expected_header, false, {},
          {{0x05, {'i', 'd'}}}));
    } else {
      Common::ProxyProtocol::generateV2LocalHeader(expected_header);
    }
    EXPECT_CALL(io_handle_, write(BufferString(expected_header.toString())))
        .WillOnce(Invoke([](Buffer::Instance& buffer) -> Api::IoCallUint64Result {
          const auto length = buffer.length();
          buffer.drain(length);
          return {length, Api::IoError::none()};
        }));
    Buffer::OwnedImpl msg("some data");
    EXPECT_CALL(*inner_socket_, doWrite(BufferEqual(&msg), false));
    proxy_protocol_socket_->doWrite(msg, false);
  }
}

TEST_F(ProxyProtocolFallbackTest, OversizedCustomTLVsKeepHeaderAndCountOverflow) {
  TestScopedRuntime runtime;
  runtime.mergeValues({{FallbackRuntimeGuard, "true"}});
  const auto source = *Network::Utility::resolveUrl("tcp://1.2.3.4:773");
  const auto destination = *Network::Utility::resolveUrl("tcp://0.1.1.2:513");
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setLocalAddress(source);
  transport_callbacks_.connection_.stream_info_.downstream_connection_info_provider_
      ->setRemoteAddress(destination);
  ProxyProtocolConfig config;
  config.set_version(ProxyProtocolConfig::V2);
  auto* tlv = config.add_added_tlvs();
  tlv->set_type(0x05);
  tlv->set_value(std::string(65536, 'a'));
  initialize(config, nullptr);
  Buffer::OwnedImpl expected_header;
  ASSERT_TRUE(Common::ProxyProtocol::generateV2Header(
      Network::ProxyProtocolData{source, destination}, expected_header, false, {}, {}));
  EXPECT_CALL(io_handle_, write(BufferString(expected_header.toString())))
      .WillOnce(Invoke([](Buffer::Instance& buffer) -> Api::IoCallUint64Result {
        const auto length = buffer.length();
        buffer.drain(length);
        return {length, Api::IoError::none()};
      }));
  Buffer::OwnedImpl msg("some data");
  EXPECT_CALL(*inner_socket_, doWrite(BufferEqual(&msg), false));
  proxy_protocol_socket_->doWrite(msg, false);
  EXPECT_EQ(1, stats_.v2_tlvs_exceed_max_length_.value());
}

} // namespace
} // namespace ProxyProtocol
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
