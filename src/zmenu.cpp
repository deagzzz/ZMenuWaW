// ZMenu WaW - in-game trainer menu for Call of Duty: World at War (campaign / co-op exe).
//
// Ships as a d3d9.dll proxy next to CoDWaW.exe:
//   * draws the menu from an EndScene hook
//   * runs console commands through the engine's Cbuf_AddText
//   * flips a few engine fields directly (cheat flag, noclip, weapon fire time)
//   * injects ZMenuWaW.gsc into the stock level script and drives it through zm_* dvars
//     (explosive bullets, AI spawning, ...)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZMENU_VERSION "v1.0"

// ---------------------------------------------------------------------------------------------
// Engine layout. Offsets are for the retail 1.0 campaign exe (PE timestamp 0x48F00000).
// Anything else only gets the console-command based features, found by signature.
// ---------------------------------------------------------------------------------------------

static const DWORD kKnownTimestamp = 0x48F00000;

static const DWORD kCbufAddText = 0x0058DF90; // eax = text, ecx = local client
static const DWORD kCheatsDvar = 0x018B8EFC;  // dvar_t *, bool value at +0x0C
static const DWORD kSaveAborted = 0x01EFFE7C; // byte: set when the player is hit, makes the next save commit do nothing
static const DWORD kPausedDvar = 0x01F4581C;  // dvar_t * of cl_paused (read only from the console), int at +0x0C
static const DWORD kEntities = 0x0173CF00;    // gentity_t[], player 1 is entity 0
static const DWORD kEntClient = 0x180;        // gentity_t::client
static const DWORD kClientNoclip = 0x2234;    // gclient_t::noclip
static const DWORD kClientUfo = 0x2238;       // gclient_t::ufo
static const DWORD kWeaponDefs = 0x008E6FF0;  // WeaponDef *[]
static const DWORD kWeaponCount = 0x0468EA40; // index of the last loaded weapon
static const DWORD kWeapType = 0x144;         // 0 = bullet weapon
static const DWORD kWeapFireType = 0x158;     // 0 = full auto
static const DWORD kWeapFireTime = 0x448;     // milliseconds

static bool g_enabled; // false in anything but the campaign exe (multiplayer is left untouched)
static void *g_cbuf;   // Cbuf_AddText, NULL when it could not be located
static bool g_native;  // exe matches kKnownTimestamp, direct memory features allowed
static char g_exeDir[MAX_PATH];
static char g_iniPath[MAX_PATH];

static bool Readable(const void *p, size_t size, bool write = false)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;
	if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
		return false;
	if (write && !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
		return false;
	return (const BYTE *)p + size <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

// Appends a line to ZMenuWaW.log next to the exe.
static void Log(const char *fmt, ...)
{
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\ZMenuWaW.log", g_exeDir);
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static void CbufAddText(const char *text)
{
	if (!g_cbuf)
		return;
	int a, c, d;
	__asm__ __volatile__("call *%%edx" : "=a"(a), "=c"(c), "=d"(d) : "0"(text), "1"(0), "2"(g_cbuf) : "memory", "cc");
}

static void Exec(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0 || n > (int)sizeof(buf) - 2)
		return;
	buf[n] = '\n';
	buf[n + 1] = 0;
	CbufAddText(buf);
}

static void *FindCbufBySignature(BYTE *base, DWORD size)
{
	// push ebp; push esi; push edi; push critsec; mov esi,eax; mov edi,ecx; call [Enter]; mov al,[esi]; cmp al,'p'
	static const short sig[] = {0x55, 0x56, 0x57, 0x68, -1, -1, -1, -1, 0x8B, 0xF0, 0x8B, 0xF9, 0xFF, 0x15, -1, -1, -1, -1, 0x8A, 0x06, 0x3C, 0x70};
	const int n = sizeof(sig) / sizeof(sig[0]);
	for (DWORD i = 0; i + n < size; i++)
	{
		int j = 0;
		while (j < n && (sig[j] < 0 || base[i + j] == sig[j]))
			j++;
		if (j == n)
			return base + i;
	}
	return NULL;
}

static void InitEngine()
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);

	if (nt->FileHeader.TimeDateStamp == kKnownTimestamp && memcmp((void *)kCbufAddText, "\x55\x56\x57\x68", 4) == 0)
	{
		g_native = true;
		g_cbuf = (void *)kCbufAddText;
		return;
	}

	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	g_cbuf = FindCbufBySignature(base + sec->VirtualAddress, sec->Misc.VirtualSize);
}

// Keeps cheat-protected dvars and commands usable. The engine clears the flag on every map load.
// Returns true when it had to switch cheats back on.
static bool ForceCheats()
{
	if (!g_native)
		return false;
	BYTE *dvar = *(BYTE **)kCheatsDvar;
	if (!Readable(dvar, 0x10, true) || dvar[0xC])
		return false;
	dvar[0xC] = 1;
	return true;
}

static void SetPaused(int paused)
{
	if (!g_native)
		return;
	BYTE *dvar = *(BYTE **)kPausedDvar;
	if (Readable(dvar, 0x10, true))
		*(int *)(dvar + 0xC) = paused;
}

static BYTE *PlayerClient()
{
	if (!g_native)
		return NULL;
	BYTE *client = *(BYTE **)(kEntities + kEntClient);
	return Readable(client, kClientUfo + 4, true) ? client : NULL;
}

static void SetClientInt(DWORD offset, int value)
{
	BYTE *client = PlayerClient();
	if (client)
		*(int *)(client + offset) = value;
}

static BYTE *WeaponDef(int index)
{
	BYTE *def = ((BYTE **)kWeaponDefs)[index];
	if (!Readable(def, kWeapFireTime + 4, true))
		return NULL;
	const char *name = *(const char **)def;
	if (!Readable(name, 2) || name[0] < 'a' || name[0] > 'z')
		return NULL;
	return def;
}

static int WeaponCount()
{
	if (!g_native)
		return 0;
	int last = *(int *)kWeaponCount;
	return last < 1 || last > 255 ? 0 : last;
}

// Rapid fire rewrites fire time / fire type of every loaded weapon and remembers the originals.
struct WeaponPatch
{
	BYTE *def;
	int fireTime, fireType;
	int patchedTime;
};
static WeaponPatch g_weaponPatches[256];

