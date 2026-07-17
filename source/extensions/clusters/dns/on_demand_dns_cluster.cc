#include "source/extensions/clusters/dns/on_demand_dns_cluster.h"

#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/logger.h"
#include "source/common/runtime/runtime_features.h"
#include "source/extensions/load_balancing_policies/round_robin/round_robin_lb.h"

namespace Envoy {
namespace Upstream {

namespace {

bool usesClusterProvidedLoadBalancing(const envoy::config::cluster::v3::Cluster& cluster) {
  if (cluster.has_load_balancing_policy()) {
    const auto& policies = cluster.load_balancing_policy().policies();
    if (policies.empty()) {
      return false;
    }
    return policies[0].typed_extension_config().name() ==
           "envoy.load_balancing_policies.cluster_provided";
  }

  return cluster.lb_policy() == envoy::config::cluster::v3::Cluster::CLUSTER_PROVIDED;
}

} // namespace

bool onDemandDnsEnabled(const envoy::config::cluster::v3::Cluster& cluster) {
  const bool enabled = Runtime::runtimeFeatureEnabled(
                           "envoy.reloadable_features.dns_cluster_on_demand_resolution") &&
                       usesClusterProvidedLoadBalancing(cluster);
  if (enabled) {
    ENVOY_LOG_TO_LOGGER(Logger::Registry::getLog(Logger::Id::upstream), debug,
                        "On-demand DNS resolution is enabled for cluster: {}", cluster.name());
  }
  return enabled;
}

absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
createOnDemandDnsCluster(const envoy::config::cluster::v3::Cluster& cluster,
                         const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                         ClusterFactoryContext& context,
                         Network::DnsResolverSharedPtr dns_resolver) {
  auto cluster_or_error =
      OnDemandDnsClusterImpl::create(cluster, dns_cluster, context, std::move(dns_resolver));
  RETURN_IF_NOT_OK(cluster_or_error.status());

  auto lb = std::make_unique<OnDemandDnsClusterImpl::ThreadAwareLoadBalancer>(*cluster_or_error);
  return std::make_pair(ClusterImplBaseSharedPtr(std::move(*cluster_or_error)), std::move(lb));
}

/**
 * Per-worker state tracking the host selections that are waiting on resolution to complete.
 */
class OnDemandDnsClusterImpl::ThreadLocalState : public ThreadLocal::ThreadLocalObject {
public:
  ~ThreadLocalState() override;
  void add(OnDemandHostSelectionHandle& handle) { pending_handles_.insert(&handle); }
  void remove(OnDemandHostSelectionHandle& handle) { pending_handles_.erase(&handle); }
  void onResolveComplete(const std::string& details);

private:
  absl::flat_hash_set<OnDemandHostSelectionHandle*> pending_handles_;
};

absl::StatusOr<std::shared_ptr<OnDemandDnsClusterImpl>>
OnDemandDnsClusterImpl::create(const envoy::config::cluster::v3::Cluster& cluster,
                               const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                               ClusterFactoryContext& context,
                               Network::DnsResolverSharedPtr dns_resolver) {
  absl::Status creation_status = absl::OkStatus();
  auto ret = std::shared_ptr<OnDemandDnsClusterImpl>(new OnDemandDnsClusterImpl(
      cluster, dns_cluster, context, std::move(dns_resolver), creation_status));

  RETURN_IF_NOT_OK(creation_status);
  return ret;
}

OnDemandDnsClusterImpl::OnDemandDnsClusterImpl(
    const envoy::config::cluster::v3::Cluster& cluster,
    const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
    ClusterFactoryContext& context, Network::DnsResolverSharedPtr dns_resolver,
    absl::Status& creation_status)
    : DnsClusterImpl(cluster, dns_cluster, context, std::move(dns_resolver), creation_status),
      main_thread_dispatcher_(context.serverFactoryContext().mainThreadDispatcher()),
      time_source_(context.serverFactoryContext().timeSource()),
      tls_slot_(context.serverFactoryContext().threadLocal()) {
  // The base constructor reports configuration errors through `creation_status`; there is no
  // cluster to resolve for if it failed.
  if (!creation_status.ok()) {
    return;
  }
  tls_slot_.set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalState>(); });
}

