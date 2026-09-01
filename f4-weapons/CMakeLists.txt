cmake_minimum_required(VERSION 3.20)
project(f4-weapons VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Weapons & effects core: missile flyout, gun model, damage, stores")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-weapons depends on:
#   - f4-geo       (WorldPosition — strong-typed sim-local frame; PUBLIC,
#                   used in public headers)
#   - f4-math      (Vec3 for velocities/guidance; PUBLIC, used in headers)
#   - f4-entities  (components: WeaponStoreComponent, MissileSimComponent,
#                   DamageStateComponent consumer; PUBLIC, used in headers)
#   - f4-messaging (MessageBus for combat event messages; PUBLIC —
#                   messages.hpp + gun.hpp expose it)
#
# Deliberately NOT a dependency: f4-flight-model (a missile is a 3-DOF
# point mass, not a 6-DOF aircraft — see COMBAT_CHAIN_PLAN.md §2 M1),
# f4-ai (tactics consume weapons, never the reverse).
# ---------------------------------------------------------------------------
add_library(f4-weapons STATIC
    src/weapon_class_table.cpp
    src/damage.cpp
    src/missile.cpp
    src/weapon_store.cpp
    src/missile_battery.cpp
    src/gun.cpp
    src/gun_component.cpp
)
target_link_libraries(f4-weapons PUBLIC f4_warnings)

target_include_directories(f4-weapons PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-weapons PUBLIC
    f4-geo
    f4-math
    f4-entities
    f4-messaging
)

# Tests
option(F4_WEAPONS_BUILD_TESTS "Build f4-weapons unit tests" ON)
if(F4_WEAPONS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