static void ApplyRapidFire(bool on)
{
	int count = WeaponCount();
	for (int i = 1; i <= count; i++)
	{
		BYTE *def = WeaponDef(i);
		if (!def)
			continue;
		int *fireTime = (int *)(def + kWeapFireTime);
		int *fireType = (int *)(def + kWeapFireType);
		WeaponPatch &p = g_weaponPatches[i];
		bool ours = p.def == def && *fireTime == p.patchedTime;

		if (on && !ours)
		{
			// bullet weapons only: grenades and the like misbehave as full auto
			if (*(int *)(def + kWeapType) != 0 || *fireTime <= 0 || *fireTime > 5000 || *fireType < 0 || *fireType > 8)
				continue;
			p.def = def;
			p.fireTime = *fireTime;
			p.fireType = *fireType;
			p.patchedTime = *fireTime > 40 ? 40 : *fireTime;
			*fireTime = p.patchedTime;
			*fireType = 0;
		}
		else if (!on && ours)
		{
			*fireTime = p.fireTime;
			*fireType = p.fireType;
			p.def = NULL;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Menu model
// ---------------------------------------------------------------------------------------------

enum Kind
{
	K_SUB,
	K_TOGGLE,
	K_ACTION,
	K_VALUE
};

enum Id
{
	ID_NONE,
	// script toggles (zm_* dvars)
	T_GOD,
	T_NOTARGET,
	T_AMMO,
	T_CLIP,
	T_EXPLOSIVE,
	T_NUKE,
	T_ONEHIT,
	T_GRAV,
	T_FLAME,
	T_HOSTILE,
	T_FRIENDLY,
	// native toggles
	T_NOCLIP,
	T_UFO,
	T_RAPID,
	// time and camera
	T_BULLET,
	T_FREEZE_TIME,
	T_FREECAM,
	// dvar toggles
	T_SPRINT,
	T_FALLDMG,
	T_MELEE,
	T_FREEZE_AI,
	T_THIRD,
	T_FULLBRIGHT,
	T_HIDE_HUD,
	T_HIDE_GUN,
	T_FPS,
	ID_COUNT
};

struct Item
{
	char label[56];
	Kind kind;
	int id;        // toggle id, value index or submenu index
	char cmd[80];  // K_ACTION: console command, "@script command" or "#built-in action"
	int hotkey;    // virtual key that triggers the item with the menu closed, 0 = none
	bool hotkeyDown;
};

struct Menu
{
	const char *title;
	Item *items;
	int count, cap;
	int sel, scroll;
	void (*build)(Menu *); // rebuilt every time the menu is entered
};

struct DvarToggle
{
	int id;
	const char *sets[2]; // "dvar value" pairs applied while on; reset when switched off
};

struct Value
{
	const char *label, *dvar;
	float def, min, max, step;
	int decimals;
	float cur;
};

static const DvarToggle g_dvarToggles[] = {
	{T_SPRINT, {"player_sprintUnlimited 1", NULL}},
	{T_FALLDMG, {"bg_fallDamageMinHeight 99998", "bg_fallDamageMaxHeight 99999"}},
	{T_MELEE, {"player_meleeRange 1000", NULL}},
	{T_FREEZE_AI, {"g_ai 0", NULL}},
	{T_THIRD, {"cg_thirdPerson 1", NULL}},
	{T_FULLBRIGHT, {"r_fullbright 1", NULL}},
	{T_HIDE_HUD, {"cg_draw2D 0", NULL}},
	{T_HIDE_GUN, {"cg_drawGun 0", NULL}},
	{T_FPS, {"cg_drawFPS 1", NULL}},
};

enum
{
	V_SPEED,
	V_JUMP,
	V_GRAVITY,
	V_TIMESCALE,
	V_RAGDOLL,
	V_KNOCKBACK,
	V_FOV,
	V_COUNT
};

static Value g_values[V_COUNT] = {
	{"Movement Speed", "g_speed", 190, 50, 2000, 50, 0},
	{"Jump Height", "jump_height", 39, 39, 1000, 40, 0},
	{"Gravity", "g_gravity", 800, 50, 2000, 50, 0},
	{"Game Speed", "timescale", 1.0f, 0.1f, 5.0f, 0.1f, 1},
	{"Ragdoll Gravity", "phys_gravity", -800, -2000, 400, 100, 0},
	{"Knockback", "g_knockback", 1000, 0, 30000, 1000, 0},
	{"Field of View", "cg_fov", 65, 40, 160, 5, 0},
};

enum
{
	M_MAIN,
	M_PLAYER,
	M_WEAPONS,
	M_GIVE,
	M_AI,
	M_WORLD,
	M_VISUALS,
	M_LEVELS,
	M_COUNT
};

static Menu g_menus[M_COUNT];
static int g_stack[8], g_depth;
static bool g_open;
static bool g_toggles[ID_COUNT];

static char g_toast[96];
static DWORD g_toastUntil;

static void Toast(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
	va_end(ap);
	g_toastUntil = GetTickCount() + 3000;
}

static Item *AddItem(Menu *m, Kind kind, const char *label, int id, const char *cmd)
{
	if (m->count == m->cap)
	{
		m->cap = m->cap ? m->cap * 2 : 16;
		m->items = (Item *)realloc(m->items, m->cap * sizeof(Item));
	}
	Item *it = &m->items[m->count++];
	memset(it, 0, sizeof(*it));
	snprintf(it->label, sizeof(it->label), "%s", label ? label : "");
	it->kind = kind;
	it->id = id;
	if (cmd)
		snprintf(it->cmd, sizeof(it->cmd), "%s", cmd);
	return it;
}

static void BuildGiveMenu(Menu *m)
{
	m->count = 0;
	int count = WeaponCount();
	for (int i = 1; i <= count; i++)
	{
		BYTE *def = WeaponDef(i);
		if (!def)
			continue;
		const char *name = *(const char **)def;
		if (!Readable(name, 48) || strlen(name) > 47 || strstr(name, "defaultweapon"))
			continue;
		char cmd[80];
		snprintf(cmd, sizeof(cmd), "@give %s", name);
		AddItem(m, K_ACTION, name, 0, cmd);
	}
	if (!m->count)
		AddItem(m, K_ACTION, g_native ? "(load a level first)" : "(needs the 1.0 exe)", 0, "");
	if (m->sel >= m->count)
		m->sel = m->scroll = 0;
}

static const char *const kLevelNames[][2] = {
	{"mak", "Semper Fi"},
	{"pel1", "Little Resistance"},
	{"pel2", "Hard Landing"},
	{"sniper", "Vendetta"},
	{"see1", "Their Land, Their Blood"},
	{"pel1a", "Burn 'em Out"},
	{"pel1b", "Relentless"},
	{"see2", "Blood and Iron"},
	{"ber1", "Ring of Steel"},
	{"ber2", "Eviction"},
	{"pby_fly", "Black Cats"},
	{"oki2", "Blowtorch & Corkscrew"},
	{"oki3", "Breaking Point"},
	{"ber3", "Heart of the Reich"},
	{"ber3b", "Downfall"},
	{"nazi_zombie_prototype", "Nacht der Untoten"},
	{"nazi_zombie_asylum", "Verruckt"},
	{"nazi_zombie_sumpf", "Shi No Numa"},
	{"nazi_zombie_factory", "Der Riese"},
};

static bool LevelInstalled(const char *map)
{
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA fd;
	snprintf(pattern, sizeof(pattern), "%s\\zone\\*", g_exeDir);
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	bool found = false;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
			continue;
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\zone\\%s\\%s.ff", g_exeDir, fd.cFileName, map);
		found = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
	} while (!found && FindNextFileA(h, &fd));
	FindClose(h);
	return found;
}

static void BuildLevelsMenu(Menu *m)
{
	if (m->count)
		return;
	AddItem(m, K_ACTION, "Restart Level", 0, "fast_restart");
	for (size_t i = 0; i < sizeof(kLevelNames) / sizeof(kLevelNames[0]); i++)
	{
		if (!LevelInstalled(kLevelNames[i][0]))
			continue;
		char cmd[80];
		snprintf(cmd, sizeof(cmd), "devmap %s", kLevelNames[i][0]);
		AddItem(m, K_ACTION, kLevelNames[i][1], 0, cmd);
	}
}

static void BuildMenus()
{
	Menu *m = &g_menus[M_MAIN];
	m->title = "Main Menu";
	AddItem(m, K_SUB, "Player", M_PLAYER, NULL);
	AddItem(m, K_SUB, "Weapons", M_WEAPONS, NULL);
	AddItem(m, K_SUB, "Enemies & AI", M_AI, NULL);
	AddItem(m, K_SUB, "World", M_WORLD, NULL);
	AddItem(m, K_SUB, "Visuals", M_VISUALS, NULL);
	AddItem(m, K_SUB, "Levels", M_LEVELS, NULL);

	m = &g_menus[M_PLAYER];
	m->title = "Player";
	AddItem(m, K_TOGGLE, "God Mode", T_GOD, NULL);
	AddItem(m, K_TOGGLE, "No Clip", T_NOCLIP, NULL);
	AddItem(m, K_TOGGLE, "UFO Mode", T_UFO, NULL);
	AddItem(m, K_TOGGLE, "Invisible to Enemies", T_NOTARGET, NULL);
	AddItem(m, K_TOGGLE, "Unlimited Sprint", T_SPRINT, NULL);
	AddItem(m, K_TOGGLE, "No Fall Damage", T_FALLDMG, NULL);
	AddItem(m, K_VALUE, NULL, V_SPEED, NULL);
	AddItem(m, K_VALUE, NULL, V_JUMP, NULL);
	AddItem(m, K_VALUE, NULL, V_GRAVITY, NULL);
	AddItem(m, K_ACTION, "Teleport to Crosshair", 0, "@tp");
	AddItem(m, K_TOGGLE, "Gravity Gun", T_GRAV, NULL);

	m = &g_menus[M_WEAPONS];
	m->title = "Weapons";
	AddItem(m, K_TOGGLE, "Unlimited Ammo", T_AMMO, NULL);
	AddItem(m, K_TOGGLE, "No Reload", T_CLIP, NULL);
	AddItem(m, K_TOGGLE, "Rapid Fire", T_RAPID, NULL);
	AddItem(m, K_TOGGLE, "Explosive Bullets", T_EXPLOSIVE, NULL);
	AddItem(m, K_TOGGLE, "Nuke Bullets", T_NUKE, NULL);
	AddItem(m, K_TOGGLE, "Super Melee Range", T_MELEE, NULL);
	AddItem(m, K_TOGGLE, "Flame Stream", T_FLAME, NULL);
	AddItem(m, K_ACTION, "Give Flamethrower", 0, "#flamethrower");
	AddItem(m, K_ACTION, "Give All Weapons", 0, "give all|@arm");
	AddItem(m, K_SUB, "Weapon Spawner", M_GIVE, NULL);
	AddItem(m, K_ACTION, "Refill Ammo", 0, "@ammo");
	AddItem(m, K_ACTION, "Remove All Weapons", 0, "@disarm");

	m = &g_menus[M_GIVE];
	m->title = "Weapon Spawner";
	m->build = BuildGiveMenu;

	m = &g_menus[M_AI];
	m->title = "Enemies & AI";
	AddItem(m, K_ACTION, "Spawn Enemy", 0, "@spawn axis 1");
	AddItem(m, K_ACTION, "Spawn 5 Enemies", 0, "@spawn axis 5");
	AddItem(m, K_ACTION, "Spawn 15 Enemies", 0, "@spawn axis 15");
	AddItem(m, K_ACTION, "Spawn Friendly", 0, "@spawn allies 1");
	AddItem(m, K_ACTION, "Spawn 5 Friendlies", 0, "@spawn allies 5");
	AddItem(m, K_ACTION, "Kill All Enemies", 0, "@killall axis");
	AddItem(m, K_ACTION, "Kill All Friendlies", 0, "@killall allies");
	AddItem(m, K_TOGGLE, "One-Hit Kills", T_ONEHIT, NULL);
	AddItem(m, K_TOGGLE, "Freeze AI", T_FREEZE_AI, NULL);
	AddItem(m, K_ACTION, "Explosion at Crosshair", 0, "@boom 1");
	AddItem(m, K_ACTION, "Huge Explosion at Crosshair", 0, "@boom 2");
	AddItem(m, K_ACTION, "Airstrike at Crosshair", 0, "#airstrike");
	AddItem(m, K_ACTION, "Battle Royale", 0, "@royale 8");
	AddItem(m, K_ACTION, "Spawn Friendly Dog", 0, "@dogs 1");
	AddItem(m, K_TOGGLE, "Everyone Hostile", T_HOSTILE, NULL);
	AddItem(m, K_TOGGLE, "Everyone Friendly", T_FRIENDLY, NULL);
	AddItem(m, K_ACTION, "Zombies: +10000 Points", 0, "@points 10000");

	m = &g_menus[M_WORLD];
	m->title = "World";
	AddItem(m, K_VALUE, NULL, V_TIMESCALE, NULL);
	AddItem(m, K_VALUE, NULL, V_RAGDOLL, NULL);
	AddItem(m, K_VALUE, NULL, V_KNOCKBACK, NULL);
	AddItem(m, K_TOGGLE, "Bullet Time", T_BULLET, NULL);
	AddItem(m, K_TOGGLE, "Freeze Time", T_FREEZE_TIME, NULL);
	AddItem(m, K_TOGGLE, "Free Camera", T_FREECAM, NULL);
	AddItem(m, K_ACTION, "Quick Save", 0, "#save");
	AddItem(m, K_ACTION, "Quick Load", 0, "#load");

	m = &g_menus[M_VISUALS];
	m->title = "Visuals";
	AddItem(m, K_VALUE, NULL, V_FOV, NULL);
	AddItem(m, K_TOGGLE, "Third Person", T_THIRD, NULL);
	AddItem(m, K_TOGGLE, "Fullbright", T_FULLBRIGHT, NULL);
	AddItem(m, K_TOGGLE, "Hide HUD", T_HIDE_HUD, NULL);
	AddItem(m, K_TOGGLE, "Hide Weapon", T_HIDE_GUN, NULL);
	AddItem(m, K_TOGGLE, "Show FPS", T_FPS, NULL);

	m = &g_menus[M_LEVELS];
	m->title = "Levels";
	m->build = BuildLevelsMenu;
	BuildLevelsMenu(m); // built up front so its entries can carry hotkeys

	for (int i = 0; i < V_COUNT; i++)
		g_values[i].cur = g_values[i].def;
}

// ---------------------------------------------------------------------------------------------
// Feature plumbing
// ---------------------------------------------------------------------------------------------

// One-shot script commands are spaced out so the script (20 Hz) never misses one.
static char g_scriptQueue[16][64];
static int g_scriptHead, g_scriptTail;
static int g_scriptSeq;
static DWORD g_scriptLast;

static void ScriptCommand(const char *cmd)
{
	int next = (g_scriptTail + 1) % 16;
	if (next == g_scriptHead)
		return;
	snprintf(g_scriptQueue[g_scriptTail], sizeof(g_scriptQueue[0]), "%s", cmd);
	g_scriptTail = next;
}

static void ApplyDvarToggle(const DvarToggle &t, bool on)
{
	for (int i = 0; i < 2 && t.sets[i]; i++)
	{
		if (on)
			Exec("set %s", t.sets[i]);
		else
		{
			char name[64];
			snprintf(name, sizeof(name), "%s", t.sets[i]);
			*strchr(name, ' ') = 0;
			Exec("reset %s", name);
		}
	}
}

// Freeze Time is a near-stop of the game clock with the player's speed scaled back up to match.
static const float kFreezeScale = 0.05f;

static void ApplyValue(const Value &v)
{
	// Bullet Time and Freeze Time own the game clock (and Freeze Time the run speed) while they are on
	if (&v == &g_values[V_TIMESCALE] && (g_toggles[T_BULLET] || g_toggles[T_FREEZE_TIME]))
		return;
	if (&v == &g_values[V_SPEED] && g_toggles[T_FREEZE_TIME])
	{
		Exec("set g_speed %d", (int)(v.cur / kFreezeScale));
		return;
	}

	if (v.cur == v.def)
		Exec("reset %s", v.dvar);
	else
		Exec("set %s %.*f", v.dvar, v.decimals, v.cur);
}

// withSpeed: also (re)apply the run speed, which only Freeze Time changes
static void ApplyTime(bool withSpeed)
{
	if (g_toggles[T_FREEZE_TIME])
		Exec("set timescale %.2f", kFreezeScale);
	else if (g_toggles[T_BULLET])
		Exec("set timescale 0.3");
	else
		ApplyValue(g_values[V_TIMESCALE]);
	if (withSpeed)
		ApplyValue(g_values[V_SPEED]);
}

static void SendScriptToggles()
{
	Exec("set zm_god %d;set zm_notarget %d;set zm_ammo %d;set zm_clip %d;set zm_onehit %d;set zm_explosive %d;set zm_rapid %d",
		 g_toggles[T_GOD], g_toggles[T_NOTARGET], g_toggles[T_AMMO], g_toggles[T_CLIP], g_toggles[T_ONEHIT],
		 g_toggles[T_NUKE] ? 2 : g_toggles[T_EXPLOSIVE] ? 1 : 0, g_toggles[T_RAPID]);
	Exec("set zm_grav %d;set zm_flame %d;set zm_team %d", g_toggles[T_GRAV], g_toggles[T_FLAME],
		 g_toggles[T_HOSTILE] ? 1 : g_toggles[T_FRIENDLY] ? 2 : 0);
}

// Free Camera: the game is paused and the engine's own fly-around camera takes over. The mouse wheel
// zooms; a low-level mouse hook is the only way to see the wheel, and it only exists while the mode is on.
static DWORD g_saveGuardUntil; // while a quick save is being written, keep hits from cancelling it
static DWORD g_saveFlushAt;    // when to ask the engine to put the committed quick save on disk
static volatile LONG g_wheel;
static DWORD g_wheelThread;
static int g_freecamFov;

static LRESULT CALLBACK WheelHook(int code, WPARAM wparam, LPARAM lparam)
{
	if (code >= 0 && wparam == WM_MOUSEWHEEL)
		InterlockedExchangeAdd(&g_wheel, (short)HIWORD(((MSLLHOOKSTRUCT *)lparam)->mouseData));
	return CallNextHookEx(NULL, code, wparam, lparam);
}

static DWORD WINAPI WheelThread(LPVOID)
{
	HHOOK hook = SetWindowsHookExA(WH_MOUSE_LL, WheelHook, GetModuleHandleA(NULL), 0);
	MSG msg;
	while (GetMessageA(&msg, NULL, 0, 0) > 0) {}
	if (hook)
		UnhookWindowsHookEx(hook);
	return 0;
}

static void ApplyDvarToggle(const DvarToggle &t, bool on);

static void SetFreeCamera(bool on)
{
	if (on)
	{
		g_freecamFov = (int)g_values[V_FOV].cur;
		InterlockedExchange(&g_wheel, 0);
		Exec("set cl_freemove 1;set cg_draw2D 0;set cg_drawGun 0");
		if (!g_wheelThread)
			CloseHandle(CreateThread(NULL, 0, WheelThread, NULL, 0, &g_wheelThread));
		return;
	}

	if (g_wheelThread)
		PostThreadMessageA(g_wheelThread, WM_QUIT, 0, 0);
	g_wheelThread = 0;
	Exec("set cl_freemove 0");
	SetPaused(0);
	for (size_t i = 0; i < sizeof(g_dvarToggles) / sizeof(g_dvarToggles[0]); i++)
		if (g_dvarToggles[i].id == T_HIDE_HUD || g_dvarToggles[i].id == T_HIDE_GUN)
			ApplyDvarToggle(g_dvarToggles[i], g_toggles[g_dvarToggles[i].id]);
	ApplyValue(g_values[V_FOV]);
}

static void PumpFreeCamera()
{
	if (!g_toggles[T_FREECAM])
		return;
	SetPaused(1); // every frame: the engine clears it whenever it thinks nothing is paused
	LONG wheel = InterlockedExchange(&g_wheel, 0);
	if (!wheel)
		return;
	g_freecamFov -= wheel / WHEEL_DELTA * 5;
	if (g_freecamFov < 10)
		g_freecamFov = 10;
	if (g_freecamFov > 160)
		g_freecamFov = 160;
	Exec("set cg_fov %d", g_freecamFov);
}

// First loaded weapon whose name starts with prefix; type < 0 accepts any weapon type.
static const char *FindWeapon(const char *prefix, int type)
{
	int count = WeaponCount();
	for (int i = 1; i <= count; i++)
	{
		BYTE *def = WeaponDef(i);
		if (!def)
			continue;
		const char *name = *(const char **)def;
		if (Readable(name, 48) && strncmp(name, prefix, strlen(prefix)) == 0 && (type < 0 || *(int *)(def + kWeapType) == type))
			return name;
	}
	return NULL;
}

static void ScriptCommand(const char *cmd);

// The "#..." menu actions that need more than a console or script command.
static void BuiltinAction(const char *action)
{
	char cmd[64];
	if (!strcmp(action, "flamethrower"))
	{
		const char *name = FindWeapon("m2_flamethrower", -1);
		if (!name)
		{
			Toast("This level has no flamethrower - use Flame Stream instead");
			return;
		}
		snprintf(cmd, sizeof(cmd), "give %s", name);
		ScriptCommand(cmd);
	}
	else if (!strcmp(action, "airstrike"))
	{
		// real rockets when the level has a launcher loaded (weapon type 2 = projectile), plain blasts otherwise
		const char *rocket = FindWeapon("panzerschrek", 2);
		if (!rocket)
			rocket = FindWeapon("bazooka", 2);
		snprintf(cmd, sizeof(cmd), "airstrike %s", rocket ? rocket : "none");
		ScriptCommand(cmd);
	}
	else if (!strcmp(action, "save"))
	{
		// saved by the script: the console's savegame command is ignored mid-level
		ScriptCommand("save");
		g_saveGuardUntil = GetTickCount() + 4000;
		g_saveFlushAt = GetTickCount() + 1800;
		Toast("Quick saved");
	}
	else if (!strcmp(action, "load"))
	{
		Exec("vstr zm_load"); // "loadgame <level>-zmenu_quick", kept current by the script
	}
}

// Grenade fountain: with Rapid Fire on, holding a grenade key makes the script pour out grenades.
// The script cannot see the special-grenade button, so the keys are read here and passed as zm_nade.
static int g_grenadeKeys[2] = {'G', '4'}; // frag, special

static void PumpGrenadeFountain()
{
	static int sent;
	int mode = 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	if (g_toggles[T_RAPID] && pid == GetCurrentProcessId())
	{
		if (g_grenadeKeys[0] && (GetAsyncKeyState(g_grenadeKeys[0]) & 0x8000))
			mode = 1;
		else if (g_grenadeKeys[1] && (GetAsyncKeyState(g_grenadeKeys[1]) & 0x8000))
			mode = 2;
	}
	if (mode != sent)
		Exec("set zm_nade %d", mode);
	sent = mode;
}

static bool NativeToggle(int id)
{
	return id == T_NOCLIP || id == T_UFO || id == T_RAPID || id == T_FREECAM;
}

static void OnToggle(int id)
{
	bool on = g_toggles[id];

	if (NativeToggle(id) && !g_native)
	{
		g_toggles[id] = false;
		Toast("Not available on this game version");
		return;
	}

	switch (id)
	{
	case T_NOCLIP:
		g_toggles[T_UFO] = false;
		SetClientInt(kClientUfo, 0);
		SetClientInt(kClientNoclip, on);
		return;
	case T_UFO:
		g_toggles[T_NOCLIP] = false;
		SetClientInt(kClientNoclip, 0);
		SetClientInt(kClientUfo, on);
		return;
	case T_RAPID:
		ApplyRapidFire(on);
		break;
	case T_BULLET:
		ApplyTime(false);
		return;
	case T_FREEZE_TIME:
		ApplyTime(true);
		return;
	case T_FREECAM:
		SetFreeCamera(on);
		return;
	case T_HOSTILE:
		if (on)
			g_toggles[T_FRIENDLY] = false;
		break;
	case T_FRIENDLY:
		if (on)
			g_toggles[T_HOSTILE] = false;
		break;
	case T_EXPLOSIVE:
		if (on)
			g_toggles[T_NUKE] = false;
		break;
	case T_NUKE:
		if (on)
			g_toggles[T_EXPLOSIVE] = false;
		break;
	}

	for (size_t i = 0; i < sizeof(g_dvarToggles) / sizeof(g_dvarToggles[0]); i++)
	{
		if (g_dvarToggles[i].id == id)
		{
			ApplyDvarToggle(g_dvarToggles[i], on);
			return;
		}
	}

	SendScriptToggles();
}

// Runs every frame: keeps engine state in line with the menu, whatever level loads did to it.
static void PumpFeatures()
{
	static DWORD lastRefresh;
	DWORD now = GetTickCount();

	bool cheatsWereReset = ForceCheats();
	PumpGrenadeFountain();
	PumpFreeCamera();
	if (g_native && (int)(g_saveGuardUntil - now) > 0)
		*(BYTE *)kSaveAborted = 0;
	if (g_saveFlushAt && (int)(now - g_saveFlushAt) >= 0)
	{
		g_saveFlushAt = 0;
		Exec("savegame_lastcommit");
	}

	if (g_toggles[T_NOCLIP])
		SetClientInt(kClientNoclip, 1);
	if (g_toggles[T_UFO])
		SetClientInt(kClientUfo, 1);

	if (cheatsWereReset || now - lastRefresh > 2000)
	{
		lastRefresh = now;
		for (size_t i = 0; i < sizeof(g_dvarToggles) / sizeof(g_dvarToggles[0]); i++)
			if (g_toggles[g_dvarToggles[i].id])
				ApplyDvarToggle(g_dvarToggles[i], true);
		for (int i = 0; i < V_COUNT; i++)
			if (g_values[i].cur != g_values[i].def)
				ApplyValue(g_values[i]);
		if (g_toggles[T_RAPID])
			ApplyRapidFire(true);
		if (g_toggles[T_BULLET] || g_toggles[T_FREEZE_TIME])
			ApplyTime(g_toggles[T_FREEZE_TIME]);
		if (g_toggles[T_FREECAM])
			Exec("set cl_freemove 1");
	}

	if (g_scriptHead != g_scriptTail && now - g_scriptLast > 150)
	{
		g_scriptLast = now;
		if (!g_scriptSeq)
			g_scriptSeq = (int)(now & 0xFFFF) + 1;
		Exec("set zm_cmd \"%s\";set zm_seq %d", g_scriptQueue[g_scriptHead], ++g_scriptSeq);
		g_scriptHead = (g_scriptHead + 1) % 16;
	}
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

enum
{
	KEY_OPEN,
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_SELECT,
	KEY_BACK,
	KEY_BIND,
	KEY_COUNT
};

static const char *const kKeyNames[KEY_COUNT] = {"Open", "Up", "Down", "Left", "Right", "Select", "Back", "Bind"};
static int g_keys[KEY_COUNT][2] = {
	{VK_F7, 0},
	{VK_NUMPAD8, VK_UP},
	{VK_NUMPAD2, VK_DOWN},
	{VK_NUMPAD4, VK_LEFT},
	{VK_NUMPAD6, VK_RIGHT},
	{VK_NUMPAD5, VK_RETURN},
	{VK_NUMPAD0, VK_BACK},
	{VK_MULTIPLY, VK_INSERT},
};

static bool g_keyDown[KEY_COUNT];
static DWORD g_keyNext[KEY_COUNT];

static void LoadConfig()
{
	for (int i = 0; i < KEY_COUNT; i++)
	{
		char alt[32];
		snprintf(alt, sizeof(alt), "%sAlt", kKeyNames[i]);
		g_keys[i][0] = GetPrivateProfileIntA("Keys", kKeyNames[i], g_keys[i][0], g_iniPath);
		g_keys[i][1] = GetPrivateProfileIntA("Keys", alt, g_keys[i][1], g_iniPath);
	}
	g_grenadeKeys[0] = GetPrivateProfileIntA("Keys", "Grenade", g_grenadeKeys[0], g_iniPath);
	g_grenadeKeys[1] = GetPrivateProfileIntA("Keys", "SpecialGrenade", g_grenadeKeys[1], g_iniPath);
}

static const char *ItemLabel(const Item *it)
{
	return it->kind == K_VALUE ? g_values[it->id].label : it->label;
}

static const struct { const char *label; int key; } kDefaultHotkeys[] = {
	{"Bullet Time", VK_MBUTTON},
	{"Quick Save", VK_F5},
	{"Quick Load", VK_F9},
};

// Hotkeys live in [Hotkeys] of the ini, keyed by the item's menu label.
static void LoadHotkeys()
{
	for (int m = 0; m < M_COUNT; m++)
	{
		for (int i = 0; i < g_menus[m].count; i++)
		{
			Item *it = &g_menus[m].items[i];
			if (it->kind == K_SUB)
				continue;
			int key = 0;
			for (size_t d = 0; d < sizeof(kDefaultHotkeys) / sizeof(kDefaultHotkeys[0]); d++)
				if (!strcmp(kDefaultHotkeys[d].label, ItemLabel(it)))
					key = kDefaultHotkeys[d].key;
			it->hotkey = GetPrivateProfileIntA("Hotkeys", ItemLabel(it), key, g_iniPath);
		}
	}
}

static void KeyName(int key, char *out, size_t size)
{
	if (key >= VK_F1 && key <= VK_F24)
		snprintf(out, size, "F%d", key - VK_F1 + 1);
	else if (key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2)
		snprintf(out, size, "Mouse%d", key == VK_MBUTTON ? 3 : key == VK_XBUTTON1 ? 4 : 5);
	else if (!GetKeyNameTextA(MapVirtualKeyA(key, MAPVK_VK_TO_VSC) << 16, out, (int)size))
		snprintf(out, size, "Key %d", key);
}

// True on the initial press and then on auto-repeat for navigation keys.
static bool KeyHit(int key, bool repeat)
{
	bool down = false;
	for (int i = 0; i < 2; i++)
		if (g_keys[key][i] && (GetAsyncKeyState(g_keys[key][i]) & 0x8000))
			down = true;

	DWORD now = GetTickCount();
	bool hit = false;
	if (down && !g_keyDown[key])
	{
		hit = true;
		g_keyNext[key] = now + 320;
	}
	else if (down && repeat && (int)(now - g_keyNext[key]) >= 0)
	{
		hit = true;
		g_keyNext[key] = now + 70;
	}
	g_keyDown[key] = down;
	return hit;
}

static void EnterMenu(int index)
{
	Menu *m = &g_menus[index];
	if (m->build)
		m->build(m);
	if (g_depth < 7)
		g_stack[++g_depth] = index;
}

static void AdjustValue(Value &v, int dir)
{
	float cur = v.cur + dir * v.step;
	// snap to the step grid so repeated presses land back on the default exactly
	cur = (float)((int)(cur / v.step + (cur < 0 ? -0.5f : 0.5f))) * v.step;
	if (dir > 0 && v.cur < v.def && cur > v.def)
		cur = v.def;
	if (dir < 0 && v.cur > v.def && cur < v.def)
		cur = v.def;
	if (cur < v.min)
		cur = v.min;
	if (cur > v.max)
		cur = v.max;
	if (cur == v.cur)
		return;
	v.cur = cur;
	ApplyValue(v);
}

static void Activate(Item *it)
{
	switch (it->kind)
	{
	case K_SUB:
		EnterMenu(it->id);
		break;
	case K_TOGGLE:
		g_toggles[it->id] = !g_toggles[it->id];
		OnToggle(it->id);
		break;
	case K_ACTION:
	{
		// "console command", "@script command", "#built-in action", or several joined with '|'
		char cmds[sizeof(it->cmd)];
		strcpy(cmds, it->cmd);
		for (char *cmd = strtok(cmds, "|"); cmd; cmd = strtok(NULL, "|"))
		{
			if (cmd[0] == '@')
				ScriptCommand(cmd + 1);
			else if (cmd[0] == '#')
				BuiltinAction(cmd + 1);
			else
				Exec("%s", cmd);
		}
		break;
	}
	case K_VALUE:
		g_values[it->id].cur = g_values[it->id].def;
		ApplyValue(g_values[it->id]);
		break;
	}
}

static bool g_binding;        // waiting for the key to bind to the selected item
static BYTE g_bindHeld[256];  // keys that were already down when binding started

// Assigns the next key pressed to the item. Delete clears the hotkey, Escape leaves it alone.
static void CaptureHotkey(Item *it)
{
	for (int key = 3; key < 255; key++)
	{
		bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
		if (!down)
			g_bindHeld[key] = 0;
		// left/right specific modifier codes repeat the generic ones
		if (!down || g_bindHeld[key] || (key >= VK_LSHIFT && key <= VK_RMENU))
			continue;

		g_binding = false;
		if (key == VK_ESCAPE)
			return;
		it->hotkey = key == VK_DELETE ? 0 : key;
		it->hotkeyDown = true;
		char value[16];
		snprintf(value, sizeof(value), "%d", it->hotkey);
		WritePrivateProfileStringA("Hotkeys", ItemLabel(it), value, g_iniPath);
		return;
	}
}

static void RunHotkeys()
{
	for (int m = 0; m < M_COUNT; m++)
	{
		for (int i = 0; i < g_menus[m].count; i++)
		{
			Item *it = &g_menus[m].items[i];
			if (!it->hotkey)
				continue;
			bool down = (GetAsyncKeyState(it->hotkey) & 0x8000) != 0;
			bool hit = down && !it->hotkeyDown;
			it->hotkeyDown = down;
			if (!hit)
				continue;

			Activate(it);
			if (it->kind == K_TOGGLE)
				Toast("%s %s", it->label, g_toggles[it->id] ? "ON" : "OFF");
			else if (strcmp(it->cmd, "#save") != 0)
				Toast("%s", ItemLabel(it));
		}
	}
}

static void HandleInput()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	if (pid != GetCurrentProcessId())
		return;

	Menu *m = &g_menus[g_stack[g_depth]];
	if (g_binding)
	{
		if (g_open && m->count)
			CaptureHotkey(&m->items[m->sel]);
		else
			g_binding = false;
		// swallow this frame's edges so the bound key does not also act as a menu key
		for (int key = 0; key < KEY_COUNT; key++)
			KeyHit(key, false);
		return;
	}

	RunHotkeys();

	if (KeyHit(KEY_OPEN, false))
		g_open = !g_open;

	bool up = KeyHit(KEY_UP, true), down = KeyHit(KEY_DOWN, true);
	bool left = KeyHit(KEY_LEFT, true), right = KeyHit(KEY_RIGHT, true);
	bool select = KeyHit(KEY_SELECT, false), back = KeyHit(KEY_BACK, false);
	bool bind = KeyHit(KEY_BIND, false);
	if (!g_open)
		return;

	if (bind && m->count && m->items[m->sel].kind != K_SUB)
	{
		if (m->build == BuildGiveMenu)
			Toast("Weapon Spawner entries change per level and cannot have hotkeys");
		else
		{
			g_binding = true;
			for (int key = 0; key < 256; key++)
				g_bindHeld[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
		}
		return;
	}

	if (up && m->count)
		m->sel = (m->sel + m->count - 1) % m->count;
	if (down && m->count)
		m->sel = (m->sel + 1) % m->count;

	Item *it = m->count ? &m->items[m->sel] : NULL;
	if (it && it->kind == K_VALUE && (left || right))
		AdjustValue(g_values[it->id], right ? 1 : -1);
	if (it && select)
		Activate(it);

	if (back)
	{
		if (g_depth > 0)
			g_depth--;
		else
			g_open = false;
	}
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

struct Vertex
{
	float x, y, z, rhw;
	DWORD color;
	float u, v;
};
#define VERTEX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct Font
{
	IDirect3DTexture9 *tex;
	int px;
	int cellW, cellH, texW, texH;
	int advance[96];
};

static Font g_font;
static IDirect3DStateBlock9 *g_stateBlock;
static IDirect3DDevice9 *g_device;
static Vertex g_verts[6 * 1024];
static int g_vertCount;

static bool BuildFont(IDirect3DDevice9 *dev, int px)
{
	if (g_font.tex)
		g_font.tex->Release();
	memset(&g_font, 0, sizeof(g_font));

	HDC dc = CreateCompatibleDC(NULL);
	HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
							 ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
	HGDIOBJ oldFont = SelectObject(dc, font);

	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	int maxW = 1;
	for (int i = 0; i < 95; i++)
	{
		char ch = (char)(32 + i);
		SIZE size;
		GetTextExtentPoint32A(dc, &ch, 1, &size);
		g_font.advance[i] = size.cx;
		if (size.cx > maxW)
			maxW = size.cx;
	}
	g_font.px = px;
	g_font.cellW = maxW + 4;
	g_font.cellH = tm.tmHeight + 2;
	for (g_font.texW = 64; g_font.texW < g_font.cellW * 16; g_font.texW *= 2) {}
	for (g_font.texH = 64; g_font.texH < g_font.cellH * 6; g_font.texH *= 2) {}

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = g_font.texW;
	bi.bmiHeader.biHeight = -g_font.texH;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	DWORD *bits = NULL;
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
	bool ok = false;
	if (bmp && bits)
	{
		HGDIOBJ oldBmp = SelectObject(dc, bmp);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		for (int i = 0; i < 95; i++)
		{
			char ch = (char)(32 + i);
			TextOutA(dc, (i % 16) * g_font.cellW + 1, (i / 16) * g_font.cellH + 1, &ch, 1);
		}
		GdiFlush();

		// the last cell is solid white and backs every flat-colored rectangle
		for (int y = 0; y < g_font.cellH; y++)
			for (int x = 0; x < g_font.cellW; x++)
				bits[(5 * g_font.cellH + y) * g_font.texW + 15 * g_font.cellW + x] = 0xFFFFFF;

		D3DLOCKED_RECT lock;
		if (SUCCEEDED(dev->CreateTexture(g_font.texW, g_font.texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_font.tex, NULL)) &&
			SUCCEEDED(g_font.tex->LockRect(0, &lock, NULL, 0)))
		{
			for (int y = 0; y < g_font.texH; y++)
			{
				DWORD *dst = (DWORD *)((BYTE *)lock.pBits + y * lock.Pitch);
				for (int x = 0; x < g_font.texW; x++)
					dst[x] = ((bits[y * g_font.texW + x] & 0xFF) << 24) | 0xFFFFFF;
			}
			g_font.tex->UnlockRect(0);
			ok = true;
		}
		SelectObject(dc, oldBmp);
		DeleteObject(bmp);
	}
	SelectObject(dc, oldFont);
	DeleteObject(font);
	DeleteDC(dc);
	return ok;
}

static void Flush()
{
	if (g_vertCount)
		g_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, g_vertCount / 3, g_verts, sizeof(Vertex));
	g_vertCount = 0;
}

static void Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, DWORD color)
{
	if (g_vertCount + 6 > (int)(sizeof(g_verts) / sizeof(g_verts[0])))
		Flush();
	x -= 0.5f;
	y -= 0.5f;
	Vertex q[4] = {
		{x, y, 0, 1, color, u0, v0},
		{x + w, y, 0, 1, color, u1, v0},
		{x, y + h, 0, 1, color, u0, v1},
		{x + w, y + h, 0, 1, color, u1, v1},
	};
	static const int order[6] = {0, 1, 2, 2, 1, 3};
	for (int i = 0; i < 6; i++)
		g_verts[g_vertCount++] = q[order[i]];
}

static void Rect(float x, float y, float w, float h, DWORD color)
{
	float u = (15.5f * g_font.cellW) / g_font.texW, v = (5.5f * g_font.cellH) / g_font.texH;
	Quad(x, y, w, h, u, v, u, v, color);
}

static float TextWidth(const char *s)
{
	float w = 0;
	for (; *s; s++)
		w += g_font.advance[(*s >= 32 && *s < 127) ? *s - 32 : '?' - 32];
	return w;
}

static void Text(float x, float y, const char *s, DWORD color)
{
	x = (float)(int)x;
	y = (float)(int)y;
	for (; *s; s++)
	{
		int i = (*s >= 32 && *s < 127) ? *s - 32 : '?' - 32;
		float u0 = (float)((i % 16) * g_font.cellW) / g_font.texW, v0 = (float)((i / 16) * g_font.cellH) / g_font.texH;
		Quad(x - 1, y - 1, (float)g_font.cellW, (float)g_font.cellH, u0, v0, u0 + (float)g_font.cellW / g_font.texW,
			 v0 + (float)g_font.cellH / g_font.texH, color);
		x += g_font.advance[i];
	}
}

static const DWORD kColPanel = 0xD2101214;
static const DWORD kColHeader = 0xF0B4281E;
static const DWORD kColSelected = 0xF0F2F2F2;
static const DWORD kColText = 0xFFF2F2F2;
static const DWORD kColTextDark = 0xFF141414;
static const DWORD kColDim = 0xFF9A9A9A;
static const DWORD kColOn = 0xFF5ADC5A;
static const DWORD kColOnDark = 0xFF1E8C1E;

static void DrawMenu(float scale)
{
	const int kVisible = 14;
	Menu *m = &g_menus[g_stack[g_depth]];

	float pad = 14 * scale;
	float rowH = g_font.cellH + 8 * scale;
	float x = 48 * scale, y = 64 * scale, w = 400 * scale;
	float headerH = g_font.cellH * 2 + 14 * scale;

	if (m->sel < m->scroll)
		m->scroll = m->sel;
	if (m->sel >= m->scroll + kVisible)
		m->scroll = m->sel - kVisible + 1;
	int rows = m->count < kVisible ? m->count : kVisible;

	Rect(x, y, w, headerH, kColHeader);
	Text(x + pad, y + 5 * scale, "ZMenu WaW", kColText);
	Text(x + pad, y + 5 * scale + g_font.cellH, m->title, 0xFFFFD2C8);
	Text(x + w - pad - TextWidth(ZMENU_VERSION), y + 5 * scale, ZMENU_VERSION, 0xFFFFD2C8);
	y += headerH;

	Rect(x, y, w, rows * rowH, kColPanel);
	for (int r = 0; r < rows; r++)
	{
		int index = m->scroll + r;
		Item *it = &m->items[index];
		bool sel = index == m->sel;
		float ty = y + r * rowH + 4 * scale;
		if (sel)
			Rect(x, y + r * rowH, w, rowH, kColSelected);

		const char *label = ItemLabel(it);
		Text(x + pad, ty, label, sel ? kColTextDark : kColText);
		if (it->hotkey)
		{
			char key[40] = "[";
			KeyName(it->hotkey, key + 1, sizeof(key) - 2);
			strcat(key, "]");
			Text(x + pad + TextWidth(label) + 8 * scale, ty, key, sel ? 0xFF5A5A5A : kColDim);
		}

		char right[48] = "";
		DWORD rightColor = sel ? kColTextDark : kColDim;
		switch (it->kind)
		{
		case K_SUB:
			strcpy(right, ">");
			break;
		case K_TOGGLE:
			strcpy(right, g_toggles[it->id] ? "ON" : "OFF");
			if (g_toggles[it->id])
				rightColor = sel ? kColOnDark : kColOn;
			break;
		case K_VALUE:
		{
			Value &v = g_values[it->id];
			snprintf(right, sizeof(right), "< %.*f >", v.decimals, v.cur);
			if (v.cur != v.def)
				rightColor = sel ? kColOnDark : kColOn;
			break;
		}
		case K_ACTION:
			break;
		}
		Text(x + w - pad - TextWidth(right), ty, right, rightColor);
	}
	y += rows * rowH;

	char footer[96];
	snprintf(footer, sizeof(footer), "%d / %d", m->count ? m->sel + 1 : 0, m->count);
	Rect(x, y, w, rowH, 0xF0000000);
	const char *hint = !g_cbuf ? "Unsupported game version" : g_binding ? "Press a key (Del clears, Esc cancels)" : "Num * binds a hotkey";
	Text(x + pad, y + 4 * scale, hint, g_binding ? kColOn : kColDim);
	Text(x + w - pad - TextWidth(footer), y + 4 * scale, footer, kColDim);
}

static void DrawToast(float scale, float screenH)
{
	if ((int)(g_toastUntil - GetTickCount()) <= 0)
		return;
	float w = TextWidth(g_toast) + 28 * scale, h = g_font.cellH + 12 * scale;
	float x = 48 * scale, y = g_open ? screenH - h - 48 * scale : 64 * scale;
	Rect(x, y, w, h, kColPanel);
	Rect(x, y, 4 * scale, h, kColHeader);
	Text(x + 14 * scale, y + 6 * scale, g_toast, kColText);
}

static void Render(IDirect3DDevice9 *dev)
{
	// only draw onto the real back buffer
	IDirect3DSurface9 *target = NULL, *backBuffer = NULL;
	D3DSURFACE_DESC desc = {};
	bool onBackBuffer = false;
	if (SUCCEEDED(dev->GetRenderTarget(0, &target)) && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
	{
		onBackBuffer = target == backBuffer;
		backBuffer->GetDesc(&desc);
	}
	if (target)
		target->Release();
	if (backBuffer)
		backBuffer->Release();
	if (!onBackBuffer || !desc.Height)
		return;

	float scale = desc.Height / 1080.0f;
	if (scale < 0.7f)
		scale = 0.7f;
	int px = (int)(20 * scale + 0.5f);
	if ((!g_font.tex || g_font.px != px) && !BuildFont(dev, px))
		return;
	if (!g_stateBlock && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock)))
		return;

	g_stateBlock->Capture();

	D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0, 1};
	dev->SetViewport(&vp);
	dev->SetPixelShader(NULL);
	dev->SetVertexShader(NULL);
	dev->SetFVF(VERTEX_FVF);
	dev->SetTexture(0, g_font.tex);
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
	dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

	g_device = dev;
	g_vertCount = 0;
	if (g_open)
		DrawMenu(scale);
	DrawToast(scale, (float)desc.Height);
	Flush();

	g_stateBlock->Apply();
}

