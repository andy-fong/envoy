#include <chrono>
#include <memory>
#include <string>

#include "envoy/common/callback.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/extensions/clusters/dns/v3/dns_cluster.pb.h"

#include "source/common/upstream/cluster_factory_impl.h"
#include "source/common/upstream/upstream_impl.h"
#include "source/extensions/clusters/dns/dns_cluster.h"
#include "source/extensions/clusters/dns/on_demand_dns_cluster.h"
#include "source/extensions/load_balancing_policies/cluster_provided/config.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/server/instance.h"
#include "test/mocks/server/server_factory_context.h"
#include "test/mocks/upstream/load_balancer_context.h"
#include "test/test_common/registry.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;
using testing::ReturnRef;

namespace Envoy {
namespace Upstream {
namespace {

class OnDemandDnsClusterTest : public Event::TestUsingSimulatedTime, public testing::Test {
protected:
  OnDemandDnsClusterTest() : api_(Api::createApiForTest(stats_store_, random_)) {
    ON_CALL(server_context_, api()).WillByDefault(ReturnRef(*api_));
  }

  void enableOnDemand() {
    scoped_runtime_.mergeValues(
        {{"envoy.reloadable_features.dns_cluster_on_demand_resolution", "true"}});
  }

  // Creates the cluster through the registered `envoy.cluster.dns` factory, which is the path the
  // cluster manager takes. The factory decides whether the cluster resolves on demand and, if so,
  // returns the thread aware load balancer that drives resolution.
  absl::StatusOr<std::pair<ClusterSharedPtr, ThreadAwareLoadBalancerPtr>>
  createClusterFromV3Yaml(const std::string& yaml) {
    ON_CALL(server_context_, api()).WillByDefault(ReturnRef(*api_));
    envoy::config::cluster::v3::Cluster cluster_config = parseClusterFromV3Yaml(yaml);
    ClusterFactoryContextImpl::LazyCreateDnsResolver resolver_fn = [&]() { return dns_resolver_; };
    return ClusterFactoryImplBase::create(cluster_config, server_context_, resolver_fn, nullptr,
                                          false);
  }

  void expectResolve(Network::DnsLookupFamily dns_lookup_family,
                     const std::string& expected_address) {
    EXPECT_CALL(*dns_resolver_, resolve(expected_address, dns_lookup_family, _))
        .WillOnce(Invoke([&](const std::string&, Network::DnsLookupFamily,
                             Network::DnsResolver::ResolveCb cb) -> Network::ActiveDnsQuery* {
          dns_callback_ = cb;
          return &active_dns_query_;
        }));
  }

