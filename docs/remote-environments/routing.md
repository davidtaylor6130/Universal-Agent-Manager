# Execution routing

Routing chooses where work executes while retaining its provider, workspace and authored request. Automatic rules must respect availability and configured confirmation, and explicit choices must remain understandable rather than silently moving work after a failure. Changes to host identity or material configuration require revalidation. A disconnected or busy environment must not cause duplicate provider work or an unauthorized local fallback.

[Remote settings](../settings/remote-hosts.md) · [Execution reference](../reference/architecture/execution-hosts.md).

Automatic routing requires enabled candidate computers, capacity, current provider sign-in/configuration confirmation and valid local-to-remote folder/repository mappings. An explicit user destination is separate from the automatic candidate policy; changed connections/configurations invalidate old confirmations. No available route must remain an honest blocked/choice state rather than an unauthorized fallback.