// ---------------------------------------------------------------------------------------------
// Direct3D hooks
// ---------------------------------------------------------------------------------------------

static void StartScriptInjection();

typedef HRESULT(WINAPI *EndScene_t)(IDirect3DDevice9 *);
typedef HRESULT(WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

static EndScene_t g_origEndScene;
static Reset_t g_origReset;
static CreateDevice_t g_origCreateDevice;

static void PatchVtable(void *object, int index, void *hook, void **original)
{
	void **vtable = *(void ***)object;
	if (vtable[index] == hook)
		return;
	*original = vtable[index];
	DWORD old;
	VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

static HRESULT WINAPI HookEndScene(IDirect3DDevice9 *dev)
{
	static bool inside, greeted;
	if (!inside)
	{
		inside = true;
		if (!greeted)
		{
			greeted = true;
			Toast(g_cbuf ? "ZMenu WaW loaded - F7 opens the menu (Numpad 8/2/4/6, 5 select, 0 back)" : "ZMenu WaW: unsupported game version");
			g_toastUntil += 5000;
		}
		HandleInput();
		PumpFeatures();
		if (g_open || (int)(g_toastUntil - GetTickCount()) > 0)
			Render(dev);
		inside = false;
	}
	return g_origEndScene(dev);
}

static void ReleaseDeviceObjects()
{
	if (g_stateBlock)
		g_stateBlock->Release();
	g_stateBlock = NULL;
}

static HRESULT WINAPI HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
	ReleaseDeviceObjects();
	return g_origReset(dev, pp);
}

static HRESULT WINAPI HookCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
									   D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
	HRESULT hr = g_origCreateDevice(d3d, adapter, type, window, flags, pp, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		// objects of a previous device died with it
		g_stateBlock = NULL;
		g_font.tex = NULL;
		PatchVtable(*out, 42, (void *)HookEndScene, (void **)&g_origEndScene);
		PatchVtable(*out, 16, (void *)HookReset, (void **)&g_origReset);
	}
	return hr;
}

