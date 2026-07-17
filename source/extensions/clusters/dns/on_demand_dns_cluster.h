#pragma once

// On-demand DNS resolution for the DNS cluster extension.
//
// A cluster using on-demand resolution does not resolve DNS while warming. It resolves on the
// first host selection instead, which keeps DNS traffic proportional to actual use for clusters
// that are pushed to many Envoy instances but rarely used.
//
// Everything specific to on-demand resolution lives in this file and its implementation, so that
// `dns_cluster.{h,cc}` carry only the small hooks needed to reach it.

#include "envoy/thread_local/thread_local.h"

#include "source/extensions/clusters/dns/dns_cluster.h"

namespace Envoy {
namespace Upstream {

/**
 * Whether on-demand DNS resolution should be used for `cluster`.
 *
 * On-demand resolution is gated on the `envoy.reloadable_features.dns_cluster_on_demand_resolution`
 * runtime guard, and is only used for clusters that use cluster-provided load balancing, since the
 * cluster supplies the load balancer that drives resolution. Clusters configured with any other
 * load balancing policy keep resolving during cluster warming.
 */
bool onDemandDnsEnabled(const envoy::config::cluster::v3::Cluster& cluster);

/**
 * Creates an on-demand DNS cluster together with the thread aware load balancer that drives its
 * resolution. Both are returned, in the form the cluster factory hands back to the cluster manager.
 */
absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
createOnDemandDnsCluster(const envoy::config::cluster::v3::Cluster& cluster,
                         const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                         ClusterFactoryContext& context,
                         Network::DnsResolverSharedPtr dns_resolver);

/**
 * A DNS cluster that defers resolution until a host is first requested.
 *
 * Host selection goes through the cluster-provided load balancer below. When no host is resolved
 * yet, the load balancer returns an AsyncHostSelectionHandle and asks the cluster to resolve; the
 * pending selections are completed once resolution finishes. Callers that only support synchronous
 * host selection see the cluster as having no healthy hosts.
 */
class OnDemandDnsClusterImpl : public DnsClusterImpl {
public:
  // Held via shared_ptr because the thread aware load balancer, and the per-worker load balancers
  // it creates, keep weak references back to the cluster.
  static absl::StatusOr<std::shared_ptr<OnDemandDnsClusterImpl>>
  create(const envoy::config::cluster::v3::Cluster& cluster,
         const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
         ClusterFactoryContext& context, Network::DnsResolverSharedPtr dns_resolver);

  class LoadBalancer;

  class ThreadAwareLoadBalancer : public Upstream::ThreadAwareLoadBalancer {
  public:
    ThreadAwareLoadBalancer(std::weak_ptr<OnDemandDnsClusterImpl> cluster)
        : cluster_(std::move(cluster)) {}

    // Upstream::ThreadAwareLoadBalancer
    Upstream::LoadBalancerFactorySharedPtr factory() override;
    absl::Status initialize() override { return absl::OkStatus(); }

  private:
    std::weak_ptr<OnDemandDnsClusterImpl> cluster_;
  };

protected:
  OnDemandDnsClusterImpl(const envoy::config::cluster::v3::Cluster& cluster,
                         const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                         ClusterFactoryContext& context, Network::DnsResolverSharedPtr dns_resolver,
                         absl::Status& creation_status);

  // Upstream::ClusterImplBase
  void startPreInit() override;

  // Upstream::DnsClusterImpl
  void onResolveTargetComplete(absl::string_view details) override;

private:
  class ThreadLocalState;
  class OnDemandHostSelectionHandle;

  void startOnDemandResolve();
  void notifyPendingOnDemandHostSelections(std::string details);

  Event::Dispatcher& main_thread_dispatcher_;
  TimeSource& time_source_;
  ThreadLocal::TypedSlot<ThreadLocalState> tls_slot_;
  bool resolve_in_progress_{};
  uint64_t pending_resolve_targets_{};
  std::string resolve_details_;
};

} // namespace Upstream
} // namespace Envoy
