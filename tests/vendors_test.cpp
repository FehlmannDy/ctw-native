// Synthetic tests for the street-vendor ped behaviour (cVendor) state machine.
// Constants here are re-derived independently from the binary citations in
// vendors.h, not copied from the enum, so a drift in either side fails the test.

#include "vendors.h"

#include <cassert>
#include <string>
#include <vector>

using namespace ctw;

namespace {

// A fake task-framework host that records the call sequence and lets the test
// drive the connection / target / subtask conditions.
struct FakeHost : IVendorHost {
    bool connected = true;
    bool target = true;
    bool subTask = true;
    bool subTaskDone = false;
    int  subTaskSelector = 0;

    std::vector<std::string> log;
    int lastDeliverSelector = -1;
    int lastAnim = 0;
    int lastAnim2 = 0;

    bool isConnected() const override { return connected; }
    bool hasTarget() const override { return target; }
    void faceCustomer() override { log.push_back("face"); }
    void updateLookDirection() override { log.push_back("look"); }
    bool hasSubTask() const override { return subTask; }
    void startHandoverAnim(int a, int a2) override {
        log.push_back("anim");
        lastAnim = a;
        lastAnim2 = a2;
    }
    bool processSubTask(int& selector) override {
        log.push_back("process");
        selector = subTaskSelector;
        return subTaskDone;
    }
    void deliverToCustomer(int selector) override {
        log.push_back("deliver");
        lastDeliverSelector = selector;
    }
    void handleStdRequests() override { log.push_back("std"); }
    void clearCompletionFlag() override { log.push_back("clear"); }
    void emitFinished() override { log.push_back("finished"); }
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v)
        if (e == s)
            return true;
    return false;
}

}  // namespace

int main() {
    // Re-derived constants (independent of the enum).
    constexpr int kIdle = 20;       // 0x14, ctor seed
    constexpr int kStartAnim = 10;  // 0x0a
    constexpr int kAnim = 11;       // 0x0b
    constexpr int kFinish = 12;     // 0x0c, Reset target
    constexpr int kAnimId = 245;    // 0xf5
    constexpr int kAnimId2 = 274;   // 0x112

    // Enum values match the binary's state ids.
    assert(static_cast<int>(VendorState::Idle) == kIdle);
    assert(static_cast<int>(VendorState::StartAnim) == kStartAnim);
    assert(static_cast<int>(VendorState::Anim) == kAnim);
    assert(static_cast<int>(VendorState::Finish) == kFinish);
    assert(kVendorHandoverAnim == kAnimId);
    assert(kVendorHandoverAnim2 == kAnimId2);
    assert(kVendorFinishedWord == 257);  // 0x101
    assert(kVendorFinishedByte == 1);

    // 1) A fresh vendor starts Idle (ctor seeds 0x14).
    {
        FakeHost h;
        VendorTask v(h);
        assert(v.state() == VendorState::Idle);
    }

    // 2) Idle tick with a target: stands still + faces + looks, stays Idle.
    {
        FakeHost h;
        VendorTask v(h);
        v.process();
        assert(v.state() == VendorState::Idle);
        assert(has(h.log, "face"));
        assert(has(h.log, "look"));
        assert(!has(h.log, "finished"));
    }

    // 3) Idle tick with NO target: no facing turn, but look still runs.
    {
        FakeHost h;
        h.target = false;
        VendorTask v(h);
        v.process();
        assert(!has(h.log, "face"));
        assert(has(h.log, "look"));
        assert(v.state() == VendorState::Idle);
    }

    // 4) Disconnected: emit finished and bail (state untouched, no look/face).
    {
        FakeHost h;
        h.connected = false;
        VendorTask v(h);
        v.process();
        assert(has(h.log, "finished"));
        assert(!has(h.log, "look"));
        assert(!has(h.log, "face"));
        assert(v.state() == VendorState::Idle);
    }

    // 5) External cue (GotoNextState StartAnim): next tick attaches the hand-over
    //    animation with the verified ids and advances to Anim.
    {
        FakeHost h;
        VendorTask v(h);
        v.gotoNextState(VendorState::StartAnim);
        v.process();
        assert(v.state() == VendorState::Anim);
        assert(h.lastAnim == kAnimId);
        assert(h.lastAnim2 == kAnimId2);
    }

    // 6) Anim tick, subtask running but not finished: process + std + clear, stays Anim.
    {
        FakeHost h;
        h.subTask = true;
        h.subTaskDone = false;
        VendorTask v(h);
        v.gotoNextState(VendorState::Anim);
        v.process();
        assert(v.state() == VendorState::Anim);
        assert(has(h.log, "process"));
        assert(has(h.log, "std"));
        assert(has(h.log, "clear"));
        assert(!has(h.log, "deliver"));
    }

    // 7) Anim tick, subtask signals completion with selector 1: deliver with that
    //    selector, then return to Idle; std-request handling still runs.
    {
        FakeHost h;
        h.subTask = true;
        h.subTaskDone = true;
        h.subTaskSelector = kVendorDeliverSelectorA;
        VendorTask v(h);
        v.gotoNextState(VendorState::Anim);
        v.process();
        assert(has(h.log, "deliver"));
        assert(h.lastDeliverSelector == kVendorDeliverSelectorA);
        assert(v.state() == VendorState::Idle);
        assert(has(h.log, "std"));
    }

    // 8) Anim tick, subtask has vanished: go to Finish, clear flag, no std-handling.
    {
        FakeHost h;
        h.subTask = false;
        VendorTask v(h);
        v.gotoNextState(VendorState::Anim);
        v.process();
        assert(v.state() == VendorState::Finish);
        assert(has(h.log, "clear"));
        assert(!has(h.log, "std"));
        assert(!has(h.log, "process"));
    }

    // 9) Finish tick: emit finished, state stays Finish.
    {
        FakeHost h;
        VendorTask v(h);
        v.gotoNextState(VendorState::Finish);
        v.process();
        assert(has(h.log, "finished"));
        assert(v.state() == VendorState::Finish);
    }

    // 10) Reset forces Finish.
    {
        FakeHost h;
        VendorTask v(h);
        v.gotoNextState(VendorState::StartAnim);
        v.reset();
        assert(v.state() == VendorState::Finish);
    }

    return 0;
}