static HMODULE RealD3D9()
{
	static HMODULE real;
	if (!real)
	{
		char path[MAX_PATH];
		GetSystemDirectoryA(path, MAX_PATH);
		strcat(path, "\\d3d9.dll");
		real = LoadLibraryA(path);
	}
	return real;
}

// Exported under the undecorated d3d9.dll names (stdcall would otherwise append @N).
#define D3D9_EXPORT(name) __asm__("_" #name) __attribute__((dllexport))
extern "C"
{
IDirect3D9 *WINAPI zm_Direct3DCreate9(UINT sdkVersion) D3D9_EXPORT(Direct3DCreate9);
int WINAPI zm_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) D3D9_EXPORT(D3DPERF_BeginEvent);
int WINAPI zm_D3DPERF_EndEvent() D3D9_EXPORT(D3DPERF_EndEvent);
void WINAPI zm_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) D3D9_EXPORT(D3DPERF_SetMarker);
DWORD WINAPI zm_D3DPERF_GetStatus() D3D9_EXPORT(D3DPERF_GetStatus);
}

extern "C" IDirect3D9 *WINAPI zm_Direct3DCreate9(UINT sdkVersion)
{
	typedef IDirect3D9 *(WINAPI *Create_t)(UINT);
	Create_t create = (Create_t)GetProcAddress(RealD3D9(), "Direct3DCreate9");
	IDirect3D9 *d3d = create ? create(sdkVersion) : NULL;
	if (d3d && g_enabled)
		StartScriptInjection();
	if (d3d && g_enabled)
		PatchVtable(d3d, 16, (void *)HookCreateDevice, (void **)&g_origCreateDevice);
	return d3d;
}

