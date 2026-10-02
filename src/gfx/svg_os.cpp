// renderSvg: gfx's own renderer on every OS (see CMakeLists for why the
// Direct2D / NSImage paths were dropped).
#include "gfx/internal.h"

namespace gfx {

int g_svgBackend = 0;

bool renderSvg(std::string_view svg, int width, int height, Bitmap *out) {
    return renderSvgOwn(svg, width, height, out);
}

} // namespace gfx
