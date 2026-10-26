Fixed several log and error messages that disclosed secret material. Config protos were printed
with ``DebugString()``, which ignores the ``udpa.annotations.sensitive`` annotation that
``/config_dump`` relies on. Most notably, a failure during server initialization logged the entire
bootstrap at ``critical`` level, disclosing inline TLS private keys, generic secrets and session
ticket keys from ``static_resources.secrets``; it now logs a redacted proto and no longer echoes
the raw config YAML. The same redaction is now applied to the ADS ``ApiConfigSource`` and the
metrics service ``GrpcService`` (both of which can carry gRPC call credentials and a client private
key). The ext_authz client also no longer logs complete ``CheckRequest`` or ``CheckResponse``
messages, which can carry credentials in unannotated runtime fields. Additionally, the api_key_auth
filter no longer embeds credential data in its
duplicate-credential error (which is also returned to the control plane in an xDS NACK), the
jwt_authn filter no longer logs the raw JWT at ``debug``, and the ``injected_credentials`` OAuth2
clients no longer log token endpoint response bodies -- which can carry ``access_token`` and
``refresh_token`` -- in errors or debug logs.
Additional v1.40 diagnostics now omit MCP header and cookie values, payloads and metadata parameters,
ext_proc processing response payloads and request paths in body-size errors.
Protocol versions, method and tool names, configured header names, and provider URIs are retained.
JWT configuration logs use annotation-based redaction. Body-size errors and MCP request mapping
logs include the request ID for correlation with access logs.
