// ZMenu WaW - game-side half of the trainer.
// The menu DLL (d3d9.dll) appends this file to the stock maps/_load.gsc in memory and calls zm_init()
// from its main(). Every name here is prefixed zm_ so nothing collides with the stock script.
// The DLL talks to it through dvars:
//   zm_god / zm_ammo / zm_clip / zm_explosive / zm_notarget / zm_onehit   toggles, polled every frame
//   zm_rapid + zm_nade                                                   grenade fountain (nade: 1 = frag key held, 2 = special)
//   zm_grav / zm_flame                                                   gravity gun and flame stream modes
//   zm_team                                                              1 = every AI hostile, 2 = every AI friendly
//   zm_cmd + zm_seq                                                       one-shot commands (seq changes per command)

zm_init()
{
	// LoadFx has to happen before the first wait; we run on the first line of _load::main.
	level._effect["zmenu_exp"] = LoadFx( "explosions/grenadeExp_dirt" );
	level._effect["zmenu_exp_big"] = LoadFx( "explosions/large_vehicle_explosion" );
	level._effect["zmenu_fire"] = LoadFx( "env/fire/fx_fire_player_sm" );

	level thread zm_run();
}

zm_run()
{
	level.zm_seq = GetDvarInt( "zm_seq" );

	// Quick Load runs this through "vstr": manual saves are named <level>-<name>
	SetDvar( "zm_load", "loadgame " + ToLower( GetDvar( "mapname" ) ) + "-zmenu_quick" );
	level.zm_tick = 0;

	for( ;; )
	{
		wait( 0.05 );
		level.zm_tick++;

		players = GetPlayers();
		if( players.size == 0 )
		{
			continue;
		}

		for( i = 0; i < players.size; i++ )
		{
			if( !IsDefined( players[i].zm_setup ) )
			{
				players[i].zm_setup = true;
				players[i] thread zm_explosive_watch();
				players[i] thread zm_gravgun();
			}

			players[i] zm_player_tick();
		}

		seq = GetDvarInt( "zm_seq" );
		if( seq != level.zm_seq )
		{
			level.zm_seq = seq;
			// cleared so a quick-loaded save (which restores an older zm_seq) cannot replay it
			cmd = GetDvar( "zm_cmd" );
			SetDvar( "zm_cmd", "" );
			level thread zm_command( cmd );
		}

		if( level.zm_tick % 5 == 0 )
		{
			if( GetDvarInt( "zm_onehit" ) )
			{
				zm_weaken_enemies();
			}
			zm_team_mode( GetDvarInt( "zm_team" ) );
		}
	}
}

zm_player_tick()
{
	if( GetDvarInt( "zm_god" ) )
	{
		self EnableInvulnerability();
		self.zm_god = true;
	}
	else if( IsDefined( self.zm_god ) )
	{
		self DisableInvulnerability();
		self.zm_god = undefined;
	}

	if( GetDvarInt( "zm_notarget" ) )
	{
		self.ignoreme = true;
		self.zm_notarget = true;
	}
	else if( IsDefined( self.zm_notarget ) )
	{
		self.ignoreme = false;
		self.zm_notarget = undefined;
	}

	if( GetDvarInt( "zm_ammo" ) && level.zm_tick % 4 == 0 )
	{
		self zm_refill_ammo();
	}

	if( GetDvarInt( "zm_clip" ) )
	{
		weapon = self GetCurrentWeapon();
		if( IsDefined( weapon ) && weapon != "none" )
		{
			self SetWeaponAmmoClip( weapon, WeaponClipSize( weapon ) );
		}
	}

	// Rapid Fire takes over the grenade keys: normal throws are off, holding a key is a fountain.
	if( GetDvarInt( "zm_rapid" ) )
	{
		if( !IsDefined( self.zm_rapid ) )
		{
			self DisableOffhandWeapons();
			self.zm_rapid = true;
		}

		nade = GetDvarInt( "zm_nade" );
		if( nade )
		{
			// own thread: a script error in there must not take the main loop down
			self thread zm_throw_grenade( nade == 2 );
		}
	}
	else if( IsDefined( self.zm_rapid ) )
	{
		self EnableOffhandWeapons();
		self.zm_rapid = undefined;
	}

	if( GetDvarInt( "zm_flame" ) && self AttackButtonPressed() && level.zm_tick % 2 == 0 )
	{
		self thread zm_flame_puff();
	}
}

