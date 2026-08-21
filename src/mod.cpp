/*
 * Twinstick Aiming
 *
 * Twin-stick item aiming: the left stick keeps moving Link while the right stick
 * (C-stick) drives the aiming reticle.
 *
 * Aim is *entered* the vanilla way, by holding the item button.  What changes is
 * what a release means.  Vanilla fires the moment the button comes up, which
 * leaves no room to actually aim; here a release does nothing and a second click
 * fires.
 *
 * mItemButton is not the item system's private state: swordButton() is
 * itemButtonCheck(BTN_B) and mUseButtonFlags drives the on-screen button
 * prompts.  So it is never held across frames.  Each override sets the bit in a
 * pre-hook and clears it in the paired post-hook, leaving the value untrue only
 * for the duration of one named call:
 *
 *   checkUpperItemAction{Boomerang,CopyRod,IronBall,Hookshot}
 *       fires on `!itemButton()`, so a release throws.
 *   procIronBallSubject
 *       only calls setBodyAngleToCamera() while itemButton() is true, so the
 *       ball and chain would aim at nothing without this.
 *   checkAimContext
 *       gates Dusklight's mouse and gyro aim on itemButton() for the ball.
 *
 * Entry differs by item class.  Third-person items (boomerang, ball and chain,
 * copy rod) use vanilla timing: a hold aims, a click throws.  First-person items
 * (bow, slingshot, clawshot) also enter on a click, which is done by entering
 * the subject proc directly rather than by re-timing mFastShotTime.
 */

#include <cstdio>

#include "clawshot_state.h"
#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/ui.h"

#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_button.h"
#include "JSystem/J2DGraph/J2DMatBlock.h"
#include "JSystem/J2DGraph/J2DMaterial.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/J2DGraph/J2DPictureEx.h"
#include "d/d_pane_class.h"
#include "m_Do/m_Do_controller_pad.h"

DEFINE_MOD();

IMPORT_SERVICE(HookService,   svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService,     svc_ui);

static ConfigVarHandle g_cfg_combat_compat = 0;
static ConfigVarHandle g_cfg_bow_reticle = 0;

/* ------------------------------------------------------------------
 *  Proc classification
 * ------------------------------------------------------------------ */
/* Named constants for everything this mod tests or writes.  Hex in a condition
 * is a bug waiting to happen: it cannot be read, cannot be grepped for meaning,
 * and has to be looked up every single time. */

/* Status0.  Set where alink starts the subjectivity proc, alongside the
 * HAWK_EYE_PUTON sound, and cleared when the scope comes off. */
static const u32 kStatusScopeOn = 0x200000;

/* dMeter2_c::mStatus bits that alphaAnimeButtonZ reads as "hide".  0x1000 is
 * set throughout aiming (getItemSubject); 0x80 is the lock-on camera. */
static const u32 kMeterHideDuringAim = 0x1000;
static const u32 kMeterHideLockOnCamera = 0x80;

/* J2DTevBlock ownership: clearing the low bit stops setTexture freeing the
 * texture it displaces. */
static const u8 kKeepDisplacedTexture = 0xFE;

/* Angles are 16-bit: a full turn is 0x10000. */
static const s16 kQuarterTurn = 0x4000;
static const s16 kEighthTurn = 0x2000;
static const s16 kSixteenthTurn = 0x1000;
static const s16 kNearReversalAngle = 0x7800; /* ~168 degrees */
static const s16 kHalfTurn = -0x8000;


/* The procs this mod names, for logging.  Reading "hookshot-move" in a trace is
 * the difference between seeing a bug and looking up 0x00C5 for the fifth
 * time. */
static const char* item_name(u16 item_id) {
    switch (item_id) {
    case dItemNo_NONE_e: return "none";
    case dItemNo_BOW_e: return "bow";
    case dItemNo_PACHINKO_e: return "slingshot";
    case dItemNo_BOOMERANG_e: return "boomerang";
    case dItemNo_COPY_ROD_e: return "copyrod";
    case dItemNo_IRONBALL_e: return "ironball";
    case dItemNo_HOOKSHOT_e: return "clawshot";
    case dItemNo_W_HOOKSHOT_e: return "double-clawshot";
    case dItemNo_HAWK_EYE_e: return "hawkeye";
    case dItemNo_HAWK_ARROW_e: return "hawkeye-bow";
    case dItemNo_BOMB_ARROW_e: return "bomb-arrow";
    default: return "other";
    }
}

static const char* proc_name(u16 proc_id) {
    switch (proc_id) {
    case daAlink_c::PROC_WAIT: return "wait";
    case daAlink_c::PROC_MOVE: return "move";
    case daAlink_c::PROC_SUBJECTIVITY: return "subjectivity";
    case daAlink_c::PROC_BOW_SUBJECT: return "bow-aim";
    case daAlink_c::PROC_BOOMERANG_SUBJECT: return "boomerang-aim";
    case daAlink_c::PROC_COPY_ROD_SUBJECT: return "copyrod-aim";
    case daAlink_c::PROC_IRON_BALL_SUBJECT: return "ironball-aim";
    case daAlink_c::PROC_HOOKSHOT_SUBJECT: return "clawshot-aim";
    case daAlink_c::PROC_HOOKSHOT_MOVE: return "clawshot-shot";
    case daAlink_c::PROC_HOOKSHOT_FLY: return "clawshot-fly";
    case daAlink_c::PROC_HAWK_SUBJECT: return "hawkeye-aim";
    default: return "other";
    }
}

/* Whether the player is aiming, which is broader than being in a first-person
 * proc.  Z-targeting aims too: Link stays on screen and the target is whatever
 * is in front of him, but he is pointing the item just the same, so a press
 * there is a shot exactly as it is in first person.  The clawshot does not even
 * reach a subject proc while targeting -- checkNextActionHookshot routes it to
 * the move proc instead -- so a test on the proc alone reports "not aiming" for
 * the whole gesture and the shot is suppressed forever.
 *
 * checkAttentionLock is true both for a real lock and for the button held at
 * nothing.  Those are the same mode and behave identically; only the choice of
 * target differs. */
static bool player_is_aiming(daAlink_c* link);

static bool is_aiming_proc(u16 proc_id) {
    switch (proc_id) {
    case daAlink_c::PROC_BOW_SUBJECT:
    case daAlink_c::PROC_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_HOOKSHOT_SUBJECT:
    case daAlink_c::PROC_COPY_ROD_SUBJECT:
    case daAlink_c::PROC_IRON_BALL_SUBJECT:
    case daAlink_c::PROC_HOOKSHOT_ROOF_WAIT:
    case daAlink_c::PROC_HOOKSHOT_ROOF_SHOOT:
    case daAlink_c::PROC_HOOKSHOT_WALL_WAIT:
    case daAlink_c::PROC_HOOKSHOT_WALL_SHOOT:
    case daAlink_c::PROC_HORSE_BOW_SUBJECT:
    case daAlink_c::PROC_HORSE_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_HORSE_HOOKSHOT_SUBJECT:
    case daAlink_c::PROC_HORSE_SUBJECTIVITY:
    case daAlink_c::PROC_CANOE_BOW_SUBJECT:
    case daAlink_c::PROC_CANOE_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_CANOE_HOOKSHOT_SUBJECT:
    case daAlink_c::PROC_CANOE_SUBJECTIVITY:
    case daAlink_c::PROC_SWIM_HOOKSHOT_SUBJECT:
    case daAlink_c::PROC_SWIM_SUBJECTIVITY:
    case daAlink_c::PROC_SUBJECTIVITY:
    case daAlink_c::PROC_BOARD_SUBJECTIVITY:
    case daAlink_c::PROC_HAWK_SUBJECT:
        return true;
    default:
        return false;
    }
}

/* Movement is handed straight back the moment a shot leaves.
 *
 * The game zeroes mNormalSpeed and mSpeedModifier at the fire and puts the shoot
 * clip on the full-body channel.  Keeping Link moving through that keeps the
 * game's attention-move stance blender live -- the one that picks between the
 * aim stance and the walk clips by speed -- and it reasserts the aim stance over
 * the shoot clip on the following frame.  Link then fires with his arm parked on
 * frame zero of the wait clip, which is the arm-down pose.
 *
 * mItemMode is only read as a clawshot state inside the clawshot's own proc,
 * because every item means something different by that field. */
static bool mod_drives_movement(const daAlink_c* link) {
    if (link->mProcID == daAlink_c::PROC_HOOKSHOT_SUBJECT &&
        chain_is_in_air(link->mItemMode)) {
        return false;
    }

    switch (link->mProcID) {
    case daAlink_c::PROC_BOW_SUBJECT:
    case daAlink_c::PROC_BOOMERANG_SUBJECT:
    case daAlink_c::PROC_HOOKSHOT_SUBJECT:
    case daAlink_c::PROC_COPY_ROD_SUBJECT:
    case daAlink_c::PROC_IRON_BALL_SUBJECT:
    case daAlink_c::PROC_HAWK_SUBJECT:
    case daAlink_c::PROC_SUBJECTIVITY:
    case daAlink_c::PROC_SWIM_SUBJECTIVITY:
        return true;
    default:
        return false;
    }
}

/* The bow, slingshot and the arrow variants all run the same procs. */
static bool is_bow_family(u16 item_id) {
    return item_id == dItemNo_BOW_e || item_id == dItemNo_PACHINKO_e ||
           item_id == dItemNo_BOMB_ARROW_e || item_id == dItemNo_HAWK_ARROW_e;
}

/* Items that used R for something else while aiming, and now use Z instead:
 * the boomerang's target lock and the bow's arrow switch.  Z is free in both
 * cases -- BTN_Z is read by exactly one thing, midnaTalkTrigger, and Midna
 * cannot be called mid-aim. */
static bool action_moved_to_z(u16 item_id) {
    return item_id == dItemNo_BOOMERANG_e || is_bow_family(item_id);
}

/* Set for the frame the game offered the arrow switch, which only happens with
 * the hawkeye.  Without this the label showed on the plain bow, where there is
 * nothing to switch. */
static bool g_bow_arrow_switch_offered = false;

/* Whether the game offered the boomerang's multi-target lock last frame.
 *
 * setBoomerangSight calls setItemActionButtonStatus(BUTTON_STATUS_LOCK) only
 * when there is actually something to lock -- a target on the sight line, and
 * the lock count not yet at its cap.  Watching for that call takes the answer
 * from the game rather than re-deriving it from daBoomerang_c's internals,
 * which is the only way it can stay right when those rules change.
 *
 * Read a frame late, because setBoomerangSight runs after the row is painted.
 * A prompt one frame behind the reticle is invisible; a second implementation
 * of the availability rule would not be. */
