// UAM — macOS CEF helper entry point (renderer, GPU and utility processes).
//
// The helper is a separate executable from the main app: it enters the Chromium sandbox before
// anything else runs, then loads the CEF framework at runtime instead of linking it, as the macOS
// sandbox requires. Keep it free of application and platform services.

#include "cef/uam_cef_renderer_app.h"

#include "include/cef_sandbox_mac.h"
#include "include/wrapper/cef_library_loader.h"

int main(int argc, char* argv[])
{
	CefScopedSandboxContext sandbox_context;
	if (!sandbox_context.Initialize(argc, argv)) return 1;

	CefScopedLibraryLoader library_loader;
	if (!library_loader.LoadInHelper()) return 1;

	CefMainArgs main_args(argc, argv);
	return CefExecuteProcess(main_args, new UamCefRendererApp(), nullptr);
}
