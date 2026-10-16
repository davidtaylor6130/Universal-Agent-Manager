# How these docs work

These docs define the expected behaviour of UAM for people and agents changing it. Start with the [map](README.md), read the relevant parent introduction, then the specific feature. Group by product concept, not source directories. A folder earns an introduction when it has meaningful subtopics; a single concept can remain one Markdown file. Aim for one paragraph per concept, keeping any boundary that changes what a user can expect.

## Agreements and implementation

Feature prose states the agreement, not a claim that it has shipped. Current limitations belong in a short implementation note, the provider checklists or the [open decisions and gaps](documentation-review.md). Use **Implemented** for an inspected implementation route, **Partial** for an identified incomplete route, **Not implemented** for an established absence, **Needs verification** when evidence is insufficient, and **Not applicable** only with a reason. No implementation status implies user approval. Outstanding acceptance alone does not make an existing route Partial; use Needs verification when delivery cannot yet be established. Delivery labels Provider, UAM and Both name inspected routes; None records an established absence and Unverified records an unresolved delivery route. Acceptance and ownership remain in the [V5 change log](<../V5 Improvement Change log.md>); retain its pending review and user-test flags.

## Provider completeness

[Providers](providers/providers.md#required-capabilities) owns the required capability list. Every provider introduction must contain one explicit checklist row for every capability, including missing ones; a missing row or file is a documentation defect, never evidence of support or deliberate exclusion. State whether delivery comes from the provider, UAM or both. Prefer a UAM equivalent when the native CLI lacks a feature. A native limitation remains a UAM implementation gap when the required outcome is not delivered. Provider-specific subpages are for meaningful differences, not copies of the common agreement.

## Ownership and links

Most distinct user-facing sections have their own owning file, even when several are grouped under one logical parent. Each behaviour has one owning page. Settings pages explain configuration; interaction pages explain where users act and link to that configuration. UI rules own shared presentation, accessibility and interaction conventions. Architecture stays high level; [engineering reference](reference/README.md) holds useful source details. Historical receipts belong in `.UAM/AINotes/` or the tracked change log, not feature paragraphs. A link is preferable to a second definition.

## Changes and commits

Before changing behaviour, find its owning page and applicable UI rules. Preserve the documented agreement unless the task authorizes changing it; a mismatch is a defect or an explicit unresolved decision, not permission to silently rewrite expectations around the code. Update affected feature pages, capability rows and links in the same commit as the implementation, plus the V5 log. Update architecture only if system relationships change. Review documentation during commit preparation even when the user did not ask for it. If the change has no documentation impact, state that in the change description rather than making a cosmetic edit.

Run `python3 scripts/check_docs.py` before committing documentation or capability changes. This checks local documentation links and required provider rows, not runtime behaviour. Do not mark implementation verified or accepted from a documentation check. Unresolved product questions are collected in [Documentation review](documentation-review.md), with existing evidence and a conservative interim interpretation.