static bool g_boomerang_lock_offered = false;
static bool g_boomerang_lock_seen_this_frame = false;

static bool cfg_combat_compat() {
    bool val = false;
    svc_config->get_bool(mod_ctx, g_cfg_combat_compat, &val);
    return val;
}

static bool cfg_bow_reticle() {
    bool val = false;
    svc_config->get_bool(mod_ctx, g_cfg_bow_reticle, &val);
    return val;
}

/* Buttons an item can sit on.  Vanilla only ever assigns items to X and Y --
 * checkItemChangeAutoAction literally loops `for (i = 0; i < 2; i++)` -- and B is
 * the sword, so treating B as an item button by default would misread a sword
 * swing as an item press.  Input mods that repurpose B opt in. */
static u8 item_button_mask() {
    u8 bits = (u8)(daAlink_c::BTN_X | daAlink_c::BTN_Y);
    if (cfg_combat_compat()) {
        bits |= (u8)daAlink_c::BTN_B;
    }
    return bits;
}

static bool g_computing_movement = false;

/* How many times movement was actually computed this frame.  More than one is
 * the root cause behind every symptom this code has produced. */
static int g_movement_calls = 0;

/* A fault that lasts two seconds is one fault, not a hundred log lines.  Each
 * check reports when it starts and when it clears, with the duration. */
DEFINE_HOOK(&daAlink_c::setSpeedAndAngleNormal, SpeedAndAngleNormal);

static HookAction on_speed_and_angle_normal_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link == nullptr || !mod_drives_movement(link)) {
        return HOOK_CONTINUE;
    }

    /* Whose call is this?  The game makes its own inside checkNextAction, and
     * the mod's setSpeedAndAngleAtn lands here too whenever the stick is inside
     * the forward cone.  Both writing speedF in one frame is the pulse -- a
     * constant 9.10 trading frames with a ramp restarting around 3.
     *
     * Skipping the function outright killed the mod's delegation along with the
     * game's, which is what left frames with no writer at all.  So skip only the
     * calls that did not come from us. */
    if (!g_computing_movement) {
        return HOOK_SKIP_ORIGINAL;
    }

    g_movement_calls++;

    /* Do not skip our own -- setSpeedAndAngleAtn only owns the strafe directions.
     * With no targeted actor, which is always the case while aiming, its forward
     * branch delegates straight back here and returns, so skipping left the frame
     * with no writer at all: the travel angle stuck and speed held whatever it
     * last had, including a negative one.
     *
     * What actually has to go is the stall.  Normal reads a heading more than
     * ~168 degrees off current.angle.y as "turn in place, set no speed", which is
     * a sane rule for a Link who faces where he walks and a permanent one for a
     * Link whose facing is pinned to his aim.  Collapsing that gap up front means
     * the test cannot trip, and Normal goes on to do the ordinary thing: travel
     * along the stick at speed.  Nothing else about its behaviour changes, and
     * the smoothing is left alone whenever the gap is small. */
    const s16 requested_heading = link->mMoveAngle;
    const s16 travel_heading = link->current.angle.y;

    if (cLib_distanceAngleS(requested_heading, travel_heading) > kNearReversalAngle) {
        link->current.angle.y = requested_heading;
    }

    return HOOK_CONTINUE;
}

/* ------------------------------------------------------------------
 *  A legend for the game's movement fields
 *
 *  These are decompiled names and none of them say what they hold, which is a
 *  large part of why this code was hard to get right.  In this file they are
 *  always read into locals named for their meaning; the mapping is:
 *
 *    current.angle.y   travel heading -- the direction Link actually moves.
 *                      Velocity is speedF along this angle and nothing else.
 *    shape_angle.y     model heading -- purely which way the model is drawn.
 *                      Aiming pins it; it has no effect on where he goes.
 *    mMoveAngle        requested heading -- where the stick is asking to go, in
 *                      world space: stick heading plus the camera's angle.  It
 *                      therefore changes when the camera moves, stick untouched.
 *    mStickAngle       the raw stick heading, before the camera is applied.
 *    mPrevStickAngle   last frame's mStickAngle.  Several routines compare the
 *                      two to detect a flick.
 *    speedF            current speed along the travel heading.
 *    mNormalSpeed      the accumulator speed is eased toward between frames.
 *    mStickValue       how far the stick is pushed, 0 to 1.
 *    mMoveValue        the same, as movement code reads it.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 *  Right stick -> aiming
 * ------------------------------------------------------------------ */
DEFINE_HOOK(&daAlink_c::setBodyAngleToCamera, SetBodyAngleToCamera);

static f32 g_left_stick_move;
static f32 g_left_stick_amount;
static s16 g_left_stick_angle;

static HookAction on_set_body_angle_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (!is_aiming_proc(link->mProcID)) return HOOK_CONTINUE;

    g_left_stick_move  = link->mMoveValue;
    g_left_stick_amount = link->mStickValue;
    g_left_stick_angle = link->mStickAngle;

    f32 subValue = mDoCPd_c::getSubStickValue(PAD_1);
    s16 subAngle = mDoCPd_c::getSubStickAngle(PAD_1);

    link->mMoveValue  = subValue;
    link->mStickValue = subValue;
    /* The C-stick angle is half a turn out from what the aiming code expects. */
    link->mStickAngle = subAngle - kHalfTurn;

    return HOOK_CONTINUE;
}

static void on_set_body_angle_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (!is_aiming_proc(link->mProcID)) return;

    link->mMoveValue  = g_left_stick_move;
    link->mStickValue = g_left_stick_amount;
    link->mStickAngle = g_left_stick_angle;
}

/* posMove is where the speed the player feels is actually produced:
 *
 *   speedF = mNormalSpeed * (1 - |mSpeedModifier|)   then   *= cos(ground angle)
 *
 * Measured at our own hook the accumulator looked healthy at 11.78 while the
 * applied speed was 4.5, so the loss is inside this derivation.  Logging its
 * inputs at the moment it runs is the only way to see which term takes it. */
/* Sited on checkNextAction: the subject procs call it unconditionally, and it
 * is where the game's own movement dispatch lives, so it reads the state the
 * game's own call would have.  (setBodyAngleToCamera sits behind two
 * conditionals and is skipped on many frames.)
 *
 * Atn rather than Normal because the facing is pinned to the aim.
 * setSpeedAndAngleNormal is written for a Link who turns to face where he walks
 * and treats a heading more than ~168 degrees off the travel angle as "turn in
 * place, set no speed" -- permanently true here.  setSpeedAndAngleAtn has no
 * such case and reads shape_angle only to pick the strafe animation. */
DEFINE_HOOK(&daAlink_c::checkNextAction, CheckNextAction);

static void on_check_next_action_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link == nullptr || !mod_drives_movement(link)) {
        return;
    }

    /* Saved because setSpeedAndAngleNormal, which Atn can still reach, finishes by
     * turning the model toward the direction of travel.  Aiming owns the model
     * heading, so it goes back afterwards. */
    const s16 model_heading = link->shape_angle.y;

    /* Captured either side of the one call that computes movement, because the
     * oscillation survives with a single writer -- so the cause is inside this
     * call, not a second one.  mNormalSpeed is the accumulator setSpeedAndAngleAtn
     * carries between frames, and the direction bucket is what selects its
     * forward branch (0) from the strafe ones. */
    /* The call itself no longer jumps speed, yet speed is still low with the stick
     * down -- so something changes speedF between our calls.  Comparing what we
     * left it at last frame against what it is now names that writer's existence,
     * which is the one thing still unaccounted for. */
    static f32 last_speed = 0.0f;
    static bool have_last_speed = false;

    /* mNormalSpeed, not speedF.  d_a_alink.cpp:13028 recomputes speedF from
     * mNormalSpeed every frame and then scales it by the ground slope, so speedF
     * is a derived value and anything written to it is discarded.  mNormalSpeed
     * is the state that actually carries. */
    const f32 speed_before = link->mNormalSpeed;
    const int direction_bucket = link->getDirectionFromShapeAngle();

    /* No longer forces a target.  Doing so put every direction on the lock-on
     * movement model, whose mAtnMove.mAcceleration is 9.10 against a free-running
     * max of 23.00 -- measured pinned at exactly 9.10 for as long as the stick was
     * held forward.  That is what the drag was, and why the delay before it varied:
     * mNormalSpeed eases toward the target from whatever speed the aim was entered
     * at, so sprinting in took seconds to bleed down and creeping in was instant.
     *
     * The reason the target was forced -- that forward otherwise delegates to
     * setSpeedAndAngleNormal and stalls there -- is handled at the source now: the
     * near-reversal test that caused the stall is defused in that function's own
     * pre-hook, and single ownership stops the writers fighting. */
    /* Nudge the facing out of the forward cone for the duration of the call.
     *
     * setSpeedAndAngleAtn picks a direction bucket from the angle between the
     * movement and the facing, and bucket 0 -- within about 8 degrees of straight
     * ahead -- is the one case it does not handle itself: it delegates to
     * setSpeedAndAngleNormal, which targets a lower speed.  Measured over a
     * stepped circle, forward settled at 10.09 against 13.00 for every other
     * direction.
     *
     * A Link whose facing is pinned to his aim is strafing in every direction, so
     * there is no forward case to speak of; the nudge is only large enough to say
     * so.  It is restored before the animation blend, which reads the real facing. */
    const s16 kOutOfForwardCone = kEighthTurn; /* 45 degrees */
    link->shape_angle.y = (s16)(model_heading + kOutOfForwardCone);

    g_computing_movement = true;
    link->setSpeedAndAngleAtn();
    g_computing_movement = false;

    link->shape_angle.y = model_heading;

    /* Hold the speed while the stick is held.
     *
     * Measured on a stepped circle: every direction settles flat at 13.00 except
     * dead ahead, which alternates 9.10 <-> 13.00 forever.  stick, mMoveValue and
     * mMaxSpeed are all constant across those frames, so max_speed is not moving;
     * what alternates is the speed argument Atn passes to setNormalSpeedF -- a
     * real acceleration one frame, then nothing, which sends it down the
     * "decelerate toward zero" branch and takes the step straight back off.
     *
     * A held stick is never a request to slow down, so the drop is refused.  It
     * is limited to a full stick clear of a wall, leaving every genuine reason to
     * lose speed -- releasing the stick, collision, terrain -- to work normally. */
    const bool stick_held = link->mStickValue > 0.9f;
    const bool touching_wall = link->mLinkAcch.ChkWallHit() != 0;

    /* Only while the sign is unchanged.  Atn has a branch that turns the travel
     * angle 180 degrees and negates the speed at the same time -- the pair means
     * the same motion.  Restoring a positive speed onto a flipped angle would
     * drive Link backwards, so a sign change is left alone. */
    const bool same_direction = (speed_before > 0.0f) == (link->mNormalSpeed > 0.0f);

    if (stick_held && !touching_wall && same_direction && link->mNormalSpeed < speed_before) {
        link->mNormalSpeed = speed_before;
    }

    /* setBlendAtnMoveAnime ends with `mSpeedModifier = 1.0f - var_f31`, the blend
     * weight of the strafe animation.  posMove then applies it as
     * `speedF = mNormalSpeed * (1 - |mSpeedModifier|)`, so calling this for the
     * animation also charges a speed tax that belongs to lock-on sidestepping,
     * where moving slowly is the point.  Measured walking forward: an accumulator
     * of 12.00 arriving at posMove with a modifier of 0.69 and leaving as 4.25.
     *
     * The animation is wanted; the tax is not.  It is a side effect of the call,
     * so it is undone right after it. */
    const f32 speed_modifier = link->mSpeedModifier;

    link->setBlendAtnMoveAnime(link->mpHIO->mBasic.m.mBasicInterpolation);

    link->mSpeedModifier = speed_modifier;
}



