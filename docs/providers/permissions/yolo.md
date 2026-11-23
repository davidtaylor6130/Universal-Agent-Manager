# YOLO

YOLO automatically approves permission requests through the least restrictive behaviour supported by the selected runtime. It is the deliberately dangerous choice and must be identifiable as such. It cannot override OS restrictions, managed policy, a read-only agent or Plan mode, and UAM must not claim it can force a provider to perform a capability the provider does not expose.

**Decided by David, 2026-11-23:** by default YOLO does not approve an action that permanently commits somewhere other than the person's computer: an SVN commit or publish, an `svn copy` to a repository URL, `git push`, creating a remote branch or tag, or merging a pull request. Each needs the person's explicit approval in every mode; only a hidden advanced setting may turn this off. **Implementation note:** not built; under YOLO these are approved like any other request today, and a provider's own full-auto mode can run them without asking UAM ([recorded gap](../../documentation-review.md#approval-for-remote-commits)).

[All permission modes](permissions.md).