extern "C" int WINAPI zm_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
	typedef int(WINAPI *Fn)(D3DCOLOR, LPCWSTR);
	Fn fn = (Fn)GetProcAddress(RealD3D9(), "D3DPERF_BeginEvent");
	return fn ? fn(color, name) : 0;
}

extern "C" int WINAPI zm_D3DPERF_EndEvent()
{
	typedef int(WINAPI *Fn)();
	Fn fn = (Fn)GetProcAddress(RealD3D9(), "D3DPERF_EndEvent");
	return fn ? fn() : 0;
}

extern "C" void WINAPI zm_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
	typedef void(WINAPI *Fn)(D3DCOLOR, LPCWSTR);
	Fn fn = (Fn)GetProcAddress(RealD3D9(), "D3DPERF_SetMarker");
	if (fn)
		fn(color, name);
}

extern "C" DWORD WINAPI zm_D3DPERF_GetStatus()
{
	typedef DWORD(WINAPI *Fn)();
	Fn fn = (Fn)GetProcAddress(RealD3D9(), "D3DPERF_GetStatus");
	return fn ? fn() : 0;
}

// ---------------------------------------------------------------------------------------------
// Script injection
//
// Stock scripts are loaded from common.ff as plain-text "rawfile" assets: { name *, length, buffer * }.
// A watcher thread finds the asset for maps/_load.gsc (run by every level) and swaps its buffer for a
// copy with ZMenuWaW.gsc appended and a call to zm_init() added to main().
// Nothing on disk is touched and the game is not started as a mod (fs_game makes this exe load every
// weapon from loose files, which stalls level loads).
// ---------------------------------------------------------------------------------------------