/* changeArrowType puts BUTTON_STATUS_SWITCH on R.  Move it to the Z slot, which
 * is where the arrow switch now lives, and give R the shot. */

/* setBoomerangSight puts BUTTON_STATUS_LOCK on R, and it runs later in the frame
 * than our observer, so the prompt has to be corrected here rather than earlier:
 * lock moves to the Z slot and R advertises the throw. */

/* What RT advertises on the HUD.  RT does not have its own behaviour -- it is an
 * alias for the item button while aiming -- so this is only a label. */
static u8 rt_prompt_status(const daAlink_c* link) {
    const u16 item_id = link->mEquipItem;

    if (item_id == dItemNo_BOOMERANG_e || item_id == dItemNo_COPY_ROD_e ||
        item_id == dItemNo_IRONBALL_e)
    {
        return BUTTON_STATUS_THROW;
    }
    if (daAlink_c::checkHookshotItem(item_id)) {
        return BUTTON_STATUS_HOOK;
    }

    /* The bow is the only item here with two phases, and the label has to follow
     * them or RT reads as a button that does nothing once the arrow is nocked.
     * ASHOOTWAIT is up-but-undrawn, ARELORD is nocking, ARELORDTAME is drawn and
     * held -- anything past the nock is a shot waiting to be let go.  There is no
     * vanilla prompt to fall back on: the game never labels RT during aiming, and
     * its own BUTTON_STATUS_DRAW is the sword draw, not this. */
    if (is_bow_family(item_id)) {
        if (link->checkBowChargeWaitAnime() || link->checkBowReloadAnime()) {
            return BUTTON_STATUS_RELEASE;
        }
        return BUTTON_STATUS_DRAW;
    }

    return BUTTON_STATUS_NONE;
}


/* ------------------------------------------------------------------
 *  The bottom-middle prompt row
 *
 *  dMeter2_c's "emphasis" row: two slots, drawn bottom-middle with real button
 *  icons.  The mod puts the rebound button in one and the fire action in the
 *  other.  See "D:/Projects/Dusklight Mods/prompt-row.md" for how the row is
 *  driven and why it is done this way -- the short version is that the row is painted slot by slot in a
 *  dMeterButton_c::_execute pre-hook, because the obvious route (setting the Z
 *  status) repaints the top-right cluster as a side effect.
 * ------------------------------------------------------------------ */
/* The RB half of the row: what the rebound button does for the item in hand. */
static u8 z_prompt_status(u16 item_id) {
    /* Only while there is something to lock on to, which is what vanilla does
     * with the same prompt on R. */
    if (item_id == dItemNo_BOOMERANG_e) {
        return g_boomerang_lock_offered ? BUTTON_STATUS_LOCK : BUTTON_STATUS_NONE;
    }
    /* With the hawkeye the switch is always live while aiming: changeArrowType's
     * only bail-out for HAWK_ARROW is checkAttentionLock, and aim entry already
     * excludes that.  Deciding it from the item rather than from last frame's
     * hook removes the few frames at the start of an aim where the RB entry did
     * not exist yet.  The latch still covers the bomb-arrow quiver, whose
     * availability depends on ammo the mod cannot see. */
    if (item_id == dItemNo_HAWK_ARROW_e) {
        return BUTTON_STATUS_SWITCH;
    }
    if (is_bow_family(item_id) && g_bow_arrow_switch_offered) {
        return BUTTON_STATUS_SWITCH;
    }
    return BUTTON_STATUS_NONE;
}

/* ------------------------------------------------------------------
 *  prompt_row -- render an arbitrary pair of (label, button) on the
 *  bottom-middle emphasis row.
 *
 *  The row is two slots, so this takes two entries: you describe the slots
 *  rather than handing over a list to be arranged.  A slot whose button is
 *  BUTTON_NONE_e is left exactly as the game left it, which is how a vanilla
 *  prompt survives beside one of ours.
 *
 *      prompt_row::render(btn, args,
 *                         {dMeterButton_c::BUTTON_Z_e, BUTTON_STATUS_LOCK},
 *                         {dMeterButton_c::BUTTON_R_e, BUTTON_STATUS_NONE, "Throw"});
 *
 *  Give a status to use the game's own wording for it, or a literal string.
 *
 *  This covers gates 2 to 4; the caller owns eligibility, because that has to be
 *  established earlier in the frame -- see keep_open.
 * ------------------------------------------------------------------ */
namespace prompt_row {
/* Which button a row slot is showing.  (dMeterButton_c::field_0x4be.) */
static u8& slot_button(dMeterButton_c* row, int slot) {
    return row->field_0x4be[slot];
}

/* How much width a slot has claimed, which setString centres the pair on.
 * (dMeterButton_c::field_0x1e4.) */
static f32& slot_claimed_width(dMeterButton_c* row, int slot) {
    return row->field_0x1e4[slot];
}

/* A button's centre, measured once at init from the untouched layout.
 * (dMeterButton_c::field_0x244.) */
static f32& button_centre(dMeterButton_c* row, u8 button) {
    return row->field_0x244[button];
}



static const int kSlots = 2;
static const int kTextMax = 32;  /* matches dMeterButton_c::mButtonText[2][32] */

struct Entry {
    u8 button;         /* BUTTON_NONE_e leaves the slot to the game */
    u8 status;         /* the game's wording for this action        */
    const char* text;  /* used instead when status is NONE          */
};

static const Entry kGameSlot = {dMeterButton_c::BUTTON_NONE_e, BUTTON_STATUS_NONE, nullptr};

/* Which buttons we are driving.  The setString hook and the draw flags both read
 * this, so they cannot disagree about who owns what. */
static u8 g_owned_buttons[kSlots];
static int g_owned_count = 0;
static bool g_painting = false;

static bool owns(u8 i_button) {
    for (int i = 0; i < g_owned_count; i++) {
        if (g_owned_buttons[i] == i_button) {
            return true;
        }
    }
    return false;
}

static bool painting() { return g_painting; }

/* Both slots are written every frame, and that is deliberate rather than lazy.
 *
 * setString relays the row out -- every glyph width and the centring in
 * field_0x2f4 are recomputed -- so writing only on change looks like an easy
 * win.  It is not: writing unconditionally, in a fixed order, is what makes this
 * the last writer every frame, and being last is the whole mechanism that stops
 * the label bouncing between RB and RT.
 *
 * Caching it per slot was tried and broke the hawkeye specifically, because that
 * is the one case where a slot's *button* changes mid-aim -- Zoom takes slot 0
 * and pushes Switch across -- while ownership is tracked per button.  The cache
 * then described a pane that no longer existed in that shape: the bow's row read
 * "Draw Draw", and the zoomed row started right and fell back to Switch on RT a
 * few seconds in.  If this is ever worth optimising, the check has to be against
 * what the pane actually holds, not against what we last wrote. */
static void release() { g_owned_count = 0; }

/* _execute's i_draw* parameters are declared in exactly the BUTTON_*_e order,
 * all twenty-two of them, after `this` and i_flags -- so a button code maps
 * straight onto an argument index. */
static void set_draw_flag(void* args, u8 i_button, bool i_value) {
    if (i_button < dMeterButton_c::BUTTON_NONE_e) {
        mods::arg_ref<bool>(args, 2 + (int)i_button) = i_value;
    }
}

/* Resolved one entry at a time, never two live at once: getActionString returns
 * a shared internal buffer, so holding two of its pointers gives you the second
 * string twice -- and setString then merges the slots, because their text
 * matches. */
static void resolve(const Entry& e, char* o_text) {
    if (e.status != BUTTON_STATUS_NONE) {
        dMeter2_c* meter = dMeter2Info_getMeterClass();
        dMeter2Draw_c* draw = (meter != nullptr) ? meter->getMeterDrawPtr() : nullptr;
        if (draw != nullptr) {
            std::snprintf(o_text, kTextMax, "%s", draw->getActionString(e.status, 0, nullptr));
            return;
        }
    }
    std::snprintf(o_text, kTextMax, "%s", e.text != nullptr ? e.text : "");
}

/* Call from a dMeterButton_c::_execute pre-hook. */
static void render(dMeterButton_c* btn, void* args, const Entry& i_left, const Entry& i_right) {
    if (btn == nullptr) {
        release();
        return;
    }

    const Entry* slot[kSlots] = {&i_left, &i_right};

    /* Claim the buttons we are about to write, so the game stops writing those
     * slots: two writers restart the label's alpha animation every frame and it
     * never finishes, giving a lit glyph over a half-lit label. */
    g_owned_count = 0;
    for (int i = 0; i < kSlots; i++) {
        if (slot[i]->button != dMeterButton_c::BUTTON_NONE_e) {
            g_owned_buttons[g_owned_count++] = slot[i]->button;
        }
    }

    g_painting = true;
    for (int i = 0; i < kSlots; i++) {
        if (slot[i]->button == dMeterButton_c::BUTTON_NONE_e) {
            continue;
        }
        char text[kTextMax];
        resolve(*slot[i], text);
        btn->setString(text, slot[i]->button, (u8)i, 0);
        set_draw_flag(args, slot[i]->button, true);
    }
    g_painting = false;

    /* Clearing a slot is not enough to remove a prompt: alphaAnimeButton* raises
     * the glyph pane on its own, with no reference to any slot, so a button that
     * has just lost its entry would leave a bare icon behind. */
    for (int b = 0; b < dMeterButton_c::BUTTON_NONE_e; b++) {
        bool live = false;
        for (int i = 0; i < kSlots; i++) {
            if (slot_button(btn, i) == (u8)b) {
                live = true;
            }
        }
        if (!live) {
            set_draw_flag(args, (u8)b, false);
        }
    }
}

/* Call from a dMeter2_c::checkStatus post-hook.  Gate 1: with no emphasis flag
 * the row is closed outright and nothing rendered above is ever drawn.
 * checkStatus clears R every frame while the aiming HUD is up, which is why this
 * has to run after it.  R rather than Z deliberately -- setZStatus reaches
 * drawButtonZ, which repaints the top-right cluster as a side effect. */
static void keep_open(u8 i_status) {
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link != nullptr && i_status != BUTTON_STATUS_NONE) {
        link->setItemActionButtonStatus(i_status);
    }
}