// Flame stream for levels that do not ship the real flamethrower: fire along the aim line and
// burn damage at the end of it, so enemies go down with the game's own on-fire deaths.
zm_flame_puff()
{
	eye = self GetEye();
	forward = AnglesToForward( self GetPlayerAngles() );
	trace = BulletTrace( eye, eye + forward * 650, true, self );
	end = trace["position"];
	reach = Distance( eye, end );

	for( d = 110; d < reach; d += 130 )
	{
		PlayFX( level._effect["zmenu_fire"], eye + forward * d + ( 0, 0, -14 ) );
	}
	PlayFX( level._effect["zmenu_fire"], end );

	self EnableInvulnerability();
	RadiusDamage( end, 120, 70, 35, self, "MOD_BURNED" );
	if( !GetDvarInt( "zm_god" ) )
	{
		self DisableInvulnerability();
	}
}

// Everyone Hostile / Everyone Friendly: 1 puts every AI on the enemy team, 2 on yours, 0 puts them back.
zm_team_mode( mode )
{
	if( !mode && !IsDefined( level.zm_team_mode ) )
	{
		return;
	}

	ai = GetAIArray();
	for( i = 0; i < ai.size; i++ )
	{
		if( !IsAlive( ai[i] ) )
		{
			continue;
		}
		if( !IsDefined( ai[i].zm_team ) )
		{
			ai[i].zm_team = ai[i].team;
		}

		team = ai[i].zm_team;
		if( mode == 1 )
		{
			team = "axis";
		}
		else if( mode == 2 )
		{
			team = "allies";
		}

		if( ai[i].team != team )
		{
			ai[i].team = team;
		}
	}

	level.zm_team_mode = true;
	if( !mode )
	{
		level.zm_team_mode = undefined;
	}
}

// Gravity gun. While the mode is on your weapons are holstered:
//   hold aim  = grab the soldier you are looking at and carry him in front of you (release to drop)
//   fire      = throw him, or shove everything loose in front of you when holding nobody
zm_gravgun()
{
	self endon( "disconnect" );

	held = undefined;
	mover = undefined;
	active = false;
	fire_was_down = false;

	for( ;; )
	{
		wait( 0.05 );

		on = GetDvarInt( "zm_grav" );
		if( on != active )
		{
			active = on;
			if( on )
			{
				self DisableWeapons();
			}
			else
			{
				self EnableWeapons();
			}
		}

		eye = self GetEye();
		forward = AnglesToForward( self GetPlayerAngles() );
		fire = on && self AttackButtonPressed();
		fire_hit = fire && !fire_was_down;
		fire_was_down = fire;

		// let go: mode switched off, aim released, thrown, or the guy died / vanished
		if( IsDefined( mover ) && ( !on || !self AdsButtonPressed() || !IsDefined( held ) || !IsAlive( held ) || fire_hit ) )
		{
			if( IsDefined( held ) && IsAlive( held ) )
			{
				held Unlink();
				held.ignoreall = false;
				if( fire_hit )
				{
					self thread zm_grav_throw( held, forward );
				}
			}
			mover Delete();
			mover = undefined;
			held = undefined;
			continue;
		}

		if( !on )
		{
			continue;
		}

		if( IsDefined( mover ) )
		{
			mover MoveTo( eye + forward * 140 + ( 0, 0, -40 ), 0.1 );
			continue;
		}

		if( self AdsButtonPressed() )
		{
			trace = BulletTrace( eye, eye + forward * 2500, true, self );
			target = trace["entity"];
			if( IsDefined( target ) && IsAI( target ) && IsAlive( target ) )
			{
				held = target;
				mover = Spawn( "script_origin", held.origin );
				held LinkTo( mover );
				held.ignoreall = true;
			}
		}
		else if( fire_hit )
		{
			// nobody held: shove bodies and loose props away
			PhysicsExplosionSphere( eye + forward * 60, 420, 200, 7 );
			Earthquake( 0.2, 0.3, eye, 300 );
		}
	}
}

zm_grav_throw( guy, forward )
{
	// the unkillable story characters are only put down again
	if( IsDefined( guy.magic_bullet_shield ) )
	{
		return;
	}

	guy.a.nodeath = true;
	guy StartRagdoll();
	guy LaunchRagdoll( forward * 260 + ( 0, 0, 70 ) );
	wait( 0.05 );
	if( IsDefined( guy ) && IsAlive( guy ) )
	{
		guy DoDamage( guy.health + 1000, guy.origin, self );
	}
}

