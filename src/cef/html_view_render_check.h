#pragma once

#include "common/chat/html_view_render_check.h"

#include <memory>

namespace uam::cef
{
	/// <summary>Checker that renders each page once in a hidden windowless browser. Create after CefSettings enables windowless rendering.</summary>
	std::shared_ptr<uam::html_view::RenderChecker> CreateHtmlViewRenderChecker();
}