/* Exclusive ownership of the buttons prompt_row is driving.  Without this the
 * game's emphasis loop rewrites the same slots every frame and the labels never
 * finish animating up to full alpha. */
DEFINE_HOOK(&dMeterButton_c::setString, PromptRowSetString);

static HookAction on_meter_set_string_pre(ModContext*, void* args, void*, void*) {
    if (painting()) {
        return HOOK_CONTINUE;
    }
    if (owns(mods::arg<u8>(args, 2))) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

/* Every button measures its glyph as that pane's own width -- except Z, which
 * measures the span from the `zbtn` pane across to `midona`, because vanilla's Z
 * prompt draws Midna's portrait beside the RB glyph.  With the portrait hidden
 * that span is still charged to the entry: getCenterPosCalc ends with
 * `field_0x1e4[slot] += mButtonWidth[button]`, and setString centres the pair on
 * `mDisplaySpace + field_0x1e4[0] + field_0x1e4[1]`.  Measured, the RB entry was
 * billed 101.5px for a glyph that draws 39px, and the second entry was pushed
 * right by half the difference.
 *
 * Re-measuring Z the way every other button is measured has to happen here,
 * between getCenterPosCalc and the centring that reads its result. */
DEFINE_HOOK(&dMeterButton_c::getCenterPosCalc, PromptRowCenterPos);

static void on_center_pos_calc_post(ModContext*, void* args, void*, void*) {
    if (!painting() || mods::arg<u8>(args, 1) != dMeterButton_c::BUTTON_Z_e) {
        return;
    }

    dMeterButton_c* btn = mods::arg<dMeterButton_c*>(args, 0);
    J2DPane* zbtn = btn->mpButtonScreen->search(MULTI_CHAR('zbtn'));
    if (zbtn == nullptr) {
        return;
    }

    const int slot = mods::arg<int>(args, 3);
    if (slot < 0 || slot > 1) {
        return;
    }

    const f32 spanned = btn->mButtonWidth[dMeterButton_c::BUTTON_Z_e];
    const f32 actual = btn->mButtonZScale * zbtn->getWidth();
    const f32 excess = spanned - actual;

    /* field_0x1e4 only.  field_0x304 is recomputed during positioning as
     * ((buttonWidth + textWidth + 20) / 2) - (buttonWidth / 2), where the button
     * width cancels out -- so the glyph's own placement does not depend on this
     * at all, and writing to it here would be overwritten regardless. */
    btn->mButtonWidth[dMeterButton_c::BUTTON_Z_e] = actual;
    slot_claimed_width(btn, slot) -= excess;
}

/* The same phantom portrait biases where the glyph is *placed*, not just how
 * wide the entry is billed.  screenInitButton derives every button's reference
 * centre from its own pane -- except Z, whose centre is the midpoint of the
 * zbtn..midona span (d_meter_button.cpp:1279).  trans_button turns that centre
 * into the glyph's offset, so the RB glyph is drawn as if Midna were still
 * beside it, which puts it on top of its own label.
 *
 * The centre is a layout constant, computed once, so the correction is measured
 * once here and applied only for the frames the mod owns the row -- vanilla's
 * Midna prompt does draw the portrait and wants the span centre it was given. */
static f32 g_z_centre_fix = 0.0f;
static bool g_z_centre_known = false;

DEFINE_HOOK(&dMeterButton_c::screenInitButton, PromptRowScreenInit);

static void on_screen_init_button_post(ModContext*, void* args, void*, void*) {
    dMeterButton_c* btn = mods::arg<dMeterButton_c*>(args, 0);
    if (btn->mpButtonZ == nullptr) {
        return;
    }

    const f32 own = btn->mpButtonZ->getGlobalVtxCenter(false, 0).x;
    g_z_centre_fix = own - button_centre(btn, dMeterButton_c::BUTTON_Z_e);
    g_z_centre_known = true;
}

DEFINE_HOOK(&dMeterButton_c::trans_button, PromptRowTransButton);

static f32 g_z_centre_saved = 0.0f;
static bool g_z_centre_applied = false;

static HookAction on_trans_button_pre(ModContext*, void* args, void*, void*) {
    g_z_centre_applied = false;

    if (!g_z_centre_known) {
        return HOOK_CONTINUE;
    }

    const int slot = mods::arg<int>(args, 1);
    if (slot < 0 || slot > 1) {
        return HOOK_CONTINUE;
    }

    dMeterButton_c* btn = mods::arg<dMeterButton_c*>(args, 0);
    if (slot_button(btn, slot) != dMeterButton_c::BUTTON_Z_e) {
        return HOOK_CONTINUE;
    }

    if (!owns(dMeterButton_c::BUTTON_Z_e)) {
        return HOOK_CONTINUE;
    }

    g_z_centre_saved = button_centre(btn, dMeterButton_c::BUTTON_Z_e);
    button_centre(btn, dMeterButton_c::BUTTON_Z_e) += g_z_centre_fix;
    g_z_centre_applied = true;

    return HOOK_CONTINUE;
}

static void on_trans_button_post(ModContext*, void* args, void*, void*) {
    if (!g_z_centre_applied) {
        return;
    }

    dMeterButton_c* btn = mods::arg<dMeterButton_c*>(args, 0);
    button_centre(btn, dMeterButton_c::BUTTON_Z_e) = g_z_centre_saved;
    g_z_centre_applied = false;
}

/* Draw the hawkeye's zoom prompt with the D-pad instead of the C-stick.
 *
 * The HUD has no whole-pad texture: the pad is one 16x16 corner tile drawn four
 * times, mirrored into a 2x2.  The sampler does not wrap and JUTTexture's wrap
 * mode is private with no setter, so tex coords past 1.0 clamp rather than tile.
 *
 * J2DPicture::draw takes an explicit box and a per-axis mirror flag, which is
 * exactly those four draws.  The C glyph's own picture does the drawing, wearing
 * the tile's texture, so it keeps the row's fade and placement; it is hidden
 * only to stop the screen painting it stretched across its full pane.  The arrow
 * panes above and below are a separate layer and are left alone. */
static const u16 kPictureTypeID = 18;

static J2DPicture* as_picture(J2DPane* pane) {
    /* getTypeID reports 16 for a plain pane and 18 for a picture.  The methods
     * used here are virtual, so casting a plain pane dispatches through the
     * wrong vtable -- a crash, and what taking a scoped bow once did. */
    if (pane == nullptr || pane->getTypeID() != kPictureTypeID) {
        return nullptr;
    }
    return static_cast<J2DPicture*>(pane);
}

/* The glyphs are containers: c_btn's parent reports typeID 16 and holds the
 * real picture as a child alongside the arrow panes. */
static J2DPicture* find_picture(J2DPane* root) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->getTypeID() == kPictureTypeID) {
        return static_cast<J2DPicture*>(root);
    }

    JSUTree<J2DPane>* node = root->getFirstChild();
    for (int n = 0; n < 16 && node != nullptr; n++) {
        J2DPane* kid = node->getObject();
        if (kid == nullptr) {
            break;
        }
        if (kid->getTypeID() == kPictureTypeID) {
            return static_cast<J2DPicture*>(kid);
        }
        node = static_cast<JSUTree<J2DPane>*>(node->getNext());
    }
    return nullptr;
}

/* J2DScreen::search does not reach these nested children even though child
 * iteration lists them, so the tree is walked directly.  Both limits matter:
 * getNextChildPane once looped forever here and wrote a million log lines. */
static J2DPane* find_pane(J2DPane* root, u64 tag, int depth = 0) {
    if (root == nullptr || depth > 6) {
        return nullptr;
    }
    if (root->mInfoTag == tag) {
        return root;
    }

    JSUTree<J2DPane>* node = root->getFirstChild();
    for (int n = 0; n < 64 && node != nullptr; n++) {
        J2DPane* kid = node->getObject();
        if (kid == nullptr) {
            break;
        }
        J2DPane* hit = find_pane(kid, tag, depth + 1);
        if (hit != nullptr) {
            return hit;
        }
        node = static_cast<JSUTree<J2DPane>*>(node->getNext());
    }
    return nullptr;
}

/* A BLO screen loads its pictures as J2DPictureEx, which draws from its
 * material's TEV block rather than from J2DPicture::mTexture.  Assigning that
 * array is invisible -- the Ex overrides route reads through the material too,
 * so it reads back the new value while the screen keeps drawing the old art.
 * (Reaching these fields by raw offset does not work either: the header's
 * comments are GameCube layout and do not survive 64-bit pointers, which is how
 * a texture count came back as 144.  Everything here goes through virtuals.) */
static JUTTexture* picture_texture(J2DPicture* pic) {
    if (pic == nullptr || pic->getTextureCount() == 0) {
        return nullptr;
    }
    return pic->getTexture(0);
}

static bool picture_set_texture(J2DPicture* pic, JUTTexture* texture) {
    J2DMaterial* material = (pic != nullptr) ? pic->getMaterial() : nullptr;
    J2DTevBlock* tev = (material != nullptr) ? material->getTevBlock() : nullptr;
    if (tev == nullptr || texture == nullptr) {
        return false;
    }
    /* setTexture frees the texture it displaces whenever the block owns it,
     * which would destroy art other panes still draw and leave nothing to put
     * back.  Clearing the low ownership bit first makes the swap a borrow in
     * both directions: setTexture clears the bit again afterwards, so the block
     * never takes ownership of what it is lent either. */
    tev->setUndeleteFlag(kKeepDisplacedTexture);
    return tev->setTexture(0, texture);
}

/* Where a pane's art actually lands.  mGlobalBounds carries position but not
 * scale, and these panes hold their local box centred on their own origin, so
 * the drawn centre is the global corner less the local one -- which the scale
 * drops out of entirely. */
