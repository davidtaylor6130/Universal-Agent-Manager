# Chat Defaults

Chat Defaults provides the initial provider-specific choices for new chats, such as model, reasoning, speed tier, permissions, memory and UAM tools where supported. All environments supplies the shared baseline; a selected environment can override supported defaults without changing unrelated computers. An explicit existing-chat choice remains explicit, while a value deliberately set to inherit follows its applicable default. Defaults must not silently rewrite active conversations.

[Environment scope](environment-scope.md) · [Memory inheritance](memory.md).

Model-catalogue visibility and provider-native versus UAM feature preference affect offered choices, not the existence of native capability or permission ceilings. Reviewer defaults, lightweight model presentation and hidden catalogue groups must retain their provider/environment scope. [Provider Overview](provider-overview.md) separately owns runtime availability.
