# Phone browser sections

The published browser has Activity, Chats and connection Settings, with its own navigation, project/search filters and refresh state. Activity groups running/needs-input and recent output; opening a chat retains provider/workspace identity, supports the permitted Chat/CLI views and exposes only companion-authorized actions. Connection uses the access token from the main device; browser-local sign-out removes that browser's saved token rather than stopping desktop work. New chat, pinning, sending and terminal input remain real controller actions with ordinary ownership/permission rules. A disconnected or stale page must report its state and must not replay input automatically. Browser storage/home-screen presentation does not turn this into a separately installed native phone app.

[Phone access](phone-access.md) · [Phone settings](settings/phone-access.md) · [Navigation](navigation/navigation.md).
