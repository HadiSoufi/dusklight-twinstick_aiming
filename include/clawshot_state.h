#pragma once

/* The clawshot's state machine, held in daAlink_c::mItemMode.
 *
 * The game's own enum is anonymous and file-local to d_a_alink_hook.inc, which
 * puts it out of reach here; it also names only four of the seven states and
 * leaves 2 and 5 as bare literals at their use sites.  Both are named below
 * from what their transitions do.
 *
 * These are named for what the machine is doing, not for what is on screen.
 * "Ready" was the game's word for state 1, and it reads as "the claw is up" --
 * which is true, and is the half that does not matter.  What matters is that a
 * shot is already committed and will fire the moment the button comes up. */
enum ClawshotState {
    kClawshotStowed = 0,          /* HS_MODE_NONE_e */
    kClawshotShotPending = 1,     /* HS_MODE_READY_e -- transient: the game
                                   * drains a 3-frame counter and fires. */
    kClawshotLeavingHand = 2,     /* unnamed in the game.  One frame: the tip is
                                   * still seeded at his hand, then -> 3. */
    kClawshotChainFlyingOut = 3,  /* HS_MODE_SHOOT_e */
    kClawshotPullingLink = 4,     /* HS_MODE_FLY_e */
    kClawshotAnchorCaught = 5,    /* unnamed in the game.  Caught an actor Link
                                   * gets pulled to, rather than one that comes
                                   * to him; both this and FLY enter the fly proc. */
    kClawshotChainReturning = 6,  /* HS_MODE_RETURN_e */
};

inline const char* clawshot_state_name(int state) {
    switch (state) {
    case kClawshotStowed:          return "stowed";
    case kClawshotShotPending:     return "shot-pending";
    case kClawshotLeavingHand:     return "leaving-hand";
    case kClawshotChainFlyingOut:  return "chain-flying-out";
    case kClawshotPullingLink:     return "pulling-link";
    case kClawshotAnchorCaught:    return "anchor-caught";
    case kClawshotChainReturning:  return "chain-returning";
    default:                       return "unnamed-clawshot-state";
    }
}

/* Everything from the claw leaving his hand onward. */
inline bool chain_is_in_air(int state) { return state >= kClawshotLeavingHand; }
