ZMenu WaW v1.0 - trainer menu for Call of Duty: World at War (campaign / solo zombies)
======================================================================================

Install
    Download ZMenuWaW-v1.0.zip from the Releases page and extract its files into the game folder,
    next to CoDWaW.exe. Built against the 1.0 campaign exe. Single player only.

Installed files (all in the game folder, next to CoDWaW.exe):
    d3d9.dll        the menu
    ZMenuWaW.gsc    game script half (spawning, explosive bullets, god mode, ammo...)
    ZMenuWaW.ini    key bindings and settings
    ZMenuWaW.log    written on every start, look here first if something is off

Just start CoDWaW.exe as usual. Multiplayer (CoDWaWmp.exe) is left untouched on purpose.

Controls
    F7                      open / close
    Numpad 8 / 2            up / down             (or arrow keys)
    Numpad 4 / 6            change a value        (or left / right)
    Numpad 5                select / toggle       (or Enter). On a value: reset to default
    Numpad 0                back                  (or Backspace)
Rebind them in ZMenuWaW.ini.

Grenade fountain
    Turn on Weapons > Rapid Fire, then hold G (frags) or 4 (special grenades) to pour out grenades,
    about 20 a second, for as long as the key is down. It uses no ammo. Normal single throws are
    switched off while Rapid Fire is on. If the level gave you no special grenade, 4 throws frags too.
    The two keys are Grenade / SpecialGrenade in ZMenuWaW.ini - set them to whatever your game uses.

Hotkeys
    Highlight any toggle or action in the menu, press Numpad * (or Insert), then the key or mouse
    button you want. It then works with the menu closed. While binding, Delete clears the hotkey and
    Escape cancels. Defaults: middle mouse = Bullet Time, F5 = Quick Save, F9 = Quick Load.
    Weapon Spawner entries cannot be bound (the list changes per level).

Gravity Gun (Player menu)
    Your weapons are holstered while it is on. Hold aim (right mouse) on a soldier to pick him up and
    carry him around; release to drop him. Fire while holding throws him. Fire with nothing held
    shoves bodies and loose props away. The unkillable story characters can be carried but not thrown.

Free Camera (World menu)
    Pauses the whole game and lets you fly the camera with the normal movement keys and mouse.
    Mouse wheel zooms (changes field of view). HUD and weapon are hidden while it is on.

Quick Save / Quick Load (World menu, F5 / F9)
    Quick Save makes a checkpoint right where you stand; Quick Load, or dying, puts you back there.
    One slot per level. Give it two seconds after saving before you load.

Freeze Time / Bullet Time (World menu)
    Bullet Time runs the game at 0.3 speed. Freeze Time slows it to a near stop (5%) while you keep
    moving at about normal speed; your own shots and jumps are slowed with everything else.

Level-dependent features
    Give Flamethrower   only on levels that ship one (Little Resistance, Hard Landing, Vendetta,
                        Burn 'em Out, Blood and Iron, Heart of the Reich, Downfall). Elsewhere use
                        Flame Stream: hold fire to spray bursts of fire that burn enemies.
    Spawn Friendly Dog  only on Vendetta, the one level that contains dogs.
    Airstrike           works everywhere; real rockets fall on levels that have a launcher loaded,
                        plain explosions on the rest.

Menu
    Player         God Mode, No Clip, UFO Mode, Invisible to Enemies, Unlimited Sprint, No Fall Damage,
                   Movement Speed, Jump Height, Gravity, Teleport to Crosshair, Gravity Gun
    Weapons        Unlimited Ammo, No Reload, Rapid Fire, Explosive Bullets, Nuke Bullets, Super Melee Range,
                   Flame Stream, Give Flamethrower, Give All Weapons, Weapon Spawner, Refill Ammo,
                   Remove All Weapons
    Enemies & AI   Spawn enemies / friendlies at the crosshair, Kill All, One-Hit Kills, Freeze AI,
                   Explosion at Crosshair, Airstrike at Crosshair, Battle Royale, Spawn Friendly Dog,
                   Everyone Hostile, Everyone Friendly, Zombies: +10000 Points
    World          Game Speed, Ragdoll Gravity, Knockback, Bullet Time, Freeze Time, Free Camera,
                   Quick Save, Quick Load
    Visuals        Field of View, Third Person, Fullbright, Hide HUD, Hide Weapon, Show FPS
    Levels         Restart Level, jump to any installed campaign level

Notes
    * No Clip, UFO Mode, Rapid Fire, Free Camera, Quick Save and the Weapon Spawner use memory offsets of the 1.0 campaign exe
      (the one this was built against). On another exe version they switch themselves off; the rest
      still works as long as ZMenuWaW.log does not say "unsupported game version".
    * Spawning borrows the level's own spawners, so you get whatever soldier types that level uses.
      A level with no enemy (or friendly) spawners cannot spawn that side.
    * The script half changes the level script, and the game ties save files to its scripts. If an old
      checkpoint refuses to resume, set LoadScripts=0 in ZMenuWaW.ini (menu still works, script features don't).
    * Field of View and Show FPS are regular game settings and are remembered by the game.

Uninstall
    Delete d3d9.dll, ZMenuWaW.gsc, ZMenuWaW.ini and ZMenuWaW.log from the game folder
    (or run: python build.py --remove).

Building from source
    pip install ziglang
    set WAW_DIR to your game folder (the one containing CoDWaW.exe)
    python build.py