  // Initializes the cluster and registers a membership callback. On-demand clusters warm without
  // resolving, so initialization completes immediately.
  void initializeCluster() {
    priority_update_cb_ = cluster_->prioritySet().addPriorityUpdateCb(
        [&](uint32_t, const HostVector&, const HostVector&) {
          membership_updated_.ready();
          return absl::OkStatus();
        });
    cluster_->initialize([&]() {
      initialized_.ready();
      return absl::OkStatus();
    });
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> server_context_;
  Stats::TestUtil::TestStore& stats_store_ = server_context_.store_;
  NiceMock<Random::MockRandomGenerator> random_;
  Api::ApiPtr api_;

  std::shared_ptr<NiceMock<Network::MockDnsResolver>> dns_resolver_{
      new NiceMock<Network::MockDnsResolver>};
  Network::MockActiveDnsQuery active_dns_query_;
  Network::DnsResolver::ResolveCb dns_callback_;
  Event::MockTimer* resolve_timer_;
  ReadyWatcher membership_updated_;
  ReadyWatcher initialized_;
  ClusterSharedPtr cluster_;
  TestScopedRuntime scoped_runtime_;
  Common::CallbackHandlePtr priority_update_cb_;
  Extensions::LoadBalancingPolicies::ClusterProvided::Factory cluster_provided_lb_factory_;
  Registry::InjectFactory<TypedLoadBalancerFactory> cluster_provided_lb_factory_injection_{
      cluster_provided_lb_factory_};
};

// A cluster that does not use cluster-provided load balancing is unaffected by the runtime guard:
// it provides no load balancer and resolves during warming.
TEST_F(OnDemandDnsClusterTest, SkippedWithoutClusterProvidedLbPolicy) {
  enableOnDemand();
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  cluster_type:
    name: envoy.cluster.dns
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_lookup_family: V4_ONLY
      all_addresses_in_single_endpoint: true
  lb_policy: ROUND_ROBIN
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();
  EXPECT_EQ(status_or_cluster->second, nullptr);

  cluster_ = std::move(status_or_cluster->first);
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  initializeCluster();
}

// The runtime guard defaults to off, so a cluster-provided cluster gets no on-demand load balancer.
TEST_F(OnDemandDnsClusterTest, DisabledByDefault) {
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  cluster_type:
    name: envoy.cluster.dns
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_lookup_family: V4_ONLY
      all_addresses_in_single_endpoint: true
  lb_policy: CLUSTER_PROVIDED
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();
  EXPECT_EQ(status_or_cluster->second, nullptr);
}

// Cluster-provided load balancing can also be selected through the typed load_balancing_policy
// field rather than the legacy lb_policy enum.
TEST_F(OnDemandDnsClusterTest, AcceptsTypedClusterProvidedLoadBalancingPolicy) {
  enableOnDemand();
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  cluster_type:
    name: envoy.cluster.dns
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_lookup_family: V4_ONLY
      all_addresses_in_single_endpoint: true
  load_balancing_policy:
    policies:
    - typed_extension_config:
        name: envoy.load_balancing_policies.cluster_provided
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.load_balancing_policies.cluster_provided.v3.ClusterProvided
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();
  EXPECT_NE(status_or_cluster->second, nullptr);
}

// Logical DNS semantics: warming does not resolve, the first host selection triggers resolution and
// is completed asynchronously once the response arrives.
TEST_F(OnDemandDnsClusterTest, SkipsWarmupResolveAndResolvesOnFirstUse) {
  enableOnDemand();
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  cluster_type:
    name: envoy.cluster.dns
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_refresh_rate: 5s
      dns_lookup_family: V4_ONLY
      all_addresses_in_single_endpoint: true
  lb_policy: CLUSTER_PROVIDED
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  EXPECT_CALL(*dns_resolver_, resolve(_, _, _)).Times(0);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();

  cluster_ = std::move(status_or_cluster->first);
  ASSERT_NE(status_or_cluster->second, nullptr);

  EXPECT_CALL(initialized_, ready());
  initializeCluster();

  testing::Mock::VerifyAndClearExpectations(dns_resolver_.get());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");

  auto lb = status_or_cluster->second->factory()->create({cluster_->prioritySet(), nullptr});
  NiceMock<MockLoadBalancerContext> context;
  auto host_selection = lb->chooseHost(&context);
  EXPECT_EQ(host_selection.host, nullptr);
  ASSERT_NE(host_selection.cancelable, nullptr);

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(5000), _));
  EXPECT_CALL(context, onAsyncHostSelection(_, _))
      .WillOnce(Invoke([](HostConstSharedPtr&& host, std::string&& details) {
        ASSERT_NE(host, nullptr);
        EXPECT_EQ("foo.bar.com", host->hostname());
        EXPECT_EQ("resolved", details);
      }));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "resolved",
                TestUtility::makeDnsResponse({"127.0.0.1"}));

