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
