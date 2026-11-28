// The street-vendor ped behaviour (cVendor). See vendors.h for the data model
// and the binary addresses each step is matched against. cVendor holds no money;
// the transaction effect is a cCustomer virtual call surfaced as a host hook.

#include "vendors.h"

namespace ctw {

// cVendor::Process (0x00720fb8)
void VendorTask::process() {
    // 0x00720ff0: if the task is no longer connected to its ped, emit the
    // "finished" output and bail (0x007210c4 path reached via the tbz at
    // 0x00720ff4). The base iSlaveTask::Process has already run (0x00720fe8).
    if (!host_.isConnected()) {
        host_.emitFinished();
        return;
    }

    // 0x00720ff8: only turn to face the customer while the target ped exists.
    // This covers sVirtYoke::ConstrainWalkSpeed(0,0) (stand still) and the
    // ATan2-derived heading write (0x00721010..0x00721058).
    if (host_.hasTarget()) {
        host_.faceCustomer();
    }
    // 0x0072105c: cCustomer::UpdateLookDirection runs unconditionally each tick.
    host_.updateLookDirection();

    // 0x00721068: the state switch on cVendor+0x2c.
    switch (state_) {
        case VendorState::Finish:  // 0xc (0x0072106c: cmp #0xc, b.eq 0x007210c4)
            host_.emitFinished();
            return;

        case VendorState::Anim:  // 0xb (0x00721074: cmp #0xb, b.eq 0x007210fc)
            if (!host_.hasSubTask()) {
                // 0x00721168: the animation subtask is gone -> finish next.
                state_ = VendorState::Finish;
                host_.clearCompletionFlag();  // 0x00721170
            } else {
                int selector = 0;
                // 0x00721114: ProcessSubTask + the completion test at 0x00721118.
                if (host_.processSubTask(selector)) {
                    // 0x00721124..0x00721148: the cCustomer vtable call, then
                    // return to Idle (0x00721148: str #0x14).
                    host_.deliverToCustomer(selector);
                    state_ = VendorState::Idle;
                }
                // 0x0072115c: standard task-request handling while a subtask runs.
                host_.handleStdRequests();
                host_.clearCompletionFlag();  // 0x00721160
            }
            break;

        case VendorState::StartAnim:  // 0xa (0x0072107c: cmp #0xa)
            // 0x00721084..0x007210b4: attach a one-shot hand-over animation,
            // then advance to Anim (0x007210b8: str #0xb).
            host_.startHandoverAnim(kVendorHandoverAnim, kVendorHandoverAnim2);
            state_ = VendorState::Anim;
            break;

        case VendorState::Idle:  // 0x14 - steady state, no transition (tail 0x007210d4)
        default:
            break;
    }
}

}  // namespace ctw