void OnDemandDnsClusterImpl::startPreInit() {
  // Nothing is resolved during warming; the cluster is immediately considered initialized and
  // resolution starts when a host is first requested.
  onPreInitComplete();
}

class OnDemandDnsClusterImpl::LoadBalancer : public Upstream::LoadBalancer {
public:
  LoadBalancer(std::weak_ptr<OnDemandDnsClusterImpl> cluster, LoadBalancerParams params)
      : cluster_(std::move(cluster)) {
    auto locked_cluster = cluster_.lock();
    if (locked_cluster) {
      envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin config;
      lb_ = std::make_unique<RoundRobinLoadBalancer>(
          params.priority_set, params.local_priority_set, locked_cluster->info()->lbStats(),
          locked_cluster->runtime_, locked_cluster->random_,
          PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(locked_cluster->info()->lbConfig(),
                                                         healthy_panic_threshold, 100, 50),
          config, locked_cluster->time_source_);
    }
  }
  ~LoadBalancer() override;

  // Upstream::LoadBalancer
  HostSelectionResponse chooseHost(LoadBalancerContext* context) override;
  HostConstSharedPtr peekAnotherHost(LoadBalancerContext* context) override {
    return chooseResolvedHost(context).host;
  }
  OptRef<Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override {
    return std::nullopt;
  }
  std::optional<SelectedPoolAndConnection>
  selectExistingConnection(LoadBalancerContext*, const Host&, std::vector<uint8_t>&) override {
    return std::nullopt;
  }

  HostSelectionResponse chooseResolvedHost(LoadBalancerContext*);
  void add(OnDemandHostSelectionHandle& handle) { pending_host_selection_handles_.insert(&handle); }
  void remove(OnDemandHostSelectionHandle& handle) {
    pending_host_selection_handles_.erase(&handle);
  }

private:
  std::weak_ptr<OnDemandDnsClusterImpl> cluster_;
  LoadBalancerPtr lb_;
  absl::flat_hash_set<OnDemandHostSelectionHandle*> pending_host_selection_handles_;
};

/**
 * A host selection that is waiting for on-demand resolution to complete. It is registered with
 * both the worker's thread local state and the load balancer that created it, so that it is
 * notified whichever of the three outlives the others.
 */
class OnDemandDnsClusterImpl::OnDemandHostSelectionHandle : public AsyncHostSelectionHandle {
public:
  OnDemandHostSelectionHandle(LoadBalancerContext& context, LoadBalancer& load_balancer,
                              ThreadLocalState& thread_local_state)
      : context_(context), load_balancer_(load_balancer), thread_local_state_(thread_local_state) {
    thread_local_state_.add(*this);
    load_balancer_.add(*this);
  }
  ~OnDemandHostSelectionHandle() override { cancel(); }

  // Upstream::AsyncHostSelectionHandle
  void cancel() override {
    if (registered_) {
      thread_local_state_.remove(*this);
      load_balancer_.remove(*this);
      registered_ = false;
    }
  }

  void onResolveComplete(const std::string& details) {
    if (!registered_) {
      return;
    }
    thread_local_state_.remove(*this);
    load_balancer_.remove(*this);
    registered_ = false;
    auto host_selection = load_balancer_.chooseResolvedHost(&context_);
    context_.onAsyncHostSelection(std::move(host_selection.host), host_selection.details.empty()
                                                                      ? std::string(details)
                                                                      : host_selection.details);
  }

  void onLoadBalancerDestroyed() {
    if (!registered_) {
      return;
    }
    thread_local_state_.remove(*this);
    load_balancer_.remove(*this);
    registered_ = false;
    context_.onAsyncHostSelection(nullptr, "load_balancer_destroyed");
  }

  void onThreadLocalStateDestroyed() {
    if (!registered_) {
      return;
    }
    load_balancer_.remove(*this);
    registered_ = false;
  }

private:
  LoadBalancerContext& context_;
  LoadBalancer& load_balancer_;
  ThreadLocalState& thread_local_state_;
  bool registered_{true};
};

OnDemandDnsClusterImpl::LoadBalancer::~LoadBalancer() {
  while (!pending_host_selection_handles_.empty()) {
    (*pending_host_selection_handles_.begin())->onLoadBalancerDestroyed();
  }
}

