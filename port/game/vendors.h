#ifndef CTW_GAME_VENDORS_H
#define CTW_GAME_VENDORS_H

// The street-vendor ped behaviour (cVendor). Unlike the drug economy (dealer.*),
// cVendor is NOT a shop and holds NO money or inventory of its own: it is a ped
// AI slave task (iSlaveTask) that drives how a vendor ped behaves while a
// customer is being served. It stands the vendor still, turns it to face the
// approaching customer ped, and on an external cue plays a one-shot "hand-over"
// animation; it finishes when the task is disconnected or reset.
//
// Ported routine-by-routine against the original Android ARM64 libGame.so. State
// ids, animation ids and the sTaskOut "finished" encoding are cited from the
// binary. The actual transaction effect (giving the item / taking the money) is
// NOT in cVendor: it is a virtual call into the shared cCustomer base
// (vtable +0xc0 / +0xc8 at 0x00721140), surfaced here only as the injected
// IVendorHost::deliverToCustomer hook. The task framework it rides on
// (iSlaveTask / iAITask / cCustomer / cOneShotAnimationTask / sVirtYoke / ATan2)
// is shared ped code (off-limits), injected through IVendorHost so the vendor's
// own state machine is testable in isolation -- the same injection pattern the
// dealer used for its wallet.

#include <cstdint>

namespace ctw {

// State ids stored at cVendor+0x2c, observed from the binary.
//   Idle      seeded by the ctor (0x00720f64: str #0x14).
//   StartAnim / Anim / Finish from the Process switch (0x0072106c..0x00721080)
//   and Reset (0x007211a4: str #0xc).
enum class VendorState : int {
    Idle      = 0x14,  // 20 - ctor initial; stand still and keep facing the customer
    StartAnim = 0x0a,  // 10 - external cue: begin the hand-over animation this tick
    Anim      = 0x0b,  // 11 - the one-shot hand-over animation subtask is running
    Finish    = 0x0c,  // 12 - emit the task-finished output (also the Reset target)
};

// The two eGenericSpriteAnim ids passed to cOneShotAnimationTask at the start of
// the hand-over (0x00721098: mov w1,#0xf5 / 0x0072109c: mov w2,#0x112). The third
// ctor arg (loop/flag) is 0 (0x007210a0: mov w3, wzr). Observation.
inline constexpr int kVendorHandoverAnim  = 0xf5;   // 245
inline constexpr int kVendorHandoverAnim2 = 0x112;  // 274

// sTaskOut "task finished" encoding written on Finish and on disconnect
// (0x007210c4: strh #0x101 @+8 ; 0x007210d0: strb #1 @+0xa). Observation.
inline constexpr uint16_t kVendorFinishedWord = 0x101;  // 257
inline constexpr uint8_t  kVendorFinishedByte = 0x01;

// The cCustomer vtable selector picked at completion (0x00721134: cmp w8,#1 ->
// +0xc0 when out[+9]==1, else +0xc8). The selector value is out[+9]; this model
// carries it through to deliverToCustomer without interpreting it. Observation.
inline constexpr int kVendorDeliverSelectorA = 1;  // out[+9]==1 -> vtable +0xc0

// Framework operations cVendor delegates to. These live in shared ped/task code
// (iSlaveTask / iAITask / cCustomer / cOneShotAnimationTask / sVirtYoke), which
// is off-limits here, so they are injected. A real integration binds each to the
// engine task object; tests supply a fake and observe the call sequence.
struct IVendorHost {
    virtual ~IVendorHost() = default;

    // iSlaveTask::IsConnected (0x00720ff0 -> 0xd38260): is the task still attached
    // to a living ped. When false, Process emits "finished" and returns.
    virtual bool isConnected() const = 0;

    // The weak ptr at cVendor+0x30 (the customer ped the vendor faces) is non-null
    // (0x00720ff8: ldr x8,[x19,#0x30]; ldr x8,[x8]; cbz). Gates the facing turn.
    virtual bool hasTarget() const = 0;

    // Stop the vendor walking (sVirtYoke::ConstrainWalkSpeed(0,0) @0x00721010) and
    // turn it to face the customer -- heading = -ATan2(delta of the two peds' XY
    // at +0x50) written to the yoke (0x00721040..0x00721058). Only when hasTarget.
    virtual void faceCustomer() = 0;

    // cCustomer::UpdateLookDirection (0x00721064 -> 0xd37840) on the cCustomer
    // sub-object at +0x20. Called every tick, even when hasTarget is false.
    virtual void updateLookDirection() = 0;

    // A cOneShotAnimationTask subtask is currently attached: cVendor+0x10 != 0
    // (0x007210fc: ldr x8,[x19,#0x10]; cbz).
    virtual bool hasSubTask() const = 0;

    // iAITask::AddSubTask(new cOneShotAnimationTask(anim, anim2, false))
    // (0x00721084..0x007210b4): begin the hand-over animation.
    virtual void startHandoverAnim(int anim, int anim2) = 0;

    // iAITask::ProcessSubTask (0x00721114) plus the completion test at 0x00721118
    // (in-flag [in+0x20] OR out-flag [out+8]). Returns true when the subtask
    // signalled completion this tick; `selector` receives out[+9] for delivery.
    virtual bool processSubTask(int& selector) = 0;

    // The transaction effect: the cCustomer virtual call at 0x00721140 (+0xc0 when
    // selector==kVendorDeliverSelectorA, else +0xc8). cVendor moves no money of its
    // own; the effect lives entirely in cCustomer (a cross-file hook).
    virtual void deliverToCustomer(int selector) = 0;

    // iAITask::HandleStdTaskRequests (0x0072115c): standard per-tick request
    // handling, run only while a subtask is attached.
    virtual void handleStdRequests() = 0;

    // Clear the subtask completion flag out[+8] (0x00721160 / 0x00721170:
    // strb wzr,[out+8]) after the Anim tick is handled.
    virtual void clearCompletionFlag() = 0;

    // Emit the task-finished sTaskOut encoding (kVendorFinishedWord @+8,
    // kVendorFinishedByte @+0xa) on Finish / disconnect (0x007210c4).
    virtual void emitFinished() = 0;
};

// cVendor: the street-vendor ped behaviour task. This models cVendor's own state
// machine; the engine base classes it derives from are represented by IVendorHost.
class VendorTask {
public:
    explicit VendorTask(IVendorHost& host) : host_(host) {}

    VendorState state() const { return state_; }

    // cVendor::GotoNextState (0x007211b0): overwrite the state field (str w1,[x0,#0x2c]).
    void gotoNextState(VendorState s) { state_ = s; }

    // cVendor::Reset (0x007211a4): set state to Finish, then iSlaveTask::Reset
    // (the base reset is a host concern and is not modelled here).
    void reset() { state_ = VendorState::Finish; }

    // cVendor::Process (0x00720fb8): advance the task one tick. The base
    // iSlaveTask::Process at the top (0x00720fe8) is a host side effect and is
    // assumed already applied by the caller before this runs.
    void process();

private:
    IVendorHost& host_;
    VendorState state_ = VendorState::Idle;  // ctor seeds 0x14 (0x00720f64)
};

}  // namespace ctw

#endif  // CTW_GAME_VENDORS_H