static void pane_draw_centre(const J2DPane* pane, f32* out_x, f32* out_y) {
    *out_x = pane->mGlobalBounds.i.x - pane->mBounds.i.x;
    *out_y = pane->mGlobalBounds.i.y - pane->mBounds.i.y;
}

/* Status0 bit 0x200000 is set exactly where the scope goes on -- alink sets it
 * alongside the HAWK_EYE_PUTON sound when the subjectivity proc starts -- and
 * cleared when it comes off, so on its own it says the hawkeye is up.
 *
 * Testing the equipped item as well, as this once did, quietly excluded the
 * standalone hawkeye: used on its own it leaves mEquipItem empty, so the check
 * could only ever match the bow-mounted one. */
static bool showing_zoom_prompt(const daAlink_c* link) {
    return link != nullptr && is_aiming_proc(link->mProcID) &&
           dComIfGp_checkPlayerStatus0(0, kStatusScopeOn) != 0;
}

/* ------------------------------------------------------------------
 *  Redrawing a button glyph
 *
 *  Everything below is generic: it repaints one J2DPictureEx's own draw call
 *  with whatever art you want, in that pane's coordinate space.
 *
 *  Draw from inside the pane's own draw, in local coordinates.  calcMtx folds a
 *  widescreen correction into mGlobalBounds that the draw-time matrix knows
 *  nothing about, so absolute coordinates land wrong; drawTexCoord takes local
 *  coordinates and the matrix the game just handed the pane.
 *
 *  Stay inside the pane's own box.  mClipRect is the pane's bounds and anything
 *  outside them is clipped silently.  Art that belongs elsewhere has to be drawn
 *  by the pane covering that spot, with the other pane's material bound.
 * ------------------------------------------------------------------ */
namespace glyph_art {

/* 1.0 in the fixed-point format drawTexCoord reads tex coords in. */
static const s16 kOne = 256;

/* Skipping the original draw skips the material bind and vertex format it does
 * first, and the quads come out wearing whatever texture was still bound. */
static bool bind(J2DPicture* source) {
    J2DMaterial* material = (source != nullptr) ? source->getMaterial() : nullptr;
    if (material == nullptr) {
        return false;
    }
    material->setGX();
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    return true;
}

/* A square of the bound texture mirrored into a 2x2, centred on (cx, cy) in
 * `into`'s local space.  Art built as one corner tile -- the D-pad is -- becomes
 * whole this way; a picture cannot tile on its own, and the texture's wrap mode
 * is private with no setter. */
static void draw_mirrored_quad(J2DPictureEx* into, Mtx* matrix, f32 cx, f32 cy, f32 size) {
    const f32 half = size * 0.5f;
    for (int quadrant = 0; quadrant < 4; quadrant++) {
        const bool mirror_x = (quadrant & 1) != 0;
        const bool mirror_y = (quadrant & 2) != 0;
        const s16 s0 = mirror_x ? kOne : 0;
        const s16 s1 = mirror_x ? 0 : kOne;
        const s16 t0 = mirror_y ? kOne : 0;
        const s16 t1 = mirror_y ? 0 : kOne;

        into->drawTexCoord(cx + (mirror_x ? 0.0f : -half), cy + (mirror_y ? 0.0f : -half), half,
                           half, s0, t0, s1, t0, s0, t1, s1, t1, matrix);
    }
}

}  // namespace glyph_art

/* ------------------------------------------------------------------
 *  Injecting a D-pad into a prompt
 *
 *  Ask for it once per frame for as long as you want it:
 *
 *      dpad_glyph::request(row, dMeterButton_c::BUTTON_C_e);
 *
 *  and the row's C glyph draws a D-pad instead of its own art.  Stop asking and
 *  it goes back on its own.  Any button in the row works, several at once, and
 *  the caller needs to know nothing about panes, materials or tex coords.
 *
 *  Placement is geometric by default: the pad is centred on the glyph's own box
 *  -- the same box the button's art fills -- and scaled to sit at the same
 *  visual weight as the other glyphs.  Where a button's art is itself off
 *  centre within its pane, pass a trim; kStickGlyphTrim is the measured one for
 *  the C stick, whose art sits a unit right and two down of its box.
 * ------------------------------------------------------------------ */
namespace dpad_glyph {

struct Placement {
    f32 scale;    /* fraction of the glyph's pane the pad covers */
    f32 trim_x;   /* offset from the pane's centre, in HUD units */
    f32 trim_y;
};

static const Placement kCentred = {0.72f, 0.0f, 0.0f};
static const Placement kStickGlyphTrim = {0.72f, 1.0f, 2.0f};

/* One per button in the row.  A slot holds what has to go back afterwards:
 * leaving modified state behind is what once made the item/map D-pad vanish. */
struct Slot {
    J2DPicture* pic;
    JUTTexture* original;
    JUtility::TColor black;
    JUtility::TColor white;
    Placement placement;
    u32 asked_on_frame;
    bool installed;
};

static Slot g_slots[dMeterButton_c::BUTTON_NONE_e];
static u32 g_frame = 0;

static CPaneMgr* button_pane(dMeterButton_c* row, u8 button) {
    if (row == nullptr) {
        return nullptr;
    }
    switch (button) {
    case dMeterButton_c::BUTTON_A_e: return row->mpButtonA;
    case dMeterButton_c::BUTTON_B_e: return row->mpButtonB;
    case dMeterButton_c::BUTTON_R_e: return row->mpButtonR;
    case dMeterButton_c::BUTTON_Z_e: return row->mpButtonZ;
    case dMeterButton_c::BUTTON_3D_e: return row->mpButton3D;
    case dMeterButton_c::BUTTON_C_e: return row->mpButtonC;
    case dMeterButton_c::BUTTON_S_e: return row->mpButtonS;
    case dMeterButton_c::BUTTON_X_e: return row->mpButtonX;
    case dMeterButton_c::BUTTON_Y_e: return row->mpButtonY;
    default: return nullptr;
    }
}

/* The pad's art: one 16x16 corner tile that the game draws four times, mirrored
 * into a 2x2.  It is borrowed from the item/map pad rather than copied, so the
 * tint that comes with it is the one the game already draws the pad in. */
static J2DPicture* pad_art() {
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    dMeter2Draw_c* draw = (meter != nullptr) ? meter->getMeterDrawPtr() : nullptr;
    if (draw == nullptr || draw->getMainScreenPtr() == nullptr) {
        return nullptr;
    }
    return as_picture(find_pane(draw->getMainScreenPtr(), MULTI_CHAR('juji_001')));
}

static void restore(Slot& slot) {
    if (slot.installed && slot.pic != nullptr) {
        picture_set_texture(slot.pic, slot.original);
        slot.pic->setBlackWhite(slot.black, slot.white);
    }
    slot.installed = false;
    slot.pic = nullptr;
}

/* Call once per frame, from anywhere that runs while the row is being drawn. */
static bool request(dMeterButton_c* row, u8 button, const Placement& placement = kCentred) {
    if (button >= dMeterButton_c::BUTTON_NONE_e) {
        return false;
    }

    CPaneMgr* pane = button_pane(row, button);
    J2DPicture* pic = (pane != nullptr) ? find_picture(pane->getPanePtr()) : nullptr;
    J2DPicture* art = pad_art();
    JUTTexture* tile = picture_texture(art);
    if (pic == nullptr || tile == nullptr) {
        return false;
    }

    Slot& slot = g_slots[button];
    slot.placement = placement;
    slot.asked_on_frame = g_frame;

    if (slot.installed && slot.pic == pic) {
        return true;
    }

    restore(slot); /* the row can hand out a different pane than last time */
    slot.pic = pic;
    slot.original = picture_texture(pic);
    slot.black = pic->getBlack();
    slot.white = pic->getWhite();

    if (!picture_set_texture(pic, tile)) {
        slot.pic = nullptr;
        return false;
    }
    /* The art is greyscale; its colour is the black and white the material maps
     * luminance onto, so the pad's own pair comes along with its texture. */
    pic->setBlackWhite(art->getBlack(), art->getWhite());
    slot.installed = true;
    return true;
}

/* Anything not asked for recently goes back.  The margin is deliberate: this
 * runs on the row's draw, and a caller's own hook may run either side of it, so
 * a slot is only dropped once it has gone properly quiet. */
static void expire_unrequested() {
    g_frame++;
    for (int button = 0; button < dMeterButton_c::BUTTON_NONE_e; button++) {
        Slot& slot = g_slots[button];
        if (slot.installed && g_frame - slot.asked_on_frame > 2) {
            restore(slot);
        }
    }
}

static Slot* slot_for(J2DPicture* pic) {
    for (int button = 0; button < dMeterButton_c::BUTTON_NONE_e; button++) {
        if (g_slots[button].installed && g_slots[button].pic == pic) {
            return &g_slots[button];
        }
    }
    return nullptr;
}

}  // namespace dpad_glyph

DEFINE_HOOK(&dMeterButton_c::draw, PromptRowDraw);

DEFINE_HOOK(&J2DPictureEx::drawSelf, PictureDrawSelf);

static HookAction on_picture_draw_self_pre(ModContext*, void* args, void*, void*) {
    J2DPictureEx* pic = mods::arg<J2DPictureEx*>(args, 0);
    dpad_glyph::Slot* slot = (pic != nullptr) ? dpad_glyph::slot_for(pic) : nullptr;
    if (slot == nullptr) {
        return HOOK_CONTINUE; /* every other picture in the game */
    }

    if (!glyph_art::bind(pic)) {
        return HOOK_CONTINUE;
    }

    const JGeometry::TBox2<f32>& box = pic->mBounds;
    glyph_art::draw_mirrored_quad(pic, mods::arg<Mtx*>(args, 3),
                                  (box.i.x + box.f.x) * 0.5f + slot->placement.trim_x,
                                  (box.i.y + box.f.y) * 0.5f + slot->placement.trim_y,
                                  box.getWidth() * slot->placement.scale);
    return HOOK_SKIP_ORIGINAL;
}