// One grenade of the fountain. special = the player's second grenade type (smoke, molotov...),
// falling back to the frag when the level did not hand one out.
zm_throw_grenade( special )
{
	frag = self GetCurrentOffhand();
	other = undefined;

	weapons = self GetWeaponsList();
	primaries = self GetWeaponsListPrimaries();
	for( i = 0; i < weapons.size; i++ )
	{
		is_primary = false;
		for( j = 0; j < primaries.size; j++ )
		{
			if( primaries[j] == weapons[i] )
			{
				is_primary = true;
			}
		}

		if( is_primary || weapons[i] == "syrette" )
		{
			continue;
		}

		if( !IsDefined( frag ) || frag == "none" )
		{
			frag = weapons[i];
		}
		else if( weapons[i] != frag )
		{
			other = weapons[i];
		}
	}

	weapon = frag;
	if( special && IsDefined( other ) )
	{
		weapon = other;
	}
	if( !IsDefined( weapon ) || weapon == "none" )
	{
		return;
	}

	forward = AnglesToForward( self GetPlayerAngles() );
	velocity = forward * RandomFloatRange( 650, 950 );
	velocity += ( RandomFloatRange( -130, 130 ), RandomFloatRange( -130, 130 ), RandomFloatRange( 220, 420 ) );

	self MagicGrenadeType( weapon, self GetEye() + forward * 40, velocity, RandomFloatRange( 2, 3.5 ) );
}

zm_refill_ammo()
{
	weapons = self GetWeaponsList();
	for( i = 0; i < weapons.size; i++ )
	{
		self GiveMaxAmmo( weapons[i] );
	}
}

zm_weaken_enemies()
{
	ai = GetAIArray( "axis" );
	for( i = 0; i < ai.size; i++ )
	{
		// scripted "bullet shield" characters sit at huge health values, leave those alone
		if( IsAlive( ai[i] ) && ai[i].health > 1 && ai[i].health < 10000 )
		{
			ai[i].health = 1;
		}
	}
}

// Where the player is aiming, pulled back a little so things placed there are not inside a wall.
zm_aim_pos( pullback )
{
	eye = self GetEye();
	forward = AnglesToForward( self GetPlayerAngles() );
	trace = BulletTrace( eye, eye + forward * 12000, true, self );
	return trace["position"] - forward * pullback;
}

zm_explosive_watch()
{
	self endon( "disconnect" );

	for( ;; )
	{
		self waittill( "weapon_fired" );

		mode = GetDvarInt( "zm_explosive" );
		if( !mode )
		{
			continue;
		}

		zm_explode( self zm_aim_pos( 4 ), self, mode > 1 );
	}
}

zm_explode( pos, attacker, big )
{
	if( big )
	{
		fx = level._effect["zmenu_exp_big"];
		sound = "car_explode";
		radius = 600;
		damage = 2500;
	}
	else
	{
		fx = level._effect["zmenu_exp"];
		sound = "barrel_explode";
		radius = 260;
		damage = 600;
	}

	PlayFX( fx, pos );
	PlaySoundAtPosition( sound, pos );

	// the shooter is immune to their own blast
	attacker EnableInvulnerability();
	RadiusDamage( pos, radius, damage, damage * 0.25, attacker );
	if( !GetDvarInt( "zm_god" ) )
	{
		attacker DisableInvulnerability();
	}

	PhysicsExplosionSphere( pos, radius, radius * 0.5, 3 );
	Earthquake( 0.3, 0.5, pos, radius * 3 );
}

zm_command( cmd )
{
	tok = StrTok( cmd, " " );
	if( tok.size == 0 )
	{
		return;
	}

	players = GetPlayers();
	if( players.size == 0 )
	{
		return;
	}
	player = players[0];

	switch( tok[0] )
	{
	case "spawn":
		match = "axis";
		if( tok[1] == "allies" )
		{
			match = "ally";
		}
		zm_spawn_ai( player zm_aim_pos( 48 ), match, Int( tok[2] ), tok[1], undefined );
		break;

	case "royale":
		// both sides dropped on the same spot
		pos = player zm_aim_pos( 48 );
		level thread zm_spawn_ai( pos + ( 90, 0, 0 ), "axis", Int( tok[1] ), "axis", undefined );
		zm_spawn_ai( pos - ( 90, 0, 0 ), "ally", Int( tok[1] ), "allies", undefined );
		break;

	case "dogs":
		zm_spawn_ai( player zm_aim_pos( 48 ), "dog", Int( tok[1] ), "dog", "allies" );
		break;

	case "save":
		// same two steps as the stock autosave; SaveGame() alone never reaches the disk
		save_id = SaveGameNoCommit( "zmenu_quick", &"AUTOSAVE_AUTOSAVE" );
		wait( 1 );
		CommitSave( save_id );
		break;

	case "airstrike":
		zm_airstrike( player, player zm_aim_pos( 4 ), tok[1] );
		break;

	case "killall":
		zm_kill_ai( tok[1] );
		break;

	case "tp":
		player SetOrigin( player zm_aim_pos( 32 ) );
		break;

	case "boom":
		zm_explode( player zm_aim_pos( 4 ), player, Int( tok[1] ) > 1 );
		break;

	case "ammo":
		for( i = 0; i < players.size; i++ )
		{
			players[i] zm_refill_ammo();
		}
		break;

	case "disarm":
		player TakeAllWeapons();
		break;

	case "give":
		player GiveWeapon( tok[1] );
		player GiveMaxAmmo( tok[1] );
		player SwitchToWeapon( tok[1] );
		break;

	case "arm":
		// "give all" does not draw anything when the player was empty-handed
		wait( 0.1 );
		if( player GetCurrentWeapon() == "none" )
		{
			weapons = player GetWeaponsListPrimaries();
			if( weapons.size > 0 )
			{
				player SwitchToWeapon( weapons[0] );
			}
		}
		break;

	case "points":
		arg = Int( tok[1] );
		for( i = 0; i < players.size; i++ )
		{
			if( IsDefined( players[i].score ) )
			{
				players[i].score += arg;
				if( IsDefined( players[i].score_total ) )
				{
					players[i].score_total += arg;
				}
			}
		}
		break;
	}
}

