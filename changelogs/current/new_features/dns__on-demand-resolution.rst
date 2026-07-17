Added support for on-demand DNS resolution in the DNS cluster extension for clusters configured
with cluster-provided load balancing. This behavior is guarded by
``envoy.reloadable_features.dns_cluster_on_demand_resolution``, which defaults to false.