static HookAction on_meter_button_draw_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));

    /* Ahead of the aiming guard on purpose: the glyph has to be put back, and
     * the guard below returns before the request could lapse. */
    dpad_glyph::expire_unrequested();
    if (showing_zoom_prompt(link)) {
        dpad_glyph::request(mods::arg<dMeterButton_c*>(args, 0), dMeterButton_c::BUTTON_C_e,
                            dpad_glyph::kStickGlyphTrim);
    }

    if (link == nullptr || !is_aiming_proc(link->mProcID) ||
        z_prompt_status(link->mEquipItem) == BUTTON_STATUS_NONE)
    {
        return HOOK_CONTINUE;
    }

    /* Vanilla's Z prompt is "press RB to call Midna", so the row draws her
     * portrait (`midona`, on dMeterButton_c's own screen -- not the top-right
     * one) beside the RB glyph.  With the slot repurposed for an item action the
     * portrait is wrong, while the glyph is exactly right, so only the portrait
     * goes.  Hiding it is also what makes the two width corrections above
     * necessary: the layout still measures the space it would have taken. */
    dMeterButton_c* button = mods::arg<dMeterButton_c*>(args, 0);





    if (button->mpMidona != nullptr) {
        button->mpMidona->hide();
    }

    return HOOK_CONTINUE;
}


/* Register the machinery.  Call once from mod_initialize, before any render.
 *
 * The Z entries are here rather than left to the caller because reusing that
 * slot means hiding Midna's portrait, and hiding it is exactly what invalidates
 * the two layout constants measured across it.  A caller who had to wire those
 * up by hand would be reimplementing the awkward half of this. */

static ModResult install() {
    ModResult r = mods::hook_add_pre<PromptRowSetString>(svc_hook, on_meter_set_string_pre);
    if (r != MOD_OK) return r;

    r = mods::hook_add_post<PromptRowCenterPos>(svc_hook, on_center_pos_calc_post);
    if (r != MOD_OK) return r;

    r = mods::hook_add_post<PromptRowScreenInit>(svc_hook, on_screen_init_button_post);
    if (r != MOD_OK) return r;

    r = mods::hook_add_pre<PromptRowTransButton>(svc_hook, on_trans_button_pre);
    if (r != MOD_OK) return r;

    r = mods::hook_add_post<PromptRowTransButton>(svc_hook, on_trans_button_post);
    if (r != MOD_OK) return r;

    r = mods::hook_add_pre<PromptRowDraw>(svc_hook, on_meter_button_draw_pre);
    if (r != MOD_OK) return r;

    return mods::hook_add_pre<PictureDrawSelf>(svc_hook, on_picture_draw_self_pre);
}

}  // namespace prompt_row

/* The row has to be emphasised by *something* or it is never drawn: dMeter2_c
 * closes it outright when no isEmphasis* flag is set, and then no amount of slot
 * painting reaches the screen.  R is the one to use.  dMeter2_c::checkStatus
 * clears it every frame while the aiming HUD is up, so it is re-asserted here,
 * afterwards.
 *
 * R is safe in a way Z is not.  drawButtonR only shows a pane and records the
 * emphasis flag; drawButtonZ copies the label text into mpXYText[i][2] and
 * rearranges the whole top-right cluster.  Emphasising R is also what vanilla
 * itself does for the boomerang's lock and the hawkeye's switch, so this is the
 * same call the game makes, at the same moments. */
/* Zoom on the D-pad, and only on the D-pad.
 *
 * The scope's zoom reads the right stick's Y each frame -- positive zooms in,
 * negative out -- and nothing else in the hawkeye reads the D-pad, so a held
 * direction is fed in as a full deflection of that axis.  The axis is taken
 * over outright rather than only written when a direction is held: leaving it
 * alone otherwise would keep the stick zooming alongside the D-pad, which is
 * the binding this replaces.
 *
 * It is handed straight back afterwards.  Within subjectCamera the zoom is the
 * only reader of that axis, but the value persists until the next frame reads
 * the pad again, so restoring it keeps the override from leaking into anything
 * else that looks at the stick later in the frame. */
DEFINE_HOOK(&dCamera_c::subjectCamera, SubjectCamera);

static bool g_stick_y_held = false;
static f32 g_stick_y_saved = 0.0f;

static HookAction on_subject_camera_pre(ModContext*, void* args, void*, void*) {
    dCamera_c* camera = mods::arg<dCamera_c*>(args, 0);
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));

    g_stick_y_held = false;
    if (camera == nullptr || !prompt_row::showing_zoom_prompt(link)) {
        return HOOK_CONTINUE;
    }

    const bool up = mDoCPd_c::getHoldUp(0) != 0;
    const bool down = mDoCPd_c::getHoldDown(0) != 0;

    g_stick_y_saved = camera->mPadInfo.mCStick.mLastPosY;
    g_stick_y_held = true;
    camera->mPadInfo.mCStick.mLastPosY = (up == down) ? 0.0f : (up ? 1.0f : -1.0f);
    return HOOK_CONTINUE;
}

static void on_subject_camera_post(ModContext*, void* args, void*, void*) {
    if (!g_stick_y_held) {
        return;
    }
    g_stick_y_held = false;

    dCamera_c* camera = mods::arg<dCamera_c*>(args, 0);
    if (camera != nullptr) {
        camera->mPadInfo.mCStick.mLastPosY = g_stick_y_saved;
    }
}

DEFINE_HOOK(&dMeter2_c::checkStatus, MeterCheckStatus);

static void on_meter_check_status_post(ModContext*, void*, void*, void*) {
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr || !is_aiming_proc(link->mProcID)) {
        return;
    }

    prompt_row::keep_open(rt_prompt_status(link));
}

/* The row is painted directly rather than through the R and Z *statuses*.
 *
 * dComIfGp_setZStatus is not an option: dMeter2_c::moveButtonZ feeds it to
 * dMeter2Draw_c::drawButtonZ, and that one function owns both the bottom row's
 * emphasis flag and the entire top-right cluster -- it rewrites mpXYText[i][2],
 * shows mpTextXY[2] and hides mpButtonMidona.  Lighting the row that way
 * repaints the corner every frame, which is not ours to touch.
 *
 * Both slots are assigned every frame, in a fixed order, by this one owner.
 * Anything less lets changeArrowType and the Z prompt write the same label to
 * different buttons, and leaves stale entries rendering as a second prompt.
 *
 * setString is the game's own API for this -- it is what the emphasis loop
 * calls -- so glyph widths and centring are recomputed as the row expects. */
DEFINE_HOOK(&dMeterButton_c::_execute, MeterButtonExecute);

static HookAction on_meter_button_execute_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr || !is_aiming_proc(link->mProcID)) {
        prompt_row::release();
        return HOOK_CONTINUE;
    }

    const u8 rt_status = rt_prompt_status(link);
    if (rt_status == BUTTON_STATUS_NONE) {
        prompt_row::release();
        return HOOK_CONTINUE;
    }

    const u8 z_status = z_prompt_status(link->mEquipItem);

    dMeterButton_c* btn = mods::arg<dMeterButton_c*>(args, 0);

    prompt_row::Entry left = prompt_row::kGameSlot;
    prompt_row::Entry right = prompt_row::kGameSlot;

    if (prompt_row::slot_button(btn, 0) == dMeterButton_c::BUTTON_C_e) {
        /* The hawkeye's "Zoom" hint holds slot 0 while the scope is up, and
         * three prompts do not fit in two slots.  Zoom stays and the fire label
         * goes: it names the button already being held to aim, while the rebound
         * button is the one a player cannot work out for themselves. */
        right = (z_status != BUTTON_STATUS_NONE)
                    ? prompt_row::Entry{dMeterButton_c::BUTTON_Z_e, z_status, nullptr}
                    : prompt_row::Entry{dMeterButton_c::BUTTON_R_e, rt_status, nullptr};
    } else if (z_status != BUTTON_STATUS_NONE) {
        /* RB first, so the row reads "<action> (RB)  <fire> (RT)". */
        left = prompt_row::Entry{dMeterButton_c::BUTTON_Z_e, z_status, nullptr};
        right = prompt_row::Entry{dMeterButton_c::BUTTON_R_e, rt_status, nullptr};
    } else {
        left = prompt_row::Entry{dMeterButton_c::BUTTON_R_e, rt_status, nullptr};
    }

    prompt_row::render(btn, args, left, right);

    return HOOK_CONTINUE;
}

/* The Z button is suppressed on purpose while an item is being aimed.
 * dMeter2_c::checkStatus sets mStatus |= 0x1000 whenever getItemSubject() is
 * true, and alphaAnimeButtonZ treats that bit as "hide" -- which is exactly why
 * Z is free for us to reuse in the first place.  alphaAnimeButtonR does not test
 * it, so the RT half of the row drew and the RB half did not, no matter how
 * correctly the slot was filled: at d_meter_button.cpp:98 a false draw_z drops
 * the entry's *text* as well as its glyph, so "Lock" was written into the slot
 * and then never rendered.
 *
 * 0x1000 also gates A, B, X, Y, 3D, C and S, so clearing it on mStatus would
 * bring back every prompt aiming is meant to hide.  Clear it on this one call's
 * own argument instead: only the Z button's decision changes, and only while we
 * actually want the prompt up. */
DEFINE_HOOK(&dMeterButton_c::alphaAnimeButtonZ, AlphaAnimeButtonZ);

static HookAction on_alpha_anime_button_z_pre(ModContext*, void* args, void*, void*) {
    if (!prompt_row::owns(dMeterButton_c::BUTTON_Z_e)) {
        return HOOK_CONTINUE;
    }

    /* 0x80 is the lock-on camera; it is normally false here, but a Z-target held
     * into aiming would otherwise blank the row again. */
    mods::arg_ref<u32>(args, 1) &= ~(kMeterHideDuringAim | kMeterHideLockOnCamera);

    return HOOK_CONTINUE;
}

/* changeArrowType is the game's own gate on whether a switch is even available:
 * it early-returns on the plain bow and on a spent bomb-arrow quiver, and only
 * gets as far as setting SWITCH when the action is real.  That makes reaching
 * this hook the signal, and nothing here writes a status -- the row is painted
 * in one place, on _execute. */
DEFINE_HOOK(&daAlink_c::changeArrowType, ChangeArrowType);

static void on_change_arrow_type_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (!is_bow_family(link->mEquipItem)) {
        return;
    }

    if (dComIfGp_getRStatus() == BUTTON_STATUS_SWITCH) {
        g_bow_arrow_switch_offered = true;
    }
}

/* ------------------------------------------------------------------
 *  Bow and slingshot reticle
 *
 *  The game already computes a sight position for the bow -- setBowSight runs
 *  checkSightLine and stores the result -- and then calls offDrawFlg() in both
 *  of its branches, so it is never drawn.  The boomerang's equivalent differs by
 *  one line: it calls onDrawFlg().  So this needs no new rendering, just the
 *  draw flag turned back on after the original has positioned the sight.
 *
 *  The slingshot comes along for free: it runs the same bow procs, which is why
 *  setBowOrSlingStatus exists to pick between two player-status bits.
 * ------------------------------------------------------------------ */
DEFINE_HOOK(&daAlink_c::setItemActionButtonStatus, ItemActionButtonStatus);

