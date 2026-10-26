#include <tuple>

#include "envoy/config/core/v3/health_check.pb.h"
#include "envoy/config/core/v3/proxy_protocol.pb.h"
#include "envoy/extensions/transport_sockets/proxy_protocol/v3/upstream_proxy_protocol.pb.h"
#include "envoy/extensions/transport_sockets/raw_buffer/v3/raw_buffer.pb.h"

#include "source/common/network/utility.h"

#include "test/integration/integration.h"
#include "test/test_common/environment.h"

namespace Envoy {
namespace {

using envoy::config::core::v3::PerHostConfig;
constexpr char FallbackRuntimeGuard[] =
    "envoy.reloadable_features.proxy_protocol_v2_custom_tlvs_without_downstream";

std::string localHeader() {
  return std::string("\r\n\r\n\0\r\nQUIT\n", 12) + std::string("\x20\0\0\0", 4);
}

class ProxyProtocolFallbackIntegrationTest
    : public testing::TestWithParam<std::tuple<Network::Address::IpVersion, bool>>,
      public BaseIntegrationTest {
public:
  ProxyProtocolFallbackIntegrationTest()
      : BaseIntegrationTest(std::get<0>(GetParam()), ConfigHelper::tcpProxyConfig()) {
    config_helper_.addRuntimeOverride(FallbackRuntimeGuard,
                                      std::get<1>(GetParam()) ? "true" : "false");
  }

  void TearDown() override {
    test_server_.reset();
    fake_upstream_connection_.reset();
    fake_upstreams_.clear();
  }

  void initialize() override {
    config_helper_.addConfigModifier([this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);
      auto* socket = cluster->mutable_transport_socket();
      socket->set_name("envoy.transport_sockets.upstream_proxy_protocol");
      envoy::extensions::transport_sockets::proxy_protocol::v3::ProxyProtocolUpstreamTransport
          config;
      config.mutable_config()->set_version(envoy::config::core::v3::ProxyProtocolConfig::V2);
      auto* inner = config.mutable_transport_socket();
      inner->set_name("envoy.transport_sockets.raw_buffer");
      ASSERT_TRUE(inner->mutable_typed_config()->PackFrom(
          envoy::extensions::transport_sockets::raw_buffer::v3::RawBuffer()));
      ASSERT_TRUE(socket->mutable_typed_config()->PackFrom(config));
      if (health_checks_) {
        cluster->mutable_common_lb_config()->mutable_healthy_panic_threshold()->set_value(0);
        auto* check = cluster->add_health_checks();
        check->mutable_timeout()->set_seconds(20);
        check->mutable_interval()->set_seconds(1);
        check->mutable_no_traffic_interval()->set_seconds(1);
        check->mutable_healthy_threshold()->set_value(1);
        check->mutable_unhealthy_threshold()->set_value(1);
        check->mutable_reuse_connection()->set_value(true);
        check->mutable_http_health_check()->set_codec_client_type(envoy::type::v3::HTTP1);
        check->mutable_http_health_check()->set_path("/healthcheck");
      }
    });
    BaseIntegrationTest::initialize();
  }

  bool health_checks_{false};
  FakeRawConnectionPtr fake_upstream_connection_;
};

INSTANTIATE_TEST_SUITE_P(
    IpVersionsAndRuntimeGuard, ProxyProtocolFallbackIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()), testing::Bool()));