// Spawns AI at pos from the level's own spawners whose classname contains match
// ("axis", "ally", "dog"). force_team, when given, moves the new AI onto that team.
zm_spawn_ai( pos, match, count, label, force_team )
{
	spawners = GetSpawnerArray();
	candidates = [];
	for( i = 0; i < spawners.size; i++ )
	{
		if( IsDefined( spawners[i].classname ) && IsSubStr( ToLower( spawners[i].classname ), match ) )
		{
			candidates[candidates.size] = spawners[i];
		}
	}

	if( candidates.size == 0 )
	{
		IPrintLnBold( "ZMenu: this level has no " + label + " spawners" );
		return;
	}

	spawned = 0;

	for( n = 0; n < count; n++ )
	{
		spawner = candidates[RandomInt( candidates.size )];

		// borrow the spawner: move it to the crosshair, spawn, put it back.
		// Its target and goal volume are hidden meanwhile so the new guy fights here instead of
		// running off to wherever the level wanted him.
		old_origin = spawner.origin;
		old_count = spawner.count;
		old_target = spawner.target;
		old_volume = spawner.script_goalvolume;
		spawner.count = 1;
		spawner.target = undefined;
		spawner.script_goalvolume = undefined;
		spawner.origin = pos + ( RandomFloatRange( -72, 72 ), RandomFloatRange( -72, 72 ), 4 );

		guy = spawner StalingradSpawn();

		spawner.origin = old_origin;
		spawner.target = old_target;
		spawner.script_goalvolume = old_volume;
		if( IsDefined( old_count ) )
		{
			spawner.count = old_count;
		}

		if( maps\_utility::spawn_failed( guy ) )
		{
			continue;
		}

		if( IsDefined( force_team ) )
		{
			guy.team = force_team;
			guy.zm_team = force_team;
		}
		guy.goalradius = 512;
		guy SetGoalPos( guy.origin );
		spawned++;
	}

	IPrintLn( "ZMenu: spawned " + spawned + " " + label );
}

// A dozen strikes scattered around the target a moment after it is called in. rocket is a
// projectile weapon to fire down from the sky, or "none" to just set the blasts off.
zm_airstrike( player, target, rocket )
{
	IPrintLnBold( "Airstrike inbound" );
	wait( 1.5 );

	for( i = 0; i < 12; i++ )
	{
		spot = target + ( RandomFloatRange( -380, 380 ), RandomFloatRange( -380, 380 ), 0 );
		ground = BulletTrace( spot + ( 0, 0, 500 ), spot - ( 0, 0, 800 ), false, undefined );
		spot = ground["position"];

		if( rocket != "none" )
		{
			MagicBullet( rocket, spot + ( RandomFloatRange( -300, 300 ), RandomFloatRange( -300, 300 ), 2200 ), spot, player );
		}
		else
		{
			zm_explode( spot, player, true );
		}

		wait( RandomFloatRange( 0.12, 0.3 ) );
	}
}

zm_kill_ai( team )
{
	ai = GetAIArray( team );
	for( i = 0; i < ai.size; i++ )
	{
		if( IsAlive( ai[i] ) )
		{
			ai[i] DoDamage( ai[i].health + 1000, ai[i].origin );
		}
	}
}