static HookAction on_item_action_button_status_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link != nullptr && mods::arg<u8>(args, 1) == BUTTON_STATUS_LOCK &&
        link->mEquipItem == dItemNo_BOOMERANG_e)
    {
        g_boomerang_lock_seen_this_frame = true;

        /* And stop the game putting that prompt on R, where this mod needs the
         * throw label.  setBoomerangSight runs after the row is painted, so its
         * write lands last and takes the slot -- which is why Lock and Throw
         * were never on screen together: with a target, R said Lock.
         *
         * Only the prompt is dropped.  This function is setRStatusEmphasys and
         * nothing else; the lock itself comes from itemActionTrigger, which is
         * spActionTrigger on BTN_R and is not touched. */
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daAlink_c::setBowSight, BowSight);

static void on_bow_sight_post(ModContext*, void* args, void*, void*) {
    if (!cfg_bow_reticle()) {
        return;
    }

    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    /* Mirror the original's own condition rather than inventing one: it only
     * computes a position while the shot is drawn, and it deliberately skips the
     * hawkeye's zoomed state (player status 0x200000), which draws its own
     * scope.  Outside that branch mSight holds a stale position. */
    if (link->checkBowChargeWaitAnime() && !dComIfGp_checkPlayerStatus0(0, kStatusScopeOn)) {
        link->mSight.onDrawFlg();
    }
}

/* R advertises the shot while the bow is drawn.  changeArrowType may overwrite
 * this later in the frame; its own post-hook puts it back. */

/* ------------------------------------------------------------------
 *  Release does not fire; the next click does
 *
 *  A press and the release that ends it are one gesture.  Every item's fire
 *  decision asks the same question -- "is the button still down?" -- so the
 *  only thing this mod decides is whether a release ended an aim or a shot.
 *
 *  The fire is suppressed by default for an aiming item, and lifted for one
 *  gesture: a press made while Link is already aiming.  That is what makes
 *  "click again to fire" work, and it is the only way a deliberate shot leaves.
 *  A click from idle therefore always opens the aim, whatever is in hand.
 *
 *  Z-targeting is exempt and keeps vanilla firing; see fire_is_suppressed.
 *
 *  Nothing here enters a proc, writes a timer, or touches an animation.  A
 *  first-person click reaches the aim on the game's own schedule: the
 *  suppressed release leaves the item pending, and checkNextActionHookshot
 *  walks mFastShotTime down to zero by itself -- that countdown is gated on
 *  checkHookshotWait (the item is stowed or pending), not on the button.
 * ------------------------------------------------------------------ */

/* Two facts about the press in progress, both settled the frame it goes down.
 * Neither asks what the item is: whichever hook reads them is installed on one
 * item's own fire decision, so the call site has already answered that. */
/* Sampled once a frame in track_firing_gesture rather than asked at the
 * decision.  checkAttentionLock is mAttention->Lockon() with no null check, and
 * the item decisions run during scene creation, before that pointer exists --
 * asking there is an access violation on load. */
static bool g_player_is_targeting = false;
static bool g_press_began_while_aiming = false;
static u8 g_gesture_buttons = 0;

static const char* gesture_name() {
    return g_press_began_while_aiming ? "shot" : "aiming";
}

/* A verdict is made here and spent in on_scope_post, when the decision it was
 * made for has run.  Nothing counts frames: this runs whenever setStickData
 * does, and the decision runs whenever the item's action does, and neither
 * needs to know the other's cadence. */
static bool player_is_aiming(daAlink_c* link) {
    return is_aiming_proc(link->mProcID) || link->checkAttentionLock();
}

static void track_firing_gesture(daAlink_c* link) {
    g_player_is_targeting = link->checkAttentionLock();
    const bool aiming = player_is_aiming(link);

    const u8 pressed = (u8)(link->mItemTrigger & item_button_mask());
    if (pressed != 0) {
        g_gesture_buttons = pressed;
        g_press_began_while_aiming = aiming;
    }
}

/* A shot the player asked for always reaches the game.  What else gets through
 * differs by how the item aims, and that is settled by which decision we are
 * standing in rather than by inspecting mEquipItem -- unreliable at exactly
 * this moment, holding 0x103 while the clawshot is on its way out.
 *
 * first person   nothing else fires.  A press from idle is a click-to-aim, and
 *                its suppressed release leaves the item pending, so
 *                checkNextActionHookshot walks mFastShotTime to zero and the
 *                game enters its own subject proc unaided.
 *
 * over shoulder  a press that has not reached the aim is still a possible quick
 *                throw, which is vanilla's business and not ours. */
/* One rule, every item: a release fires only if its press began while Link was
 * already aiming.  A click from idle therefore always opens the aim, whatever is
 * in hand.
 *
 * There used to be a second rule for the over-shoulder items, which let a press
 * that had not reached the aim yet still throw -- vanilla's quick throw.  It
 * stopped mattering once those items reached the aim promptly, because the
 * release then landed after the aim and the window was already shut.  That made
 * their behaviour depend on winning a race, and the ball and chain lost it the
 * moment its wind-up was kept.  A rule that only holds while the timing happens
 * to cooperate is not a rule.
 *
 * Z-targeting is exempt and keeps vanilla firing.  Suppression exists to protect
 * a first-person aim from the release that would end it, and targeting has no
 * such aim to protect: Link stays on screen, the target is picked for him, and a
 * click there is meant to fire.  Suppressing it costs a click -- one to bring
 * the item up, another to shoot. */
static bool fire_is_suppressed() {
    if (g_player_is_targeting) {
        return false;
    }
    return !g_press_began_while_aiming;
}

DEFINE_HOOK(&daAlink_c::checkNextActionHookshot, NextActionHookshot);
DEFINE_HOOK(&daAlink_c::checkNextActionBoomerang, NextActionBoomerang);
DEFINE_HOOK(&daAlink_c::checkNextActionCopyRod, NextActionCopyRod);

/* The claw goes first person as soon as it is up, instead of after the item's
 * transition timer.
 *
 * checkNextActionHookshot takes the aim branch once mFastShotTime reaches zero,
 * and setFastShotTimer loads that from mItemFPTransitionTimer -- around a
 * second.  The wait exists so a quick shot can still be fired on the way; with
 * the shot suppressed there is nothing left to wait for.
 *
 * Clearing that field is what failed twice before, because setHookshotReadyAnime
 * reads the same field to choose whether the wait clip plays or is parked, and a
 * parked clip is the arm-down pose.  It cannot see this one: that call sits
 * behind !checkHookshotAnime(), so it runs only while the clip is absent, and
 * this runs only once the clip is present.  The animation choice is already made
 * and cannot be revisited by the time this fires.
 *
 * Holding L is third-person item use and keeps vanilla timing. */
/* Every aiming item is built the same way: count the transition timer down, set
 * the ready clip and reload the timer the first time through, then go first
 * person once the timer is zero.  So they all wait out mItemFPTransitionTimer
 * before the aim appears, and they can all skip it the same way.
 *
 * ready_clip_is_up is the item's own "my wait animation is loaded" test.  It is
 * what makes this safe: set<Item>ReadyAnime sits behind !check<Item>Anime(), so
 * it runs only while the clip is absent and this runs only once it is present.
 * The clawshot's picks its animation speed from this very field, and clearing it
 * too early is what froze the arm on frame zero twice before.
 *
 * Targeting is left alone -- it keeps vanilla timing and vanilla firing. */
static void skip_item_transition_timer(daAlink_c* link, bool ready_clip_is_up) {
    if (link == nullptr || link->mFastShotTime == 0 || g_player_is_targeting) {
        return;
    }
    if (ready_clip_is_up) {
        link->mFastShotTime = 0;
    }
}

static HookAction on_next_action_hookshot_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    skip_item_transition_timer(link, link != nullptr && link->checkHookshotAnime());
    return HOOK_CONTINUE;
}

static HookAction on_next_action_boomerang_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    skip_item_transition_timer(link, link != nullptr && link->checkBoomerangAnime());
    return HOOK_CONTINUE;
}

static HookAction on_next_action_copy_rod_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    skip_item_transition_timer(link, link != nullptr && link->checkCopyRodAnime());
    return HOOK_CONTINUE;
}

/* The ball and chain is deliberately not in this list.  For the other items the
 * transition timer is dead time before the aim, but setIronBallReadyAnime
 * spends it: it sets a 21-frame ANM_IRONBALL_ATTACK wind-up, puts IBTHROW on
 * the upper channel and plays the swing grunt, and setFastShotTimer is called
 * straight afterwards to cover exactly that.  Skipping the timer skips the
 * overhead swing -- the aim still works, so it reads as a missing animation
 * rather than as broken timing. */

DEFINE_HOOK(&daAlink_c::setStickData, SetStickData);

static void on_stick_data_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    /* setStickData opens the frame. */
    g_movement_calls = 0;

    /* Latched for the length of the aim, not per frame.  changeArrowType is only
     * reached from the bow's wait and shoot branches, so a per-frame flag went
     * false for every frame the arrow was drawn. */
    if (!is_aiming_proc(link->mProcID)) {
        g_bow_arrow_switch_offered = false;
        g_boomerang_lock_seen_this_frame = false;
    }

    /* Publish last frame's answer and start a fresh one. */
    g_boomerang_lock_offered = g_boomerang_lock_seen_this_frame;
    g_boomerang_lock_seen_this_frame = false;

    /* Aliasing happens before anything reads the button, this mod included.  An
     * alias that lands after the gesture is tracked is not an alias: the press
     * stays invisible to our own fire decision, which then suppresses the shot
     * the player just asked for.
     *
     * RT is an alias for the item's button while aiming.  The bit mirrored is
     * the one the player actually pressed, not `1 << mSelectItemId`: those
     * disagree whenever an input mod routes an item onto another button, and
     * mSelectItemId is assigned in checkItemChangeAutoAction, which runs after
     * this.  Aliasing onto the wrong item button reads as an item change, and
     * Link puts the item away and draws his sword. */
    if (is_aiming_proc(link->mProcID) && g_gesture_buttons != 0) {
        if ((link->mItemButton & daAlink_c::BTN_R) != 0) {
            link->mItemButton |= g_gesture_buttons;
        }
        if ((link->mItemTrigger & daAlink_c::BTN_R) != 0) {
            link->mItemTrigger |= g_gesture_buttons;
        }
    }

    /* Having aliased R, take it away.  An alias that only adds leaves the real
     * bit set, and itemActionTrigger is spActionTrigger on BTN_R -- so one press
     * both threw the boomerang and locked on, the lock clearing instantly behind
     * the throw.  It only showed if you held R without releasing.
     *
     * Cleared for the two items whose action was moved to Z, because those are
     * the ones with somewhere else to press; the Z remap below puts BTN_R back
     * for them, and that is now the only way to reach the lock or the switch. */
    if (is_aiming_proc(link->mProcID) && action_moved_to_z(link->mEquipItem)) {
        link->mItemButton &= (u8)~daAlink_c::BTN_R;
        link->mItemTrigger &= (u8)~daAlink_c::BTN_R;
    }

    /* The boomerang already used R while aiming, for its multi-target lock, so
     * that lock moves to Z -- which is otherwise dead here, being read only by
     * midnaTalkTrigger, and Midna cannot be called mid-aim.  This is a per-frame
     * remap of a button the player really pressed, which is what setStickData
     * itself does when it builds mItemButton from the pad. */
    if (action_moved_to_z(link->mEquipItem)) {
        const bool z_held = (link->mItemButton & daAlink_c::BTN_Z) != 0;
        const bool z_pressed = (link->mItemTrigger & daAlink_c::BTN_Z) != 0;

        if (z_held) link->mItemButton |= (u8)daAlink_c::BTN_R;
        if (z_pressed) link->mItemTrigger |= (u8)daAlink_c::BTN_R;
        link->mItemButton &= (u8)~daAlink_c::BTN_Z;
        link->mItemTrigger &= (u8)~daAlink_c::BTN_Z;
    }

    track_firing_gesture(link);

}

