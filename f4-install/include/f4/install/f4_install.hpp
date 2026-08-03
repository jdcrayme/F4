// f4-install/include/f4/install/f4_install.hpp
//
// Umbrella header for f4-install — the engine-agnostic Falcon 4.0 install
// layout locator. Include this to get the full public API.
//
// f4-install owns the layout knowledge of a Falcon 4.0 / FreeFalcon
// installation: where FALCON4.ct lives, how to enumerate theaters under
// terrdata/, how to find .cam saves under campaign/, and how to resolve
// any well-known file by name. No other library needs to duplicate this
// knowledge — they call into f4-install instead.
//
// Dependencies: standard library only. This keeps f4-install portable
// (it must work on Windows, macOS, and Linux without modification) and
// keeps it out of the dependency cycle between f4-world-convert and
// f4-world-viewer.

#pragma once

#include <f4/install/campaign.hpp>
#include <f4/install/installation.hpp>
#include <f4/install/theater.hpp>
