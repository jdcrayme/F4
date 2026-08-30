// f4-weapons/src/weapon_store.cpp — see weapon_store.hpp.

#include <f4/weapons/weapon_store.hpp>

#include <algorithm>

namespace f4::weapons {

std::size_t WeaponStoreComponent::add_station(std::uint32_t weapon_handle, int rounds,
                                              std::string label) {
    WeaponStation s;
    s.weapon_handle = weapon_handle;
    s.rounds = std::max(0, rounds);
    s.initial_rounds = s.rounds;
    s.label = std::move(label);
    stations_.push_back(std::move(s));
    return stations_.size() - 1;
}

WeaponStoreComponent WeaponStoreComponent::standard_fighter(const WeaponClassTable& table) {
    WeaponStoreComponent store;
    const auto gun    = table.find_by_name("M61A1");
    const auto sidewinder = table.find_by_name("AIM-9M");
    const auto amraam = table.find_by_name("AIM-120C");
    if (gun != kInvalidWeapon) {
        store.add_station(gun, 511, "gun");
    }
    if (amraam != kInvalidWeapon) {
        store.add_station(amraam, 2, "station 1");
        store.add_station(amraam, 2, "station 2");
        store.add_station(amraam, 2, "station 7");
        store.add_station(amraam, 2, "station 8");
    }
    if (sidewinder != kInvalidWeapon) {
        store.add_station(sidewinder, 1, "wingtip left");
        store.add_station(sidewinder, 1, "wingtip right");
    }
    return store;
}

const WeaponStation* WeaponStoreComponent::station(std::size_t index) const noexcept {
    return index < stations_.size() ? &stations_[index] : nullptr;
}

int WeaponStoreComponent::count_for(std::uint32_t weapon_handle) const noexcept {
    int total = 0;
    for (const auto& s : stations_) {
        if (s.weapon_handle == weapon_handle) {
            total += s.rounds;
        }
    }
    return total;
}

std::size_t WeaponStoreComponent::find_with_category(const WeaponClassTable& table,
                                                     WeaponCategory category) const noexcept {
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        const auto* rec = table.get(stations_[i].weapon_handle);
        if (rec != nullptr && rec->category == category && stations_[i].rounds > 0) {
            return i;
        }
    }
    return npos;
}

std::size_t WeaponStoreComponent::select(std::size_t index) {
    if (stations_.empty()) {
        selected_ = 0;
        return 0;
    }
    selected_ = std::min(index, stations_.size() - 1);
    return selected_;
}

std::size_t WeaponStoreComponent::select_next_loaded() {
    if (stations_.empty()) {
        return npos;
    }
    for (std::size_t step = 0; step < stations_.size(); ++step) {
        const std::size_t idx = (selected_ + 1 + step) % stations_.size();
        if (stations_[idx].rounds > 0) {
            selected_ = idx;
            return idx;
        }
    }
    return npos;
}

int WeaponStoreComponent::expend(std::size_t index, int n) {
    if (index >= stations_.size() || n <= 0) {
        return 0;
    }
    auto& s = stations_[index];
    const int taken = std::min(n, s.rounds);
    s.rounds -= taken;
    return taken;
}

bool WeaponStoreComponent::can_fire(std::size_t index) const noexcept {
    if (index >= stations_.size()) {
        return false;
    }
    return stations_[index].weapon_handle != kInvalidWeapon && stations_[index].rounds > 0;
}

} // namespace f4::weapons