/* Scoped override: the bit exists only inside one hooked call.  Nesting is real
 * -- checkUpperItemActionHookshot calls checkNextActionHookshot internally -- so
 * what each level added is stacked and undone in order. */
static const int kMaxOverrideDepth = 8;
static u8 g_added_bits[kMaxOverrideDepth];
static int g_override_depth = 0;

static HookAction hold_button_for_decision(void* args, bool suppress) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    u8 bits_we_added = 0;

    if (suppress) {
        /* Set the bit the decision will actually read.  itemButton() tests
         * `1 << mSelectItemId`, not the physical bit the player pressed; the two
         * disagree when an input mod routes an item onto another button.  Here
         * mSelectItemId is current, because checkItemChangeAutoAction runs
         * earlier in checkNextActionFromButton -- being at the decision point is
         * what makes the right bit unambiguous. */
        const u8 wanted = (u8)(1 << link->mSelectItemId);
        bits_we_added = (u8)(wanted & ~link->mItemButton);
        link->mItemButton |= bits_we_added;
    }

    if (g_override_depth < kMaxOverrideDepth) {
        g_added_bits[g_override_depth] = bits_we_added;
    } else if (bits_we_added != 0) {
        link->mItemButton &= (u8)~bits_we_added; /* out of stack: undo rather than leak */
    }
    g_override_depth++;

    return HOOK_CONTINUE;
}

static HookAction on_item_decision_pre(ModContext*, void* args, void*, void*) {
    return hold_button_for_decision(args, fire_is_suppressed());
}

static void on_scope_post(ModContext*, void* args, void*, void*) {
    if (g_override_depth <= 0) {
        return;
    }
    g_override_depth--;

    if (g_override_depth >= kMaxOverrideDepth) {
        return;
    }

    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    const u8 bits_we_added = g_added_bits[g_override_depth];
    if (bits_we_added != 0) {
        link->mItemButton &= (u8)~bits_we_added;
    }

    /* The decision has now run.  If it ran with the button genuinely up then the
     * release has been seen, and whatever the player asked for has been asked:
     * the verdict is spent.  Holding it any longer fires the item again the
     * moment it returns to its pending state with the button still up. */
    /* No depth test here: a nested level still has the outer level's bit set, so
     * the button only reads genuinely up at the outermost call. */
    if ((link->mItemButton & item_button_mask()) == 0) {
        g_press_began_while_aiming = false;
    }
}

DEFINE_HOOK(&daAlink_c::checkUpperItemActionBoomerang, UpperItemBoomerang);
DEFINE_HOOK(&daAlink_c::checkUpperItemActionCopyRod,   UpperItemCopyRod);
DEFINE_HOOK(&daAlink_c::checkUpperItemActionIronBall,  UpperItemIronBall);
DEFINE_HOOK(&daAlink_c::checkUpperItemActionHookshot,  UpperItemHookshot);
DEFINE_HOOK(&daAlink_c::procIronBallSubject,           IronBallSubject);
DEFINE_HOOK(&daAlink_c::checkAimContext,               AimContext);

/* ------------------------------------------------------------------
 *  Mods panel
 * ------------------------------------------------------------------ */
static ModResult on_build_panel(ModContext* ctx, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(ctx, panel, "Item Aiming");

    UiControlDesc reticle = UI_CONTROL_DESC_INIT;
    reticle.kind = UI_CONTROL_TOGGLE;
    reticle.label = "Bow &amp; slingshot reticle";
    reticle.help_rml = "Shows an aiming reticle while drawing the bow or slingshot. The game already tracks where the shot will go; vanilla just never draws it. Hidden while the hawkeye is zoomed, which has its own scope.";
    reticle.binding = UI_BINDING_CONFIG_VAR;
    reticle.config_var = g_cfg_bow_reticle;
    svc_ui->pane_add_control(ctx, panel, &reticle, nullptr);

    UiControlDesc compat = UI_CONTROL_DESC_INIT;
    compat.kind = UI_CONTROL_TOGGLE;
    compat.label = "Modern Combat compatibility";
    compat.help_rml = "Enable if an input mod puts an item on the B button. Vanilla only assigns items to X and Y, and B is the sword, so this is off by default.";
    compat.binding = UI_BINDING_CONFIG_VAR;
    compat.config_var = g_cfg_combat_compat;
    svc_ui->pane_add_control(ctx, panel, &compat, nullptr);

    return MOD_OK;
}

/* ------------------------------------------------------------------
 *  Entry points
 * ------------------------------------------------------------------ */
extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;

    ConfigVarDesc compat = CONFIG_VAR_DESC_INIT;
    compat.name = "combat_compat";
    compat.type = CONFIG_VAR_BOOL;
    compat.default_bool = false;
    r = svc_config->register_var(mod_ctx, &compat, &g_cfg_combat_compat);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var combat_compat");

    ConfigVarDesc reticle = CONFIG_VAR_DESC_INIT;
    reticle.name = "bow_reticle";
    reticle.type = CONFIG_VAR_BOOL;
    reticle.default_bool = true;
    r = svc_config->register_var(mod_ctx, &reticle, &g_cfg_bow_reticle);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var bow_reticle");

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = on_build_panel;
    r = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (r != MOD_OK) return mods::set_error(error, r, "register_mods_panel");

    r = mods::hook_add_pre<SpeedAndAngleNormal>(svc_hook, on_speed_and_angle_normal_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "setSpeedAndAngleNormal pre");

    r = mods::hook_add_pre<SetBodyAngleToCamera>(svc_hook, on_set_body_angle_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "setBodyAngleToCamera pre");

    r = mods::hook_add_post<CheckNextAction>(svc_hook, on_check_next_action_post);
    if (r != MOD_OK) return mods::set_error(error, r, "checkNextAction post");

    r = mods::hook_add_post<SetBodyAngleToCamera>(svc_hook, on_set_body_angle_post);
    if (r != MOD_OK) return mods::set_error(error, r, "setBodyAngleToCamera post");


    r = prompt_row::install();
    if (r != MOD_OK) return mods::set_error(error, r, "prompt_row::install");

    r = mods::hook_add_post<MeterCheckStatus>(svc_hook, on_meter_check_status_post);
    if (r != MOD_OK) return mods::set_error(error, r, "dMeter2::checkStatus post");

    r = mods::hook_add_pre<MeterButtonExecute>(svc_hook, on_meter_button_execute_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "dMeterButton::_execute pre");

    r = mods::hook_add_pre<AlphaAnimeButtonZ>(svc_hook, on_alpha_anime_button_z_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "alphaAnimeButtonZ pre");

    r = mods::hook_add_pre<SubjectCamera>(svc_hook, on_subject_camera_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "hook subjectCamera");

    r = mods::hook_add_post<SubjectCamera>(svc_hook, on_subject_camera_post);
    if (r != MOD_OK) return mods::set_error(error, r, "hook subjectCamera post");

    r = mods::hook_add_post<ChangeArrowType>(svc_hook, on_change_arrow_type_post);
    if (r != MOD_OK) return mods::set_error(error, r, "changeArrowType post");

    r = mods::hook_add_post<BowSight>(svc_hook, on_bow_sight_post);
    if (r != MOD_OK) return mods::set_error(error, r, "setBowSight post");


    r = mods::hook_add_post<SetStickData>(svc_hook, on_stick_data_post);
    if (r != MOD_OK) return mods::set_error(error, r, "setStickData");

    /* Every scoped pre must have its post or the bit leaks. */
    r = mods::hook_add_pre<UpperItemBoomerang>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemBoomerang pre");
    r = mods::hook_add_post<UpperItemBoomerang>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemBoomerang post");

    r = mods::hook_add_pre<UpperItemCopyRod>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemCopyRod pre");
    r = mods::hook_add_post<UpperItemCopyRod>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemCopyRod post");

    r = mods::hook_add_pre<UpperItemIronBall>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemIronBall pre");
    r = mods::hook_add_post<UpperItemIronBall>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemIronBall post");

    r = mods::hook_add_pre<ItemActionButtonStatus>(svc_hook, on_item_action_button_status_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "hook setItemActionButtonStatus pre");

    r = mods::hook_add_pre<NextActionHookshot>(svc_hook, on_next_action_hookshot_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "hook checkNextActionHookshot pre");

    r = mods::hook_add_pre<NextActionBoomerang>(svc_hook, on_next_action_boomerang_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "hook checkNextActionBoomerang pre");

    r = mods::hook_add_pre<NextActionCopyRod>(svc_hook, on_next_action_copy_rod_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "hook checkNextActionCopyRod pre");

    r = mods::hook_add_pre<UpperItemHookshot>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemHookshot pre");
    r = mods::hook_add_post<UpperItemHookshot>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "upperItemHookshot post");

    r = mods::hook_add_pre<IronBallSubject>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "procIronBallSubject pre");
    r = mods::hook_add_post<IronBallSubject>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "procIronBallSubject post");

    r = mods::hook_add_pre<AimContext>(svc_hook, on_item_decision_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "checkAimContext pre");
    r = mods::hook_add_post<AimContext>(svc_hook, on_scope_post);
    if (r != MOD_OK) return mods::set_error(error, r, "checkAimContext post");

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