// Verify the actual HTTP health-check connection carries the endpoint's peering TLVs,
// even though there is no downstream connection to supply PROXY protocol options.
TEST_P(ProxyProtocolFallbackIntegrationTest, V2HealthCheckWithHostTLVs) {
  health_checks_ = true;
  const std::string hostname = "echo.echo.mesh.internal:8080";
  const std::string network = "kind-cluster-2";
  config_helper_.addConfigModifier([&](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    PerHostConfig host_config;
    auto* hostname_tlv = host_config.add_added_tlvs();
    hostname_tlv->set_type(0xd3);
    hostname_tlv->set_value(hostname);
    auto* network_tlv = host_config.add_added_tlvs();
    network_tlv->set_type(0xd4);
    network_tlv->set_value(network);
    auto* metadata = bootstrap.mutable_static_resources()
                         ->mutable_clusters(0)
                         ->mutable_load_assignment()
                         ->mutable_endpoints(0)
                         ->mutable_lb_endpoints(0)
                         ->mutable_metadata();
    ASSERT_TRUE(
        (*metadata->mutable_typed_filter_metadata())["envoy.transport_sockets.proxy_protocol"]
            .PackFrom(host_config));
  });

  FakeRawConnectionPtr health_connection;
  on_server_init_function_ = [&]() {
    ASSERT_TRUE(fake_upstreams_[0]->waitForRawConnection(health_connection));
    std::string observed_data;
    ASSERT_TRUE(health_connection->waitForData(
        FakeRawConnection::waitForInexactMatch("GET /healthcheck HTTP/1.1\r\n"), &observed_data));
    if (std::get<1>(GetParam())) {
      ASSERT_GE(observed_data.size(), 16);
      EXPECT_EQ(std::string("\r\n\r\n\0\r\nQUIT\n", 12), observed_data.substr(0, 12));
      EXPECT_EQ(0x21, static_cast<uint8_t>(observed_data[12])); // PROXY, not LOCAL.
      const bool ipv4 = std::get<0>(GetParam()) == Network::Address::IpVersion::v4;
      EXPECT_EQ(ipv4 ? 0x11 : 0x21, static_cast<uint8_t>(observed_data[13]));
      const size_t address_length = ipv4 ? 12 : 36;
      const std::string expected_tlvs =
          std::string("\xd3\0\x1c", 3) + hostname + std::string("\xd4\0\x0e", 3) + network;
      const size_t payload_length =
          (static_cast<uint8_t>(observed_data[14]) << 8) | static_cast<uint8_t>(observed_data[15]);
      EXPECT_EQ(address_length + expected_tlvs.size(), payload_length);
      EXPECT_EQ(expected_tlvs, observed_data.substr(16 + address_length, expected_tlvs.size()));
      EXPECT_THAT(observed_data.substr(16 + payload_length),
                  testing::StartsWith("GET /healthcheck HTTP/1.1\r\n"));
    } else {
      EXPECT_THAT(observed_data,
                  testing::StartsWith(localHeader() + "GET /healthcheck HTTP/1.1\r\n"));
    }
    ASSERT_TRUE(health_connection->write("HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n"));
  };
  initialize();
  test_server_->waitForCounter("cluster.cluster_0.health_check.success", testing::Ge(1));
  ASSERT_TRUE(health_connection->close());
  ASSERT_TRUE(health_connection->waitForDisconnect());
}

class ProxyProtocolUdsFallbackIntegrationTest : public ProxyProtocolFallbackIntegrationTest {
public:
  void createUpstreams() override {
    const std::string path = TestEnvironment::unixDomainSocketPath("proxy_protocol_health.sock");
    createUpstream(*Network::Utility::resolveUrl(absl::StrCat("unix://", path)), upstreamConfig());
    config_helper_.addConfigModifier([path](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
      auto* address = bootstrap.mutable_static_resources()
                          ->mutable_clusters(0)
                          ->mutable_load_assignment()
                          ->mutable_endpoints(0)
                          ->mutable_lb_endpoints(0)
                          ->mutable_endpoint()
                          ->mutable_address();
      address->clear_socket_address();
      address->mutable_pipe()->set_path(path);
    });
  }
};

INSTANTIATE_TEST_SUITE_P(
    IpVersions, ProxyProtocolUdsFallbackIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()), testing::Bool()));

// A real non-IP upstream address must retain LOCAL even when custom TLVs are configured.
TEST_P(ProxyProtocolUdsFallbackIntegrationTest, UnsupportedUpstreamAddressesUseLocal) {
  health_checks_ = true;
  config_helper_.addConfigModifier([](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
    PerHostConfig host_config;
    auto* tlv = host_config.add_added_tlvs();
    tlv->set_type(0x05);
    tlv->set_value("health-check-id");
    auto* metadata = bootstrap.mutable_static_resources()
                         ->mutable_clusters(0)
                         ->mutable_load_assignment()
                         ->mutable_endpoints(0)
                         ->mutable_lb_endpoints(0)
                         ->mutable_metadata();
    ASSERT_TRUE(
        (*metadata->mutable_typed_filter_metadata())["envoy.transport_sockets.proxy_protocol"]
            .PackFrom(host_config));
  });
  FakeRawConnectionPtr health_connection;
  on_server_init_function_ = [&]() {
    ASSERT_TRUE(fake_upstreams_[0]->waitForRawConnection(health_connection));
    std::string observed_data;
    ASSERT_TRUE(health_connection->waitForData(
        FakeRawConnection::waitForInexactMatch("GET /healthcheck HTTP/1.1\r\n"), &observed_data));
    EXPECT_THAT(observed_data,
                testing::StartsWith(localHeader() + "GET /healthcheck HTTP/1.1\r\n"));
    ASSERT_TRUE(health_connection->write("HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n"));
  };
  initialize();
  test_server_->waitForCounter("cluster.cluster_0.health_check.success", testing::Ge(1));
  ASSERT_TRUE(health_connection->close());
  ASSERT_TRUE(health_connection->waitForDisconnect());
}

} // namespace
} // namespace Envoy