  // Once resolved, host selection is synchronous again.
  auto resolved_selection = lb->chooseHost(&context);
  ASSERT_NE(resolved_selection.host, nullptr);
  EXPECT_EQ("foo.bar.com", resolved_selection.host->hostname());
  EXPECT_EQ(resolved_selection.cancelable, nullptr);
}

// Strict DNS semantics: every returned address becomes its own endpoint.
TEST_F(OnDemandDnsClusterTest, StrictDnsResolvesMultipleAddressesOnFirstUse) {
  enableOnDemand();
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  cluster_type:
    name: envoy.cluster.dns
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_refresh_rate: 5s
      dns_lookup_family: V4_ONLY
  lb_policy: CLUSTER_PROVIDED
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  EXPECT_CALL(*dns_resolver_, resolve(_, _, _)).Times(0);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();

  cluster_ = std::move(status_or_cluster->first);
  ASSERT_NE(status_or_cluster->second, nullptr);

  EXPECT_CALL(initialized_, ready());
  initializeCluster();

  testing::Mock::VerifyAndClearExpectations(dns_resolver_.get());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");

  auto lb = status_or_cluster->second->factory()->create({cluster_->prioritySet(), nullptr});
  NiceMock<MockLoadBalancerContext> context;
  auto host_selection = lb->chooseHost(&context);
  EXPECT_EQ(host_selection.host, nullptr);
  ASSERT_NE(host_selection.cancelable, nullptr);

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(5000), _));
  EXPECT_CALL(context, onAsyncHostSelection(_, _))
      .WillOnce(Invoke([](HostConstSharedPtr&& host, std::string&& details) {
        ASSERT_NE(host, nullptr);
        EXPECT_EQ("foo.bar.com", host->hostname());
        EXPECT_EQ("resolved", details);
      }));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "resolved",
                TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));

  EXPECT_EQ(2UL, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts().size());
  auto resolved_selection = lb->chooseHost(&context);
  ASSERT_NE(resolved_selection.host, nullptr);
  EXPECT_EQ("foo.bar.com", resolved_selection.host->hostname());
}

// The legacy `type: STRICT_DNS` form is handled by LegacyDnsClusterFactory, which supports
// on-demand resolution too, so callers need not migrate to the verbose `cluster_type` form.
TEST_F(OnDemandDnsClusterTest, LegacyStrictDnsClusterTypeResolvesOnDemand) {
  enableOnDemand();
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  type: STRICT_DNS
  dns_refresh_rate: 5s
  dns_lookup_family: V4_ONLY
  lb_policy: CLUSTER_PROVIDED
  load_assignment:
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
  )EOF";

  resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
  EXPECT_CALL(*dns_resolver_, resolve(_, _, _)).Times(0);
  auto status_or_cluster = createClusterFromV3Yaml(yaml);
  ASSERT_TRUE(status_or_cluster.ok()) << status_or_cluster.status();

  cluster_ = std::move(status_or_cluster->first);
  ASSERT_NE(status_or_cluster->second, nullptr);

  EXPECT_CALL(initialized_, ready());
  initializeCluster();

  testing::Mock::VerifyAndClearExpectations(dns_resolver_.get());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");

  auto lb = status_or_cluster->second->factory()->create({cluster_->prioritySet(), nullptr});
  NiceMock<MockLoadBalancerContext> context;
  auto host_selection = lb->chooseHost(&context);
  EXPECT_EQ(host_selection.host, nullptr);
  ASSERT_NE(host_selection.cancelable, nullptr);

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(5000), _));
  EXPECT_CALL(context, onAsyncHostSelection(_, _))
      .WillOnce(Invoke([](HostConstSharedPtr&& host, std::string&& details) {
        ASSERT_NE(host, nullptr);
        EXPECT_EQ("foo.bar.com", host->hostname());
        EXPECT_EQ("resolved", details);
      }));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "resolved",
                TestUtility::makeDnsResponse({"127.0.0.1"}));

  auto resolved_selection = lb->chooseHost(&context);
  ASSERT_NE(resolved_selection.host, nullptr);
  EXPECT_EQ("foo.bar.com", resolved_selection.host->hostname());
}

} // namespace
} // namespace Upstream
} // namespace Envoy
