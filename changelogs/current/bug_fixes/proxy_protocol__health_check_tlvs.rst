Fixed upstream PROXY protocol v2 sockets dropping configured and per-host TLVs on connections without downstream
PROXY protocol information, including active health checks. These connections now send a PROXY header using the
upstream connection addresses when custom TLVs are present and the addresses are IP addresses of the same family.
Otherwise, they retain LOCAL. The fallback can be disabled by setting runtime guard
``envoy.reloadable_features.proxy_protocol_v2_custom_tlvs_without_downstream`` to ``false``.
