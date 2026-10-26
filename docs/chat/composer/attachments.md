# Composer attachments

Users can attach files, images, folders and skills as visible context chips, inspect their identity and remove them before sending. The message must retain its selected context through send, save, retry and editing; silently omitted resources are a failure. Supported file, image or folder input can be sent without text; malformed attachment input rejects the whole request under the [shared admission rule](../../providers/attachments.md#input-admission). Provider consumption and SSH delivery follow the [provider attachment agreement](../../providers/attachments.md), and skills retain their [supporting resources](../../providers/skills.md).

Attachment staging is user-visible work and remains independent of low-priority background file maintenance. Ordinary file bytes bypass optional native image-format probing; recognized images retain the existing bounded decoding, orientation and preview policy, while original attachment bytes remain unchanged. These rules apply to desktop, browser and native-phone callers and preserve the same workspace/SSH ownership checks.

Native iPhone HEIC/HEIF imports show two removable attachments before sending: the byte-exact original as a file and an orientation-corrected JPEG for agents to inspect. The JPEG has bounded pixel dimensions and encoded size; its metadata does not replace the retained original. Both attachments count toward the phone composer’s eight-file/20 MB limit and are added together or not at all. Photo-library and file imports use the same preparation, and malformed or unsupported sequences produce a recoverable error rather than silently dropping the original. Other supported image bytes remain unchanged.

Phone draft text edits save independently of retained attachment bytes. A missing or damaged saved attachment must surface a recoverable error and preserve the draft record; it must never silently restore an empty draft or revive a previously acknowledged message.

[Context-chip rules](../../ui/interaction.md).

[File inspection](../../files/inspection.md).