static const char kLoadScript[] = "maps/_load.gsc";
static const char kScriptHook[] = "\tzm_init();\r\n";

static char *g_scriptExtra;     // contents of ZMenuWaW.gsc
static DWORD *g_scriptHeader;   // rawfile header we patched
static char *g_scriptPatched;   // buffer it now points to

static char *ReadTextFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = (char *)malloc(size + 1);
	size = (long)fread(text, 1, size, f);
	text[size] = 0;
	fclose(f);
	return text;
}

static bool ScriptPatchIntact()
{
	return g_scriptHeader && Readable(g_scriptHeader, 12) && g_scriptHeader[2] == (DWORD)g_scriptPatched &&
		   Readable((void *)g_scriptHeader[0], sizeof(kLoadScript)) && memcmp((void *)g_scriptHeader[0], kLoadScript, sizeof(kLoadScript)) == 0;
}

// Calls visit(address, bytes, size) for every chunk of the game's read/write memory until it returns true.
// ReadProcessMemory keeps the walk safe against pages the game frees underneath us.
template <typename Visit>
static void ScanMemory(Visit visit)
{
	static BYTE chunk[0x100000 + 64];
	const SIZE_T kChunk = 0x100000, kOverlap = 32;
	HANDLE self = GetCurrentProcess();
	HMODULE exe = GetModuleHandleA(NULL);
	SYSTEM_INFO si;
	GetSystemInfo(&si);

	MEMORY_BASIC_INFORMATION mbi;
	for (BYTE *addr = (BYTE *)si.lpMinimumApplicationAddress; addr < (BYTE *)si.lpMaximumApplicationAddress && VirtualQuery(addr, &mbi, sizeof(mbi));)
	{
		BYTE *base = (BYTE *)mbi.BaseAddress, *end = base + mbi.RegionSize;
		addr = end;
		// zone data is private memory, the asset pools are in the exe's own data section
		bool wanted = mbi.Type == MEM_PRIVATE || (mbi.Type == MEM_IMAGE && mbi.AllocationBase == (void *)exe);
		if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE || !wanted)
			continue;

		for (BYTE *p = base; p < end; p += kChunk)
		{
			SIZE_T want = (SIZE_T)(end - p) < kChunk + kOverlap ? (SIZE_T)(end - p) : kChunk + kOverlap, got = 0;
			if (ReadProcessMemory(self, p, chunk, want, &got) && got >= kOverlap && visit((DWORD)p, chunk, got))
				return;
		}
	}
}

