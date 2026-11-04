# Chat Defaults

Chat Defaults provides the initial provider-specific choices for new chats, such as model, reasoning, speed tier, permissions, memory and UAM tools where supported. All environments supplies the shared baseline; a selected environment can override supported defaults without changing unrelated computers. An explicit existing-chat choice remains explicit, while a value deliberately set to inherit follows its applicable default. Defaults must not silently rewrite active conversations.

For inherited memory, select the provider record for the chat’s execution environment when present, otherwise the All environments provider record. A concrete explicit chat choice wins; a selected provider record set to Default follows the concrete app memory default rather than looking up another provider record. Host-only Off must block future recall and extraction under that inherited policy; current resolver verification remains pending.

[Environment scope](environment-scope.md) · [Memory inheritance](memory.md).

Model-catalogue visibility and provider-native versus UAM feature preference affect offered choices, not the existence of native capability or permission ceilings. Reviewer defaults, lightweight model presentation and hidden catalogue groups must retain their provider/environment scope. [Provider Overview](provider-overview.md) separately owns runtime availability.

[Approval-mode and reviewer defaults](permission-defaults.md) are independent of OS permission grants.
