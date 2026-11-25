# YOLO

YOLO automatically approves permission requests through the least restrictive behaviour supported by the selected runtime. It is the deliberately dangerous choice and must be identifiable as such. It cannot override OS restrictions, managed policy, a read-only agent or Plan mode, and UAM must not claim it can force a provider to perform a capability the provider does not expose.

By default YOLO does not approve an action that permanently commits somewhere other than the person's computer, such as `git push` or an SVN commit; [remote commits](permissions.md#remote-commits) need explicit approval in every mode (decided 2026-11-23, not built).

[All permission modes](permissions.md).

Even under YOLO, pushes, merges and other [remote commits always ask](rules.md#remote-commits-always-ask) for explicit approval, and the user's own [ask and deny rules](rules.md) still apply.
