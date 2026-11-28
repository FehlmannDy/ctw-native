// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kryo.to
// See LICENSE in the repository root.
// Exercise garage loading and the cGarage door state machine with synthetic garages.dat, never game lookup data.
#include "garages.h"
#include "world/collision.h"
#include "os/wad.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {
int failures;
void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; } }
using Bytes = std::vector<uint8_t>;
template<class T> void append(Bytes& b, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v);
}
void save(const std::filesystem::path& path, const Bytes& b) {
    std::ofstream f(path, std::ios::binary); f.write((const char*)b.data(), b.size());
}
// One 16-byte garages.dat record: i32 x, y, z; i16 heading; u16 doorProp (cGarageManager::AddOne).
void record(Bytes& d, int32_t x, int32_t y, int32_t z, int16_t heading, uint16_t doorProp) {
    append(d, x); append(d, y); append(d, z); append(d, heading); append(d, doorProp);
}

struct Fixture {
    std::filesystem::path dir;
    std::map<std::string, Bytes> files;
    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("ctw-garages-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        Bytes d;
        // Garage 0: low ground, prop 0x1234. Garage 1: high ground, prop 0x5678 (doorZ sign-extension wraps there).
        record(d, 0x10000, 0x20000, 0x2000, 0, 0x1234);
        record(d, 0x100000, 0, 0x7000, 0, 0x5678);
        files["garages.dat"] = d;
        write();
    }
    ~Fixture() { std::filesystem::remove_all(dir); }
    void write() {
        struct Entry { uint32_t hash, offset, size; };
        Bytes wad; std::vector<Entry> entries;
        for (const auto& [key, value] : files) {
            entries.push_back({Wad::hashName(key.c_str()), (uint32_t)wad.size(), (uint32_t)value.size()});
            wad.insert(wad.end(), value.begin(), value.end());
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
        Bytes toc; for (const Entry& e : entries) append(toc, e);
        save(dir / "rom.wad", wad); save(dir / "rom.toc", toc);
    }
};

Collision::Prop door(uint16_t prop, int32_t x, int32_t y) {
    Collision::Prop p{}; p.prop = prop; p.kind = 1; p.x = x; p.y = y; return p;
}

// A temp data dir holding one arbitrary garages.dat, packed the same way as Fixture, for extra scenes.
struct Scene {
    std::filesystem::path dir;
    explicit Scene(const Bytes& garagesDat) {
        dir = std::filesystem::temp_directory_path() /
              ("ctw-garages-s-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        struct Entry { uint32_t hash, offset, size; };
        Entry e{Wad::hashName("garages.dat"), 0, (uint32_t)garagesDat.size()};
        Bytes toc; append(toc, e);
        save(dir / "rom.wad", garagesDat); save(dir / "rom.toc", toc);
    }
    ~Scene() { std::filesystem::remove_all(dir); }
};

// cGarageManager::AddOne projects the front area 9.6 units (0x999900000 >> 32 = 0x9999) along the heading, halved:
// offset = (((int64)fastsin(h) * 0x999900000) >> 32) >> 1. At heading 0x4000 (90 deg) fastsin is +0x1000 (4096), so the
// x offset is (0x9999 >> 1) = 19660 and the y offset (from fastsin(0x8000) = 0) is 0. Re-derived here so the expected
// values do not depend on the production code.
constexpr int32_t kFront90 = 19660;

// Non-zero headings, a shared door model and the proximity boundary (cGarage front-area math, GiveGarageHandleToThisDoor,
// GetGarageClosestToPoint). All coordinates chosen so every expected value is re-derived from the constants above.
void runHeadings() {
    Bytes d;
    // 0: at origin, 90 deg, model 0x00AA, front (19660, 0).
    record(d, 0, 0, 0, 0x4000, 0x00AA);
    // 1: far east, heading 0, model 0x00AA (same model), front (0x400000, 19660).
    record(d, 0x400000, 0, 0, 0, 0x00AA);
    // 2: far north, heading 0, unique model 0x00BB, front (0, 0x800000 + 19660).
    record(d, 0, 0x800000, 0, 0, 0x00BB);
    Scene s(d);
    Garages g;
    check(g.load(s.dir.string()) && g.count() == 3, "the three-garage scene loads");

    // Front projection at 90 deg: a 0x00AA door exactly on garage 0's front resolves to it.
    check(g.doorOf(door(0x00AA, kFront90, 0)) == 0, "a door at the 90-degree front area maps to garage 0");

    // Shared model: the door whose x+y is within a unit of garage 1's front picks garage 1, even though garage 0 shares
    // the model (GiveGarageHandleToThisDoor needs both model and proximity, and garage 0 fails proximity here).
    check(g.doorOf(door(0x00AA, 0x400000, kFront90)) == 1, "the shared-model door near garage 1 maps to garage 1");

    // Proximity boundary: (door.x+door.y) - (front.x+front.y) <= 0xFFF matches, one more does not. Garage 2's front sum
    // is 0x800000 + 19660; put the whole slack on x so the sum difference is exactly the tested value.
    const int32_t f2y = 0x800000 + kFront90;
    check(g.doorOf(door(0x00BB, 0xFFF, f2y)) == 2, "a door exactly 0xFFF past the front area still maps to garage 2");
    check(g.doorOf(door(0x00BB, 0x1000, f2y)) == -1, "a door one unit beyond 0xFFF is not garage 2's door");

    // GetGarageClosestToPoint seeds its best distance at 0x7A120000 and keeps garage 0 if nothing beats it. A point
    // 0x7D000000 east is farther than that from every garage, so the fallback returns garage 0.
    const int32_t faraway[3] = {0x7D000000, 0, 0};
    check(g.closest(faraway) == 0, "closest falls back to garage 0 when no garage is within the seed distance");
}

void run() {
    Fixture f;
    Garages g;
    check(g.load(f.dir.string()), "garages.dat loads from the production WAD path");
    check(g.count() == 2, "both 16-byte records become garages");

    // Heading and position are read back from the record (cGarage::GetHeading, +0x34 heading; position +0x18..).
    check(g.heading(0) == 0 && g.heading(1) == 0, "GetHeading returns the stored heading");
    int32_t at[3]; g.position(0, at);
    check(at[0] == 0x10000 && at[1] == 0x20000 && at[2] == 0x2000, "position returns the record coordinates");

    // closest() picks the garage whose front area is nearest the point (front is 4.8 units along heading from pos).
    const int32_t nearG1[3] = {0x100000, 0x5000, 0x7000};
    check(g.closest(nearG1) == 1, "closest resolves to the nearer garage's front area");

    // doorOf matches a kind-1 prop by door model and the front-area x+y proximity rule.
    check(g.doorOf(door(0x1234, 0x10000, 0x20000 + 19660)) == 0, "a kind-1 prop at the front area is garage 0's door");
    check(g.doorOf(door(0x5678, 0x100000, 19660)) == 1, "the second door model maps to garage 1");
    Collision::Prop notADoor = door(0x1234, 0x10000, 0x20000 + 19660); notADoor.kind = 0;
    check(g.doorOf(notADoor) == -1, "a non kind-1 prop is never a garage door");

    // Shut door: z is baseZ (z>>12) shifted back up, height 0. Garage 0 baseZ 2 -> 0x2000.
    check(g.state(0) == Garages::kShut && g.doorCollides(0), "a fresh garage is shut and its door collides");
    check(g.doorZ(0) == 0x2000, "a shut door sits at the whole-unit ground height");

    // Opening: +0x333 a frame, WhenOpen fires once when it passes 0x2CCD (2.8), then it holds at 0x3000 (3.0) open.
    int opened = 0;
    g.whenOpen(0, [&] { ++opened; });
    g.open(0);
    for (int i = 0; i < 15; ++i) { g.process(); check(g.state(0) == Garages::kOpening, "door is still rising"); }
    check(opened == 0, "WhenOpen has not fired before the door reaches 2.8");
    g.process();   // 16th frame crosses 0x2CCD
    check(g.state(0) == Garages::kOpen && opened == 1, "the 16th frame opens the door and runs WhenOpen once");
    check(g.doorZ(0) == 0x2000 + 0x3000 && !g.doorCollides(0), "an open door is raised 3.0 and does not collide");
    g.process();
    check(opened == 1, "WhenOpen runs only once");

    // Garage 1's open height makes doorZ sign-extend to 16 bits (0x7000 + 0x3000 = 0xA000 -> negative).
    g.open(1);
    for (int i = 0; i < 16; ++i) g.process();
    check(g.state(1) == Garages::kOpen && g.doorZ(1) == (int16_t)0xA000,
          "doorZ sign-extends the raised height to 16 bits as cGarage::Process stores it");

    // Closing: symmetric, -0x333 a frame, shut on the 16th frame.
    g.close(0);
    for (int i = 0; i < 15; ++i) { g.process(); check(g.state(0) == Garages::kClosing, "door is still lowering"); }
    g.process();
    check(g.state(0) == Garages::kShut && g.doorCollides(0) && g.doorZ(0) == 0x2000,
          "the 16th frame shuts the door back to the ground and it collides again");

    // Reload (cGarageManager::Reset): re-reading garages.dat clears every door's runtime state.
    g.open(1);
    check(g.state(1) == Garages::kOpening, "garage 1 is mid-open before the reload");
    check(g.load(f.dir.string()) && g.count() == 2, "reloading garages.dat succeeds");
    check(g.state(1) == Garages::kShut && g.doorZ(1) == 0x7000, "a reload returns the door to its shut state");
}
}   // namespace

int main() {
    run();
    runHeadings();
    if (failures) { std::fprintf(stderr, "%d garage check(s) failed\n", failures); return 1; }
    std::printf("garage checks passed\n");
    return 0;
}