// The header lives in the engine's rawfile pool, away from the name and text it points to, so this takes
// two passes: find the "maps/_load.gsc" strings, then the { name, length, name + 15 } struct pointing at one.
static DWORD *FindLoadScriptHeader()
{
	DWORD names[16];
	int nameCount = 0;
	ScanMemory([&](DWORD addr, const BYTE *data, SIZE_T size) {
		const BYTE *limit = data + size - sizeof(kLoadScript);
		for (const BYTE *hit = data; hit <= limit && (hit = (const BYTE *)memchr(hit, 'm', limit - hit + 1)); hit++)
			if (memcmp(hit, kLoadScript, sizeof(kLoadScript)) == 0 && nameCount < 16)
				names[nameCount++] = addr + (DWORD)(hit - data);
		return false;
	});
	if (!nameCount)
		return NULL;

	DWORD *found = NULL;
	ScanMemory([&](DWORD addr, const BYTE *data, SIZE_T size) {
		const DWORD *words = (const DWORD *)data;
		for (SIZE_T i = 0; i + 3 <= size / 4; i++)
			for (int n = 0; n < nameCount; n++)
				if (words[i] == names[n] && words[i + 2] == names[n] + sizeof(kLoadScript) && words[i + 1] > 1000 && words[i + 1] < 0x400000)
				{
					found = (DWORD *)(addr + i * 4);
					return true;
				}
		return false;
	});
	return found;
}

