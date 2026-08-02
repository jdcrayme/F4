// f4-math — Numerical mathematics for flight simulation.
//
// Master include. Pulls in every f4-math header.
//
// Dependencies: NONE. This library is pure numerics with no domain coupling
// (no f4-units, no f4-data, no flight-model concepts). It can be used in any
// C++20 project without dragging in flight-simulation semantics.

#pragma once

#include "f4/math/scalar.hpp"
#include "f4/math/table.hpp"
#include "f4/math/integration.hpp"
#include "f4/math/filters.hpp"
#include "f4/math/vec3.hpp"
#include "f4/math/quat.hpp"
#include "f4/math/solver.hpp"
