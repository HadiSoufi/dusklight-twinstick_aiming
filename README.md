# Twinstick Aiming
## Part of a series of mods for [Dusklight](https://twilitrealm.dev/) attempting to modernize it

### Description
_Twilight Princess_ has, overall, aged pretty well, but the way it handles first-person controls and aiming with it's items has not. There's a stiffness inherent to equipping an item, holding a button to enter aiming mode, then being locked in place and being forced to use the movement controls to manipulate the camera. This mod aims to fix that by unlocking movement while aiming, mapping camera controls to the right stick, and allowing RT to be used to fire while aiming. A few other control & HUD changes had to follow to accommodate the new design, and I'm very satisfied with the results.

If that sounds good to you, you can stop reading here and download the mod via the installation instructions below.

### Some More Detail
Unlocking movement and enabling right-stick camera controls while aiming was pretty straightforward, but forces Hawkeye (and it's bow-equipped equivalent) to have it's zoom controls re-mapped to the D-Pad. Adding RT as a generic Fire button then forced Hawkeye + Bow & Boomerang to have their respective Swap and Lock functions re-mapped from RT to Z.

In development, I really felt the clumsiness of using these items- they default to quickfire on use, which, in the context of standard gameplay, makes no sense. _Twilight Princess_ is a lock-on driven game, Link's direction at any given moment is usually unreliable for aiming. So, in vanilla, pressing the item button would fire it off in whatever direction he was currently facing & holding it would begin aiming, but now, a single click of the item button begins aiming. Of course, if he's locked on, quickfire is preserved, but otherwise, I found that this new way feels much better.

### Installation
Make sure you're on a version of Dusklight that supports mods- as of right now, you have to compile from source, but they should be available in the next numbered version. Once you've got that set up & have launched at least once, copy [twinstick_aiming.dusk](https://github.com/HadiSoufi/dusklight-twinstick_aiming/blob/main/twinstick_aiming.dusk) into `%APPDATA%\TwilitRealm\Dusklight\mods\`. It will then appear in the Mod menu & be enabled by default.

**Note:** If you're using [Modern Combat](https://github.com/HadiSoufi/dusklight-moderncombat), you'll have to enable compatibility for it in the _Twinstick Aiming_ mod menu or else certain features won't work properly.