static void PatchLoadScript(DWORD *header)
{
	const char *text = (const char *)header[2];
	size_t length = header[1];
	if (!Readable(text, length + 1))
		return;

	// insertion point: first line inside main()
	const char *entry = strstr(text, "main(");
	const char *brace = entry ? strchr(entry, '{') : NULL;
	const char *eol = brace ? strchr(brace, '\n') : NULL;
	if (!eol)
		return;
	size_t head = eol + 1 - text;

	size_t extra = strlen(g_scriptExtra), hook = sizeof(kScriptHook) - 1;
	char *patched = (char *)malloc(length + hook + extra + 8);
	char *out = patched;
	memcpy(out, text, head);
	out += head;
	memcpy(out, kScriptHook, hook);
	out += hook;
	memcpy(out, text + head, length - head);
	out += length - head;
	memcpy(out, "\r\n", 2);
	out += 2;
	memcpy(out, g_scriptExtra, extra + 1);
	out += extra;

	g_scriptPatched = patched;
	g_scriptHeader = header;
	header[2] = (DWORD)patched;
	header[1] = (DWORD)(out - patched);
	Log("script injected into %s (asset at %p)", kLoadScript, header);
}

static DWORD WINAPI ScriptThread(LPVOID)
{
	for (;;)
	{
		Sleep(250);
		if (ScriptPatchIntact())
			continue;
		DWORD *header = FindLoadScriptHeader();
		if (header)
			PatchLoadScript(header);
	}
}

static void StartScriptInjection()
{
	static bool started;
	if (started || !GetPrivateProfileIntA("Settings", "LoadScripts", 1, g_iniPath))
		return;
	started = true;

	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\ZMenuWaW.gsc", g_exeDir);
	g_scriptExtra = ReadTextFile(path);
	if (!g_scriptExtra)
	{
		Toast("ZMenuWaW.gsc is missing - script features are off");
		return;
	}
	CloseHandle(CreateThread(NULL, 0, ScriptThread, NULL, 0, NULL));
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);

		GetModuleFileNameA(NULL, g_exeDir, MAX_PATH);
		char *slash = strrchr(g_exeDir, '\\');
		if (!slash || _stricmp(slash + 1, "CoDWaW.exe") != 0)
			return TRUE;
		*slash = 0;
		g_enabled = true;
		snprintf(g_iniPath, sizeof(g_iniPath), "%s\\ZMenuWaW.ini", g_exeDir);

		char logPath[MAX_PATH];
		snprintf(logPath, sizeof(logPath), "%s\\ZMenuWaW.log", g_exeDir);
		DeleteFileA(logPath);

		LoadConfig();
		InitEngine();
		Log("ZMenu WaW " ZMENU_VERSION ": %s", g_native ? "game version recognised, all features available"
											   : g_cbuf	 ? "unknown game version, no clip / rapid fire / weapon spawner are off"
														 : "unsupported game version");
		BuildMenus();
		LoadHotkeys();
	}
	return TRUE;
}
