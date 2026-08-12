// f4-world-viewer/src/symbols.hpp
//
// Thin wrapper — delegates to f4::renderer::symbols.hpp and provides
// backward-compatibility aliases in the f4::viewer namespace so that
// existing code (canvas.cpp, imgui_panels.cpp) continues to compile
// without changes.
//
// The symbol implementation (symbol_for_objective_type, symbol_for_unit,
// draw_symbol_imgui, draw_symbol) now lives in the f4-renderer library.
// This header exists solely to avoid a mass-rename of every call site.

#pragma once

#include <f4/renderer/symbols.hpp>

// Backward compatibility — bring f4::renderer symbol types and functions
// into the f4::viewer namespace so existing code can use unqualified names.
namespace f4::viewer {
    // Type aliases (these work with `using =` syntax)
    using SymbolKind = f4::renderer::SymbolKind;
    using RlColor = f4::renderer::RlColor;

    // Function imports (using-declarations, not aliases)
    using f4::renderer::symbol_for_objective_type;
    using f4::renderer::symbol_for_unit;
    using f4::renderer::draw_symbol_imgui;
    using f4::renderer::draw_symbol;
}