HostSelectionResponse
OnDemandDnsClusterImpl::LoadBalancer::chooseHost(LoadBalancerContext* context) {
  auto host_selection = chooseResolvedHost(context);
  if (host_selection.host != nullptr || context == nullptr) {
    return host_selection;
  }

  auto cluster = cluster_.lock();
  if (!cluster) {
    return {nullptr, "on_demand_dns_cluster_destroyed"};
  }

  ThreadLocalState& thread_local_state = *cluster->tls_slot_;
  auto handle = std::make_unique<OnDemandHostSelectionHandle>(*context, *this, thread_local_state);
  cluster->main_thread_dispatcher_.post([cluster]() { cluster->startOnDemandResolve(); });
  return {nullptr, std::move(handle)};
}

HostSelectionResponse
OnDemandDnsClusterImpl::LoadBalancer::chooseResolvedHost(LoadBalancerContext* context) {
  if (lb_ == nullptr) {
    return {nullptr, "on_demand_dns_cluster_destroyed"};
  }
  auto host_selection = lb_->chooseHost(context);
  if (host_selection.host == nullptr && host_selection.details.empty()) {
    host_selection.details = "on_demand_dns_not_resolved";
  }
  return host_selection;
}

OnDemandDnsClusterImpl::ThreadLocalState::~ThreadLocalState() {
  while (!pending_handles_.empty()) {
    (*pending_handles_.begin())->onThreadLocalStateDestroyed();
  }
}

void OnDemandDnsClusterImpl::ThreadLocalState::onResolveComplete(const std::string& details) {
  std::vector<OnDemandHostSelectionHandle*> handles(pending_handles_.begin(),
                                                    pending_handles_.end());
  for (auto* handle : handles) {
    handle->onResolveComplete(details);
  }
}

namespace {

class OnDemandDnsLoadBalancerFactory : public LoadBalancerFactory {
public:
  OnDemandDnsLoadBalancerFactory(std::weak_ptr<OnDemandDnsClusterImpl> cluster)
      : cluster_(std::move(cluster)) {}

  // Upstream::LoadBalancerFactory
  LoadBalancerPtr create(LoadBalancerParams params) override {
    return std::make_unique<OnDemandDnsClusterImpl::LoadBalancer>(cluster_, params);
  }
  // The load balancer resolves on demand and reads the priority set directly, so it does not need
  // to be recreated when hosts change.
  bool recreateOnHostChange() const override { return false; }

private:
  std::weak_ptr<OnDemandDnsClusterImpl> cluster_;
};

} // namespace

LoadBalancerFactorySharedPtr OnDemandDnsClusterImpl::ThreadAwareLoadBalancer::factory() {
  return std::make_shared<OnDemandDnsLoadBalancerFactory>(cluster_);
}

void OnDemandDnsClusterImpl::startOnDemandResolve() {
  ASSERT(main_thread_dispatcher_.isThreadSafe());
  if (resolve_in_progress_) {
    return;
  }
  if (resolve_targets_.empty()) {
    notifyPendingOnDemandHostSelections("on_demand_dns_no_resolve_targets");
    return;
  }
  resolve_in_progress_ = true;
  pending_resolve_targets_ = resolve_targets_.size();
  resolve_details_ = "on_demand_dns_not_resolved";
  for (const ResolveTargetPtr& target : resolve_targets_) {
    target->startResolve();
  }
}

void OnDemandDnsClusterImpl::onResolveTargetComplete(absl::string_view details) {
  if (!resolve_in_progress_) {
    return;
  }
  if (!details.empty()) {
    resolve_details_ = std::string(details);
  }
  ASSERT(pending_resolve_targets_ > 0);
  --pending_resolve_targets_;
  if (pending_resolve_targets_ > 0) {
    return;
  }
  resolve_in_progress_ = false;
  notifyPendingOnDemandHostSelections(std::move(resolve_details_));
}

void OnDemandDnsClusterImpl::notifyPendingOnDemandHostSelections(std::string details) {
  tls_slot_.runOnAllThreads([details = std::move(details)](OptRef<ThreadLocalState> local_state) {
    if (!local_state.has_value()) {
      return;
    }
    local_state->onResolveComplete(details);
  });
}

} // namespace Upstream
} // namespace Envoy
