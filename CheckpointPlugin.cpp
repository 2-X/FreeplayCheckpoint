/*
 * Copyright (c) 2021
 * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pch.h"
#include "CheckpointPlugin.h"

#include "bakkesmod/wrappers/GameEvent/TutorialWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarComponent/BoostWrapper.h"
#include "bakkesmod/wrappers/GameObject/CarWrapper.h"
#include "bakkesmod/wrappers/GameObject/BallWrapper.h"

#include "bakkesmod/wrappers/ArrayWrapper.h"

using namespace std::placeholders;

std::string_view DEFAULT_SAVE_FILE_NAME = "freeplaycheckpoint.data";
std::string_view DEFAULT_MATCH_SAVE_FILE_NAME = "matchcheckpoint.data";
// Loading a match checkpoint never leaves less than this on the clock, so the
// match does not end in the middle of a repeated drill.
constexpr float MATCH_MIN_LOAD_TIME = 60.0f;

BAKKESMOD_PLUGIN(CheckpointPlugin, "Freeplay Checkpoint", plugin_version, PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

// Controlled by cvars.
float snapshotInterval = 0.010f; //time (s) between updates
int historyTime = 30; // length of history (s)
int maxHistory = int(historyTime / snapshotInterval); // length of history (GameStates)

void CheckpointPlugin::log(std::string s) {
	if (debug) {
		cvarManager->log(s);
	}
}

void CheckpointPlugin::boolvar(std::string name, std::string desc, bool *var) {	
	auto cv = cvarManager->registerCvar(name, "0", desc, true, true, 0, true, 1);
	cv.addOnValueChanged([this, var](std::string old, CVarWrapper now) {
		*var = now.getBoolValue();
	});
	cv.notify();
}

std::unique_ptr<GameState> CheckpointPlugin::getReplayGameState() {
	ReplayServerWrapper replay = gameWrapper->GetGameEventAsReplay();
	if (!replay) {
		cvarManager->log("Error getting replay");
		return nullptr;
	}
	BallWrapper ball = replay.GetBall();
	if (!ball) {
		cvarManager->log("Error getting ball");
		return nullptr;
	}
	CameraWrapper cam = gameWrapper->GetCamera();
	if (!cam) {
		cvarManager->log("Error getting camera");
		return nullptr;
	}
	PriWrapper specPRI = PriWrapper(reinterpret_cast<std::uintptr_t>(cam.GetViewTarget().PRI));
	if (!specPRI) {
		cvarManager->log("Error getting PRI");
		return nullptr;
	}
	std::string playerName = specPRI.GetPlayerName().ToString();

	ArrayWrapper<CarWrapper> cars = replay.GetCars();
	for (int i = 0; i < cars.Count(); i++) {
		CarWrapper car = cars.Get(i);
		if (!car) {
			continue;
		}
		PriWrapper pri = car.GetPRI();
		if (!pri) {
			continue;
		}
		if (playerName == pri.GetPlayerName().ToString()) {
			return std::unique_ptr<GameState>(new GameState(car, ball));
		}
	}
	return nullptr;
}

// While frozen / rewinding, scrubbing through history can carry the ball through a goal,
// and the game would score it even though nothing is being played. In freeplay BakkesMod's
// own goal scoring switch (sv_soccar_enablegoal) is turned off for the duration and put
// back afterwards; in offline matches there is no such switch, so keepBallOutOfGoal()
// holds the physical ball just outside the goal while scrubbing instead.
void CheckpointPlugin::suppressGoals(bool on) {
	if (on == goalsSuppressed) {
		return;
	}
	if (inMatch() && on) {
		return; // handled by keepBallOutOfGoal() in the rewind tick
	}
	CVarWrapper cv = cvarManager->getCvar("sv_soccar_enablegoal");
	if (cv.IsNull()) {
		return;
	}
	if (on) {
		savedEnableGoal = cv.getBoolValue();
		if (!savedEnableGoal) {
			return; // already off (as the README recommends); nothing to restore later
		}
		cv.setValue(false);
	} else {
		cv.setValue(savedEnableGoal);
	}
	goalsSuppressed = on;
}

// The state to show while scrubbing an offline match: if the ball is inside a goal,
// keep it just outside the goal line so the game never registers a goal. The true
// state (`latest`) is untouched - resuming play applies it as recorded.
GameState CheckpointPlugin::keepBallOutOfGoal(const GameState& s) {
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull() || !sw.IsInGoal(s.ball.location)) {
		return s;
	}
	GameState shown = s;
	// Goal lines are at |Y| = 5120; the ball (radius ~93) is "in" once its centre is past
	// the line. Hold its centre a ball's width in front of the line.
	float limit = 5120.f - 100.f;
	if (shown.ball.location.Y > limit) {
		shown.ball.location.Y = limit;
	} else if (shown.ball.location.Y < -limit) {
		shown.ball.location.Y = -limit;
	}
	shown.ball.velocity = Vector(0, 0, 0);
	return shown;
}

void CheckpointPlugin::setFrozen(bool car, bool ball) {
	if (rewindMode && !car) {
		// Play resumes from `latest`: the pads follow, including what happened to them while frozen.
		applyBoostPads(latest);
		if (!ball && noGoalsFrozen && inMatch()) {
			// While scrubbing a match the shown ball may have been held outside the goal;
			// resuming must start from the ball as actually recorded.
			ServerWrapper sw = gameWrapper->GetGameEventAsServer();
			if (!sw.IsNull() && !sw.GetBall().IsNull()) {
				latest.ball.apply(sw.GetBall());
			}
		}
		if (!ball && inMatch() && settleMs > 0) {
			// Hold the resumed situation briefly so the bots act on it, not on where
			// play was before the load (see settleMs).
			ServerWrapper sw = gameWrapper->GetGameEventAsServer();
			if (!sw.IsNull()) {
				settling = true;
				settleUntil = sw.GetSecondsElapsed() + settleMs / 1000.f;
			}
		}
	}
	if (noGoalsFrozen) {
		if (car && !rewindMode) {
			suppressGoals(true);
		} else if (!car && rewindMode) {
			suppressGoals(false);
		}
	}
	rewindMode = car;
	freezeBall = ball;
	cvarManager->getCvar("cpt_car_frozen").setValue(car);
	cvarManager->getCvar("cpt_ball_frozen").setValue(ball);
}

void CheckpointPlugin::onLoad()
{
	boolvar("cpt_clean_history", "If set, deletes history after the current point when exiting rewind mode", &deleteFutureHistory);

	boolvar("cpt_reset_on_goal", "If set, restore last resumed checkpoint when scoring a goal", &resetOnGoal);
	boolvar("cpt_no_goals_frozen", "If set, a goal can never be scored while frozen / rewinding", &noGoalsFrozen);
	boolvar("cpt_reset_on_ball_ground", "If set, restore last resumed checkpoint when ball touches ground", &resetOnBallGround);
	boolvar("cpt_next_instead_of_reset", "If set, load next checkpoint instead of resetting", &nextInsteadOfReset);

	boolvar("cpt_debug", "If set, render debugging info", &debug);

	boolvar("cpt_next_prev_when_frozen", "LEGACY; DO NOT USE", &ignorePNNotFrozen);
	boolvar("cpt_ignore_next", "If set, ignore next when not frozen", &ignorePrev);
	boolvar("cpt_ignore_prev", "If set, ignore prev when not frozen", &ignoreNext);
	boolvar("cpt_ignore_freeze_ball", "If set, ignore freeze ball when not frozen", &ignoreFreezeBall);
	boolvar("cpt_disable_training", "If set, disable in custom training", &disableTraining);
	boolvar("cpt_disable_workshop", "If set, disable in workshop", &disableWorkshop);
	boolvar("cpt_show_boost", "If set, show player boost usage while rewinding", &showBoost);

	// Migration from cpt_next_prev_when_frozen to split variables.
	if (ignorePNNotFrozen) {
		cvarManager->getCvar("cpt_next_prev_when_frozen").setValue(false);
		cvarManager->getCvar("cpt_ignore_prev").setValue(true);
		cvarManager->getCvar("cpt_ignore_next").setValue(true);
		cvarManager->getCvar("cpt_ignore_freeze_ball").setValue(true);
	}

	boolvar("cpt_mirror_loads", "If set, randomly mirror when loading checkpoints", &mirrorLoads);
	boolvar("cpt_randomize_loads", "If set, load a random checkpoint instead of the latest", &randomizeLoads);

	auto matchCV = cvarManager->registerCvar("cpt_enable_match", "1", "If set, also work in offline exhibition / RLBot matches (all cars are frozen and restored)", true, true, 0, true, 1);
	matchCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		matchEnabled = now.getBoolValue();
	});
	auto settleCV = cvarManager->registerCvar("cpt_match_resume_settle_ms", "300", "Offline matches: hold the situation this long after resuming so bots take it in before play starts (0 = off)", true, true, 0, true, 1000);
	settleCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		settleMs = now.getIntValue();
	});
	auto padsCV = cvarManager->registerCvar("cpt_match_boost_pads", "1", "If set, boost pads are restored with the rest of the situation in offline matches", true, true, 0, true, 1);
	padsCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		restorePads = now.getBoolValue();
	});
	auto teamCV = cvarManager->registerCvar("cpt_match_team_aware", "1", "If set, a match checkpoint saved on the other team is loaded turned around, so the shot stays on your side of the field", true, true, 0, true, 1);
	teamCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		matchTeamAware = now.getBoolValue();
	});
	registerBoostPadHooks();
	registerBotCameraHooks();
	registerDemolitionHooks();

	// Register CVars for action thresholds and enable/disable toggles
	cvarManager->registerCvar("enable_throttle_unpause", "1", "Enable throttle to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("throttle_threshold", "0.1", "Threshold for throttle to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("enable_steer_unpause", "0", "Enable steering to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("steer_threshold", "0.1", "Threshold for steering to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("enable_pitch_unpause", "1", "Enable pitch to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("pitch_threshold", "0.80", "Threshold for pitch to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("enable_yaw_unpause", "0", "Enable yaw to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("yaw_threshold", "0.1", "Threshold for yaw to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("enable_roll_unpause", "1", "Enable roll to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("roll_threshold", "0.1", "Threshold for roll to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("rewind_threshold", "0.05", "Threshold for the rewind input to unpause", true, true, 0, true, 1, true);

	cvarManager->registerCvar("enable_handbrake_unpause", "1", "Enable handbrake to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("enable_jump_unpause", "1", "Enable jump to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("enable_boost_activate_unpause", "1", "Enable boost activation to unpause", true, true, 0, true, 1, true);
	cvarManager->registerCvar("enable_boost_hold_unpause", "1", "Enable boost hold to unpause", true, true, 0, true, 1, true);


	cvarManager->registerCvar("rewind_axis", "steer", "Input used for rewinding (steer, throttle, pitch, yaw, roll)");
	cvarManager->registerCvar("matching_axis", "pitch", "Your bind that is on the same stick as the rewind input");

	// original cvars
	cvarManager->registerCvar("cpt_allow_delete_all", "0", "Enables the delete all button", false, true, 0, true, 1, false);

	cvarManager->registerCvar("cpt_car_frozen", "0", "Set when the car is frozen; read-only", false, true, 0, true, 1, false);
	cvarManager->registerCvar("cpt_ball_frozen", "0", "Set when the ball is frozen; read-only", false, true, 0, true, 1, false);

	auto snapshotIntervalCV = cvarManager->registerCvar(
		"cpt_snapshot_interval", "1", "Collect a snapshot every <n> milliseconds; changing deletes history", true, true, 1, true, 10, true);
	snapshotIntervalCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		snapshotInterval = now.getIntValue()/100.0f;
		maxHistory = int(historyTime / snapshotInterval);
		history.resize(0);
		setFrozen(false, false);
		dodgeExpiration = 0.0;
	});
	snapshotIntervalCV.notify();

	auto historyLenCV = cvarManager->registerCvar(
		"cpt_history_length", "30", "Save history for <n> seconds", true, true, 10, true, 120, true);
	historyLenCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		historyTime = now.getIntValue();
		maxHistory = int(historyTime / snapshotInterval);
		if (history.size() > maxHistory) {
			history.erase(history.begin(), history.begin() + history.size() - maxHistory);
		}
	});
	historyLenCV.notify();

	auto filenameCV = cvarManager->registerCvar(
		"cpt_filename", static_cast<std::string>(DEFAULT_SAVE_FILE_NAME), "Sets the filename to use for saved checkpoints; every workshop map adds its name to it", true, false, 0, false, 0, true);
	filenameCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		if (matchStoreActive) {
			return; // read when freeplay is entered again
		}
		setFrozen(false, false);
		curCheckpoint = 0;
		storePositions.clear();
		loadCheckpointFile();
	});
	snapshotIntervalCV.notify();

	auto matchFilenameCV = cvarManager->registerCvar(
		"cpt_match_filename", static_cast<std::string>(DEFAULT_MATCH_SAVE_FILE_NAME), "Sets the filename to use for checkpoints saved in offline matches; every team size (1v1, 2v2, ...) adds its name to it", true, false, 0, false, 0, true);
	matchFilenameCV.addOnValueChanged([this](std::string old, CVarWrapper now) {
		if (!matchStoreActive) {
			return; // read when a match is entered
		}
		setFrozen(false, false);
		curCheckpoint = 0;
		storePositions.clear();
		loadCheckpointFile();
	});

	auto resetDelayCV = cvarManager->registerCvar(
		"cpt_load_after_reset", "0", "Load last checkpoint on reset if loaded within last N seconds", true, true, 0, false, 0, true);

	registerVarianceCVars();

	loadCheckpointFile();

	// Continually call OnPreAsync.
	gameWrapper->HookEvent("Function PlayerController_TA.Driving.PlayerMove",
		bind(&CheckpointPlugin::OnPreAsync, this, _1));

	// Disable rewind mode if the user resets freeplay.
	// Or load latest checkpoint if within N seconds.
	gameWrapper->HookEvent("Function GameEvent_TA.Countdown.BeginState",
		[this](std::string eventName) {
			if (!enabledLoads()) {
				return;
			}
			if (inMatch()) {
				// Kickoff countdown, not a freeplay reset.  Every car is back for it.
				forgetDemolishedCars();
				parkedCars.clear();
				playingFromCheckpoint = false;
				setFrozen(false, false);
				dodgeExpiration = 0.0;
				return;
			}
			int resetDelay = cvarManager->getCvar("cpt_load_after_reset").getIntValue();
			if (!rewindMode && playingFromCheckpoint && resetDelay > 0) {
				ServerWrapper sw = gameWrapper->GetGameEventAsServer();
				float lastLoad = sw.GetSecondsElapsed() - lastRewindTime;
				if (lastLoad > 0 && lastLoad < resetDelay) {
					loadLatestCheckpoint();
					return;
				}
			}
			playingFromCheckpoint = false;
			setFrozen(false, false);
			dodgeExpiration = 0.0;
		});

	gameWrapper->HookEvent("Function TAGame.Ball_TA.OnHitGoal",
		[this](std::string eventName) {
			if (inMatch()) {
				// The goal counts; replay the checkpoint once the kickoff starts.
				if (!rewindMode && playingFromCheckpoint && resetOnGoal) {
					pendingMatchLoad = true;
				}
				return;
			}
			if (!gameWrapper->IsInFreeplay() || rewindMode || !playingFromCheckpoint || !resetOnGoal) {
				return;
			}
			loadLatestCheckpoint();
		});

	// Enter rewind mode.
	cvarManager->registerNotifier("cpt_freeze", [this](std::vector<std::string> command) {
		if (!enabled() || history.size() == 0 || rewindMode || gameWrapper->IsInReplay() || matchBlocked()) {
			return;
		}
		latest = history.back();
		loadGameState(latest);
	}, "Activates rewind mode", PERMISSION_ALL);

	// If in play mode, load the latest checkpoint / quick checkpoint.
	// If in rewind mode, add a checkpoint or delete the current checkpoint.
	cvarManager->registerNotifier("cpt_do_checkpoint", std::bind(&CheckpointPlugin::doCheckpoint, this, _1), "Saves/restores/removes a checkpoint", PERMISSION_ALL);

	cvarManager->registerNotifier("cpt_lock_checkpoint", std::bind(&CheckpointPlugin::lockCheckpoint, this, _1), "Lock/unlock a checkpoint", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_prev_checkpoint", std::bind(&CheckpointPlugin::prevCheckpoint, this, _1), "Loads the previous checkpoint", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_next_checkpoint", std::bind(&CheckpointPlugin::nextCheckpoint, this, _1), "Loads the next checkpoint", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_rand_checkpoint", std::bind(&CheckpointPlugin::randCheckpoint, this, _1), "Restores a random saved checkpoint", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_delete_all", std::bind(&CheckpointPlugin::deleteAllCheckpoints, this, _1), "Deletes ALL checkpoints", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_mirror_state", std::bind(&CheckpointPlugin::mirrorState, this, _1), "Mirrors the current frozen state", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_checkpoint_team", std::bind(&CheckpointPlugin::checkpointTeam, this, _1), "Tags the loaded match checkpoint with the team it was saved on: no argument = as shown, flip = the other way around, or blue / orange", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_checkpoint_team_all", std::bind(&CheckpointPlugin::checkpointTeamAll, this, _1), "Tags every match checkpoint of this mode that has no team yet (team ?) with blue / orange; add 'force' to re-tag all of them", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_freeze_ball", std::bind(&CheckpointPlugin::freezeBallUnfreezeCar, this, _1), "Freezes/unfreezes the ball", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_ball_in_front", std::bind(&CheckpointPlugin::ballInFront, this, _1), "Puts the ball in front of your car", PERMISSION_ALL);
	cvarManager->registerCvar("cpt_ball_front_distance", "200", "How far ahead of the car cpt_ball_in_front puts the ball", true, true, 150, true, 1500, true);
	cvarManager->registerNotifier("cpt_copy", std::bind(&CheckpointPlugin::copyShot, this, _1), "Copies the frozen state / quick checkpoint / last checkpoint to the clipboard", PERMISSION_ALL);
	cvarManager->registerNotifier("cpt_paste", std::bind(&CheckpointPlugin::pasteShot, this, _1), "Loads a checkpoint from the clipboard as a quick checkpoint", PERMISSION_FREEPLAY);

	// Add default bindings.
	registerBindingCVars();

	// Draw the checkpoint or notification about checkpoint deletion.
	gameWrapper->RegisterDrawable(std::bind(&CheckpointPlugin::Render, this, std::placeholders::_1));

	writeSettingsFile();
}

bool CheckpointPlugin::inMatch() {
	return matchEnabled && isOfflineMatch(gameWrapper);
}

// Keeps track of which cars are demolished (see carAlive()).  A demolished car's
// actor is not destroyed right away: it lingers through the explosion with its
// physics gone, still as its PRI's car, and moving it crashed the game whenever a
// checkpoint was loaded or scrubbed at that moment.
void CheckpointPlugin::registerDemolitionHooks() {
	auto demolished = [this](CarWrapper caller, void* params, std::string eventName) {
		if (caller.IsNull()) {
			return;
		}
		if (noteCarDemolished(caller.memory_address)) {
			log(fmt::format("car {:#x} demolished ({})", caller.memory_address, eventName));
		}
	};
	// Every way a car gets demolished goes through one of these; the last one runs
	// when the explosion is over and the actor is about to be destroyed.
	for (const char* fn : { "Function TAGame.Car_TA.Demolish", "Function TAGame.Car_TA.Demolish2",
	                        "Function TAGame.Car_TA.EventDemolished", "Function TAGame.Car_TA.DemolishDestroyTimer" }) {
		gameWrapper->HookEventWithCaller<CarWrapper>(fn, demolished);
	}
	// AddCar(Car_TA Car) runs for every car put into the match: at the start and on
	// every respawn.  A new car may reuse a demolished one's address, so it is cleared.
	gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TA.AddCar",
		[this](ActorWrapper caller, void* params, std::string eventName) {
			if (params == nullptr) {
				return;
			}
			noteCarSpawned(*static_cast<std::uintptr_t*>(params));
		});
	// The match ending clears the rest (hooked once, in registerBoostPadHooks).
}

// Puts the frozen situation back, as it is shown while frozen: with the variance
// settings applied, the ball kept out of the net in a match, and cars the state has
// nothing for held where they are.
void CheckpointPlugin::holdFrozen() {
	GameState shown = applyVariance(latest);
	if (noGoalsFrozen && inMatch()) {
		shown = keepBallOutOfGoal(shown); // scrubbing must never put the ball in the net
	}
	shown.apply(gameWrapper, showBoost, &parkedCars);
}

// In a match, nothing can be frozen or loaded during goal replays and countdowns.
bool CheckpointPlugin::matchBlocked() {
	if (!inMatch()) {
		return false;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	return sw.IsNull() || sw.GetBall().IsNull() || !sw.GetbRoundActive();
}

static std::string teamKey(int own, int opponents) {
	return std::to_string(own) + "v" + std::to_string(opponents);
}

static std::string teamKey(const GameState& s) {
	int own = 1, opponents = 0;
	for (auto& o : s.others) {
		(o.ally ? own : opponents)++;
	}
	return teamKey(own, opponents);
}

// <game>/Binaries/Win64/RocketLeague.exe -> <game>; empty if it cannot be told.
static std::filesystem::path gameFolder() {
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return {};
	}
	return std::filesystem::path(exe).parent_path().parent_path().parent_path();
}

static std::string lower(std::string s) {
	for (char& c : s) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

// Stock arenas are in the game's CookedPCConsole folder; workshop maps are loaded
// from somewhere else.
static bool isStockMap(const std::string& map) {
	std::filesystem::path cooked = gameFolder() / "TAGame" / "CookedPCConsole";
	std::error_code ec;
	if (!std::filesystem::is_directory(cooked, ec)) {
		return true;
	}
	return std::filesystem::exists(cooked / (map + ".upk"), ec);
}

// GetCurrentMap() leaves out everything up to the first '-' of a map's name
// ("Goal-O-Meter" is "o-meter").  The whole name is taken from the map's file in
// Steam's workshop folder, if exactly one map there fits.
static std::string workshopMapName(const std::string& map) {
	// <steamapps>/common/rocketleague -> <steamapps>/workshop/content/252950/<id>/<map>.udk
	std::filesystem::path workshop = gameFolder().parent_path().parent_path() / "workshop" / "content" / "252950";
	std::string found;
	std::error_code ec;
	for (auto& item : std::filesystem::directory_iterator(workshop, ec)) {
		std::error_code ec2;
		for (auto& f : std::filesystem::directory_iterator(item.path(), ec2)) {
			if (lower(f.path().extension().string()) != ".udk") {
				continue;
			}
			std::string name = lower(f.path().stem().string());
			if (name == map || name.substr(name.find('-') + 1) == map) {
				if (!found.empty() && found != name) {
					return map; // two maps fit
				}
				found = name;
			}
		}
	}
	return found.empty() ? map : found;
}

// The store the current mode uses: "" for freeplay on the stock arenas (and custom
// training), "map_<name>" on a workshop map, "<N>v<M>" (own team first) in an offline match.
std::string CheckpointPlugin::currentStoreKey(bool match) {
	if (match) {
		int own = 0, opponents = 0;
		if (!matchTeamSizes(gameWrapper, own, opponents)) {
			// Not on a team yet; decide once the player is.
			return matchStoreActive ? storeKey : teamKey(1, 0);
		}
		return teamKey(own, opponents);
	}
	if (!gameWrapper->IsInFreeplay()) {
		return "";
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull()) {
		return "";
	}
	std::string map = gameWrapper->GetCurrentMap();
	if (map == mapName || map.empty()) { // empty for a moment while a map loads
		return mapKey;
	}
	// A new map was loaded.
	mapName = map;
	mapKey = "";
	map = lower(map);
	if (isStockMap(map)) {
		return mapKey;
	}
	map = workshopMapName(map);
	for (char& c : map) {
		if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' && c != '_') {
			c = '_';
		}
	}
	mapKey = "map_" + map;
	return mapKey;
}

// The file of the current store: the configured name, plus the store's key.
std::filesystem::path CheckpointPlugin::storeFile() {
	std::filesystem::path name = cvarManager->getCvar(matchStoreActive ? "cpt_match_filename" : "cpt_filename").getStringValue();
	if (!storeKey.empty()) {
		std::filesystem::path ext = name.extension();
		name.replace_extension();
		name += "_" + storeKey;
		name += ext;
	}
	return gameWrapper->GetDataFolder() / name;
}

// Switches to the checkpoints of the current mode (freeplay, workshop map, match size)
// when it changes.
void CheckpointPlugin::syncStore() {
	bool match = inMatch();
	std::string key = currentStoreKey(match);
	if (match == matchStoreActive && key == storeKey) {
		return;
	}
	storePositions[storeKey] = curCheckpoint;
	matchStoreActive = match;
	storeKey = key;
	loadCheckpointFile();
	curCheckpoint = storePositions[storeKey];
	if (curCheckpoint >= checkpoints.size()) {
		curCheckpoint = 0;
	}
	// History and quick checkpoints do not carry over between the modes.
	history.clear();
	forgetBoostPads();
	forgetDemolishedCars();
	parkedCars.clear();
	hasQuickCheckpoint = false;
	playingFromCheckpoint = false;
	pendingMatchLoad = false;
	rewindState = RewindState();
	setFrozen(false, false);
	dodgeExpiration = 0.0;
	std::string mode = match ? "offline match " + key : key.empty() ? "freeplay" : "workshop map " + key.substr(4);
	cvarManager->log("Freeplay Checkpoint: " + mode + ", using its checkpoints (" + std::to_string(checkpoints.size()) + ")");
}

// True when the stored match checkpoint s is loaded turned around: it was saved on
// the other team.
bool CheckpointPlugin::shownFlipped(const GameState& s) {
	if (!matchTeamAware || !inMatch() || s.team < 0) {
		return false;
	}
	int team = playerTeam(gameWrapper);
	return team >= 0 && s.team != team;
}

// A stored checkpoint as it should be loaded now.
GameState CheckpointPlugin::forLoad(GameState s) {
	if (!inMatch()) {
		return s;
	}
	if (s.time >= 0 && s.time < MATCH_MIN_LOAD_TIME) {
		s.time = MATCH_MIN_LOAD_TIME;
	}
	// Saved while on the other team: turn the whole situation around, so it is
	// on the same side of the field relative to the player as when it was saved.
	if (shownFlipped(s)) {
		s = s.flipSides();
	}
	// Whatever it was, it is now the situation as seen from this team; a checkpoint
	// saved from it (after rewinding or changing it) carries the right team.
	int team = playerTeam(gameWrapper);
	if (team >= 0) {
		s.team = team;
	}
	return s;
}

bool CheckpointPlugin::enabled() {
	syncStore();
	if (gameWrapper->IsInReplay()) {
		// Replays may be paused when checkpoints are taken.
		return true;
	}
	if (gameWrapper->IsPaused()) {
		// Don't allow checkpoint operations while paused.
		return false;
	}
	if (!disableTraining && gameWrapper->IsInCustomTraining()) {
		return true;
	}
	if (inMatch()) {
		return true;
	}
	if (!gameWrapper->IsInFreeplay()) {
		return false;
	}
	return !disableWorkshop || PlaylistIds(gameWrapper->GetGameEventAsServer().GetPlaylist().GetPlaylistId()) != PlaylistIds::Workshop;
}

bool CheckpointPlugin::enabledLoads() {
	syncStore();
	if (gameWrapper->IsPaused()) {
		// Don't allow checkpoint operations while paused.
		return false;
	}
	if (inMatch()) {
		return true;
	}
	if (!gameWrapper->IsInFreeplay()) {
		return false;
	}
	return !disableWorkshop || PlaylistIds(gameWrapper->GetGameEventAsServer().GetPlaylist().GetPlaylistId()) != PlaylistIds::Workshop;
}

void CheckpointPlugin::copyShot(std::vector<std::string> command) {
	std::string output = "";
	if (gameWrapper->IsInReplay()) {
		std::unique_ptr<GameState> gs = getReplayGameState();
		if (gs == nullptr) {
			return;
		}
		output = gs->toString();
	} else if (rewindMode) {
		cvarManager->log("Copying current position");
		output = latest.toString();
	} else if (hasQuickCheckpoint) {
		cvarManager->log("Copying quick checkpoint");
		output = quickCheckpoint.toString();
	} else if (checkpoints.size() > 0) {
		cvarManager->log("Copying checkpoint " + std::to_string(curCheckpoint + 1));
		output = checkpoints.at(curCheckpoint).toString();
	} else {
		cvarManager->log("No checkpoint to copy!");
		return;
	}
	output = "cpv1" + output + ".";
	OpenClipboard(nullptr);
	EmptyClipboard();
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, output.size() + 1);
	if (hg == nullptr) {
		cvarManager->log("Error copying to clipboard!");
		CloseClipboard();
		return;
	}
	LPVOID lptstrCopy = GlobalLock(hg);
	if (lptstrCopy == nullptr) {
		cvarManager->log("Error copying to clipboard!");
		CloseClipboard();
		return;
	}
	memcpy(lptstrCopy, output.c_str(), output.size() + 1);
	GlobalUnlock(hg);
	SetClipboardData(CF_TEXT, hg);
	CloseClipboard();
	GlobalFree(hg);
	cvarManager->log("Data copied to clipboard!");
	log("Written to clipboard: " + output);
}

void CheckpointPlugin::pasteShot(std::vector<std::string> command) {
	// Clipboard checkpoints only hold one car and the ball.
	if (!enabledLoads() || inMatch()) {
		return;
	}
	OpenClipboard(nullptr);
	HANDLE hData = GetClipboardData(CF_TEXT);
	if (hData == nullptr) {
		cvarManager->log("Error reading clipboard!");
		return;
	}
	char* pszText = static_cast<char*>(GlobalLock(hData));
	if (pszText == nullptr) {
		cvarManager->log("Error reading clipboard!");
		return;
	}
	std::string input(pszText);
	GlobalUnlock(hData);
	CloseClipboard();
	log("Read from clipboard: " + input);
	if (input.substr(0, 4) != "cpv1" || input[input.size() - 1] != '.') {
		cvarManager->log("Malformed checkpoint in clipboard: " + input);
		return;
	}
	quickCheckpoint = GameState(input.substr(4, input.size() - 5));
	loadGameState(quickCheckpoint);
	hasQuickCheckpoint = true;
	rewindState.justLoadedQuickCheckpoint = true;
}

// Like the dribble plugin's "ballontop", but the ball goes in front of the car, moving
// along with it.
void CheckpointPlugin::ballInFront(std::vector<std::string> command) {
	if ((!gameWrapper->IsInFreeplay() && !inMatch()) || gameWrapper->IsPaused() || matchBlocked() || rewindMode || freezeBall) {
		return;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull()) {
		return;
	}
	BallWrapper ball = sw.GetBall();
	CarWrapper car = playerCar(gameWrapper);
	if (ball.IsNull() || !carAlive(car)) {
		return;
	}
	float distance = cvarManager->getCvar("cpt_ball_front_distance").getFloatValue();
	// 76 above the car's centre is where the ball rests on the surface the car stands on.
	Vector offset = RotateVectorWithQuat(Vector(distance, 0, 76), RotatorToQuat(car.GetRotation()));
	ball.SetLocation(car.GetLocation() + offset);
	ball.SetVelocity(car.GetVelocity());
	ball.SetAngularVelocity(Vector(0, 0, 0), false);
}

void CheckpointPlugin::freezeBallUnfreezeCar(std::vector<std::string> command) {
	if (!enabledLoads() || history.size() == 0) {
		return;
	}
	if (rewindMode) {
		setFrozen(false, true);
		return;
	}
	if (freezeBall) {
		setFrozen(false, false);
		latest.car = history.back().car;
		latest.others = history.back().others;
		latest.pads = history.back().pads;
		latest.time = history.back().time;
		latest.apply(gameWrapper, showBoost);
		quickCheckpoint = latest;
		hasQuickCheckpoint = true;
		return;
	}
	if (ignoreFreezeBall) {
		return;
	}
	latest = history.back();
	latest.apply(gameWrapper, false);
	setFrozen(false, true);
}

void CheckpointPlugin::mirrorState(std::vector<std::string> command) {
	if (!enabledLoads() || !rewindMode) {
		return;
	}
	rewindState.atCheckpoint = false;
	hasQuickCheckpoint = true;
	quickCheckpoint = latest.mirror();
	loadLatestCheckpoint();
}

// cpt_checkpoint_team [flip|blue|orange]: tags the loaded match checkpoint with the
// team it was saved on.  Checkpoints saved before the team was recorded have no team
// and are loaded as they are; with no argument the checkpoint is tagged the way it
// is shown right now (that is the correct side), "flip" tags it the other way around.
void CheckpointPlugin::checkpointTeam(std::vector<std::string> command) {
	if (!enabledLoads() || !inMatch() || !rewindMode || !rewindState.atCheckpoint || curCheckpoint >= checkpoints.size()) {
		cvarManager->log("cpt_checkpoint_team: load a match checkpoint first (frozen, at a checkpoint)");
		return;
	}
	int mine = playerTeam(gameWrapper);
	if (mine < 0) {
		cvarManager->log("cpt_checkpoint_team: you are not on a team");
		return;
	}
	GameState& cp = checkpoints[curCheckpoint];
	bool flipped = shownFlipped(cp);
	std::string arg = command.size() > 1 ? lower(command[1]) : "";
	int team;
	if (arg == "blue" || arg == "0") {
		team = 0;
	} else if (arg == "orange" || arg == "1") {
		team = 1;
	} else if (arg == "flip" || arg == "other") {
		team = flipped ? mine : 1 - mine;   // shown the other way around from now on
	} else {
		team = flipped ? 1 - mine : mine;   // as shown
	}
	cp.team = team;
	saveCheckpointFile();
	cvarManager->log("checkpoint " + std::to_string(curCheckpoint + 1) + " is a " + (team == 0 ? "blue" : "orange") + " team shot now");
	loadCurCheckpoint();
}

// cpt_checkpoint_team_all blue|orange [force]: tags every checkpoint of the current
// match mode that has no team yet (saved before teams were recorded) with that team;
// with "force" also the ones that already have one.  For a whole file of old shots
// that were all saved on the same team.
void CheckpointPlugin::checkpointTeamAll(std::vector<std::string> command) {
	if (!inMatch()) {
		cvarManager->log("cpt_checkpoint_team_all: only in an offline match (its checkpoints are tagged)");
		return;
	}
	std::string arg = command.size() > 1 ? lower(command[1]) : "";
	int team;
	if (arg == "blue" || arg == "0") {
		team = 0;
	} else if (arg == "orange" || arg == "1") {
		team = 1;
	} else {
		cvarManager->log("usage: cpt_checkpoint_team_all blue|orange [force]");
		return;
	}
	bool force = command.size() > 2 && lower(command[2]) == "force";
	int tagged = 0;
	for (auto& cp : checkpoints) {
		if (cp.team < 0 || force) {
			cp.team = team;
			tagged++;
		}
	}
	if (tagged > 0) {
		saveCheckpointFile();
	}
	cvarManager->log(std::to_string(tagged) + " of " + std::to_string(checkpoints.size()) + " checkpoints tagged as " + (team == 0 ? "blue" : "orange") + " team shots" + (tagged ? "" : " (nothing to do)"));
	if (tagged > 0 && rewindMode && rewindState.atCheckpoint && curCheckpoint < checkpoints.size()) {
		loadCurCheckpoint();
	}
}

void CheckpointPlugin::deleteAllCheckpoints(std::vector<std::string> command) {
	if (!cvarManager->getCvar("cpt_allow_delete_all").getBoolValue()) {
		return;
	}
	cvarManager->getCvar("cpt_allow_delete_all").setValue("0");
	checkpoints.resize(0);
	locks.resize(0);
	curCheckpoint = 0;
	saveCheckpointFile();
}

void CheckpointPlugin::randCheckpoint(std::vector<std::string> command) {
	if (!enabledLoads()) {
		return;
	}
	loadRandomCheckpoint();
}

void CheckpointPlugin::prevCheckpoint(std::vector<std::string> command) {
	if (!enabledLoads() || checkpoints.size() == 0) {
		return;
	}
	if (ignorePrev && !rewindMode) {
		return;
	}
	if (!rewindState.justDeletedCheckpoint) {
		// If you just deleted a checkpoint, prev should go one prior to
		// the deleted one (the current one).
		if (curCheckpoint == 0) {
			curCheckpoint = checkpoints.size() - 1;
		} else {
			curCheckpoint--;
		}
	}
	loadCurCheckpoint();
}

void CheckpointPlugin::nextCheckpoint(std::vector<std::string> command) {
	if (!enabledLoads() || checkpoints.size() == 0) {
		return;
	}
	if (ignoreNext && !rewindMode) {
		return;
	}
	curCheckpoint++;
	if (curCheckpoint == checkpoints.size()) {
		curCheckpoint = 0;
	}
	loadCurCheckpoint();
}

void CheckpointPlugin::lockCheckpoint(std::vector<std::string> command) {
	if (gameWrapper->IsPaused() || !rewindMode || !rewindState.atCheckpoint) {
		return;
	}
	rewindState.deleting = false;
	if (locks.size() <= curCheckpoint) {
		locks.resize(curCheckpoint + 1);
	}
	if (locks[curCheckpoint]) {
		log("at cpt; unlocking: " + std::to_string(curCheckpoint + 1));
	} else {
		log("at cpt; locking: " + std::to_string(curCheckpoint + 1));
	}
	locks[curCheckpoint] = !locks[curCheckpoint];
	saveCheckpointFile();
}

void CheckpointPlugin::doCheckpoint(std::vector<std::string> command) {
	{
		if (!enabled()) {
			return;
		}
		if (gameWrapper->IsInReplay()) {
			std::unique_ptr<GameState> gs = getReplayGameState();
			if (gs == nullptr) {
				return;
			}
			cvarManager->log("adding checkpoint " + std::to_string(checkpoints.size() + 1));
			checkpoints.push_back(*gs);
			saveCheckpointFile();
			return;
		}
		if (gameWrapper->IsInCustomTraining()) {
			// Only support loading the quick checkpoint for now.
			if (hasQuickCheckpoint) {
				loadLatestCheckpoint();
			}
			return;
		}
		if (!rewindMode) {
			if (randomizeLoads) {
				loadRandomCheckpoint();
				return;
			}
			loadLatestCheckpoint();
			return;
		}
		hasQuickCheckpoint = false;
		if (rewindState.atCheckpoint) { // Delete the current checkpoint we are at.
			if (locks.size() > curCheckpoint && locks[curCheckpoint]) {
				log("at cpt but locked: " + std::to_string(curCheckpoint + 1));
				return;
			}
			if (!rewindState.deleting) {
				rewindState.deleting = true;
				return;
			}
			rewindState.deleting = false;
			log("at cpt; removing: " + std::to_string(curCheckpoint + 1));
			checkpoints.erase(checkpoints.begin() + curCheckpoint);
			if (locks.size() > curCheckpoint) {
				locks.erase(locks.begin() + curCheckpoint);
			}
			curCheckpoint = std::min(curCheckpoint, checkpoints.size() - 1);
			rewindState.atCheckpoint = false;
			rewindState.justDeletedCheckpoint = true;
			saveCheckpointFile();
			return;
		}
		// Add a new checkpoint here.
		log("adding checkpoint " + std::to_string(checkpoints.size() + 1));
		curCheckpoint = checkpoints.size();
		checkpoints.push_back(latest);
		saveCheckpointFile();
		// loadGameState(latest);
		rewindState.atCheckpoint = true;
	}
}

void CheckpointPlugin::registerVarianceCVars() {
	cvarManager->registerCvar("cpt_variance_car_dir", "0", "If set, randomly vary car's direction when resuming", true, true, 0, true, 30, true);
	cvarManager->registerCvar("cpt_variance_car_spd", "0", "If set, randomly vary car's speed when resuming", true, true, 0, true, 50, true);
	cvarManager->registerCvar("cpt_variance_car_rot", "0", "If set, randomly vary car's rotation when resuming", true, true, 0, true, 10, true);
	cvarManager->registerCvar("cpt_variance_ball_dir", "0", "If set, randomly vary ball's direction when resuming", true, true, 0, true, 30, true);
	cvarManager->registerCvar("cpt_variance_ball_spd", "0", "If set, randomly vary ball's speed when resuming", true, true, 0, true, 50, true);
	cvarManager->registerCvar("cpt_variance_ball_rot", "0", "If set, randomly vary ball's rotation when resuming", true, true, 0, true, 10, true);
	cvarManager->registerCvar("cpt_variance_tot", "0", "Total variance applied to all factors (range)", true, true, 0, true, 50, true);
}

void CheckpointPlugin::onUnload() {
	suppressGoals(false); // never leave freeplay goal scoring switched off behind us
}

void CheckpointPlugin::loadLatestCheckpoint() {
	if (hasQuickCheckpoint) {
		log("loading quick checkpoint");
		loadGameState(forLoad(quickCheckpoint));
		hasQuickCheckpoint = true;
		rewindState.justLoadedQuickCheckpoint = true;
		return;
	}
	if (checkpoints.size() > 0) {
		log("loading checkpoint " + std::to_string(curCheckpoint+1));
		loadCurCheckpoint();
		return;
	}
	log("no checkpoint to load");
	rewindState.virtualTimeOffset = 0;
	rewindState.holdingFor = 0;
}

void CheckpointPlugin::loadRandomCheckpoint() {
	if (checkpoints.size() == 0) {
		return;
	}
	hasQuickCheckpoint = false;
	curCheckpoint = rand() % checkpoints.size();
	loadLatestCheckpoint();
}

void CheckpointPlugin::loadCurCheckpoint() {
	auto checkpoint = checkpoints.at(curCheckpoint);
	if (mirrorLoads && rand() % 2 == 0) {
		checkpoint = checkpoint.mirror();
	}
	hasQuickCheckpoint = false;
	loadGameState(forLoad(checkpoint));
	rewindState.atCheckpoint = true;
}

void CheckpointPlugin::loadGameState(const GameState& state) {
	if (matchBlocked()) {
		// Goal replay or kickoff countdown: load as soon as play resumes.
		pendingMatchLoad = true;
		return;
	}
	pendingMatchLoad = false;
	settling = false; // a new load supersedes any settle in progress
	parkedCars.clear(); // a new freeze: cars without a state are held where they are now
	latest = state;
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (!inMatch() && cvarManager->getCvar("sv_soccar_enablegoal").getBoolValue()) {
		sw.PlayerResetTraining(); // In case a goal was just scored, there may be no ball.
	}
	if (noGoalsFrozen) {
		suppressGoals(true); // before the state lands: the loaded ball may be in the net
	}
	if (noGoalsFrozen && inMatch()) {
		keepBallOutOfGoal(latest).apply(gameWrapper, false, &parkedCars);
	} else {
		latest.apply(gameWrapper, false, &parkedCars);
	}
	applyBoostPads(latest);
	padSettleTicks = 3;
	rewindState.virtualTimeOffset = 0;
	rewindState.holdingFor = 0;
	setFrozen(true, true);
	rewindState.atCheckpoint = false;
	hasQuickCheckpoint = false;
	rewindState.justDeletedCheckpoint = false;
	rewindState.justLoadedQuickCheckpoint = false;
	rewindState.deleting = false;
	rewindState.buttonsDown = 0x7f;
	playingFromCheckpoint = true; // not playing yet but must resume eventually.
}

void CheckpointPlugin::OnPreAsync(std::string funcName)
{
	syncStore();
	bool match = inMatch();
	if (!gameWrapper->IsInFreeplay() && !gameWrapper->IsInCustomTraining() && !match) {
		return;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull() || sw.GetBall().IsNull()) {
		return;
	}
	if (match) {
		bool playing = sw.GetbRoundActive();
		if (pendingMatchLoad && playing && !gameWrapper->IsPaused()) {
			pendingMatchLoad = false;
			loadLatestCheckpoint();
		}
		if (!playing && !rewindMode) {
			return; // Goal replay / kickoff countdown: nothing worth recording.
		}
	}
	if (!carAlive(playerCar(gameWrapper))) {
		// The player's car is gone (demolished, in a match).  Frozen: keep holding the
		// rest of the situation until the car is back; it gets its state then.
		if (match && rewindMode) {
			settling = false;
			holdFrozen();
		}
		return;
	}

	if (settling) {
		// Just resumed in a match: hold the resumed situation for settleMs so the bots
		// get a few decision cycles on it before anything moves (see settleMs).
		if (match && sw.GetSecondsElapsed() < settleUntil) {
			latest.apply(gameWrapper, showBoost, &parkedCars);
			return;
		}
		settling = false;
		parkedCars.clear();
	}
	if (rewindMode) {
		if (padSettleTicks > 0 && --padSettleTicks == 0) {
			// A car that stood on a pad when the state was loaded may have taken it
			// again before it was moved away; put the pads right once more.
			applyBoostPads(latest);
		}
		if (rewind(sw)) {
			holdFrozen();
		}
	} else {
		record(sw);
	}
}

// Helper function to get the input value based on axis name
float getInputValue(const std::string& axisName, const ControllerInput& ci) {
	if (axisName == "throttle") {
		return ci.Throttle;
	}
	else if (axisName == "pitch") {
		return ci.Pitch;
	}
	else if (axisName == "roll") {
		return ci.Roll;
	}
	else if (axisName == "yaw") {
		return ci.Yaw;
	}
	else if (axisName == "steer") {
		return ci.Steer;
	}
	return 0.0f; // Default return if axisName doesn't match
}

// Returns true if we need to apply the state again.
bool CheckpointPlugin::rewind(ServerWrapper sw) {
	ControllerInput ci = (inMatch() ? playerCar(gameWrapper) : sw.GetCars().Get(0)).GetInput();

	float currentTime = sw.GetSecondsElapsed();
	float elapsed = std::min(currentTime - lastRewindTime, 0.03f);
	if (elapsed < 0) {
		lastRewindTime = currentTime;
		return false;  // Ignored whatever inputs may have happened to exit mode; do not apply state.
	}
	if (elapsed < 0.01f) {
		return false;  // Ignored whatever inputs may have happened to exit mode; do not apply state.
	}
	lastRewindTime = currentTime;

	// Get user settings
	std::string rewindAxis = cvarManager->getCvar("rewind_axis").getStringValue();
	std::string matchingAxis = cvarManager->getCvar("matching_axis").getStringValue();

	// Determine the active and matching inputs based on user selection
	float rewindInput = getInputValue(rewindAxis, ci);
	float matchingInput = getInputValue(matchingAxis, ci);

	// Retrieve rewind unpause settings from CVars
	int buttonsDown = 0;
	bool isAtCheckpoint = rewindState.atCheckpoint || rewindState.justLoadedQuickCheckpoint;

	auto checkAndSet = [&](const std::string& axis, float axisValue, const char* unpauseSetting, const char* thresholdSetting, int buttonBit) {
		if (axis == rewindAxis) {
  			return;
		}
		if (cvarManager->getCvar(unpauseSetting).getBoolValue()) {
			float effectiveThreshold = cvarManager->getCvar(thresholdSetting).getFloatValue();
			if (fabs(axisValue) > effectiveThreshold) {
				buttonsDown |= buttonBit;
			}
		}
	};

	// Check analog inputs
	checkAndSet("throttle", ci.Throttle, "enable_throttle_unpause", "throttle_threshold", 0x01);
	checkAndSet("roll", ci.Roll, "enable_roll_unpause", "roll_threshold", 0x02);
	checkAndSet("steer", ci.Steer, "enable_steer_unpause", "steer_threshold", 0x40);
	checkAndSet("pitch", ci.Pitch, "enable_pitch_unpause", "pitch_threshold", 0x80);
	checkAndSet("yaw", ci.Yaw, "enable_yaw_unpause", "yaw_threshold", 0x100);

	// Directly check digital inputs without considering the "matching axis"
	if (ci.Handbrake)
		buttonsDown |= 0x04;
	if (ci.Jump)
		buttonsDown |= 0x08;
	if (ci.ActivateBoost)
		buttonsDown |= 0x10;
	if (ci.HoldingBoost)
		buttonsDown |= 0x20;

	// See if we should exit rewind mode due to input.
	if (buttonsDown != 0) {
		if ((buttonsDown > rewindState.buttonsDown && currentTime - lastRecordTime > 0.1f) ||
			currentTime - lastRecordTime > 0.5f) {
			log("resuming...");
			setFrozen(false, false);
			lastRecordTime = currentTime;
			dodgeExpiration = (latest.car.hasDodge && latest.car.lastJumped != -1) ? (currentTime + MAX_DODGE_TIME - latest.car.lastJumped) : 0;
			if (!rewindState.atCheckpoint) {
				log("quick checkpoint taken");
				hasQuickCheckpoint = true;
				quickCheckpoint = latest;
				if (deleteFutureHistory) {
					size_t current = std::clamp<size_t>(
						history.size() - 1 + size_t(ceil(rewindState.virtualTimeOffset / snapshotInterval)),
						0, history.size() - 1);
					history.erase(history.begin() + current, history.end());
				}
			}
			return false; // Leaving rewind; do not apply state.
		}
		rewindState.buttonsDown = buttonsDown;
		return true; // Staying in rewind; apply state.
	}
	rewindState.buttonsDown = buttonsDown;

	
	// Ignore slight input; keep current game state.
	const float rewindThreshold = cvarManager->getCvar("rewind_threshold").getFloatValue();
	if (abs(rewindInput) < rewindThreshold) {
		return true; // Ignoring input; apply state.
	} else if (isAtCheckpoint) {
		// At a checkpoint the rewind input resumes play instead of rewinding, but only
		// if that input is set to unpause; otherwise it is ignored.
		CVarWrapper unpause = cvarManager->getCvar("enable_" + rewindAxis + "_unpause");
		CVarWrapper threshold = cvarManager->getCvar(rewindAxis + "_threshold");
		if (unpause.IsNull() || threshold.IsNull() || !unpause.getBoolValue() ||
			fabs(rewindInput) <= threshold.getFloatValue()) {
			return true; // Ignoring input; apply state.
		}
		log("checkpoint active, unpausing game instead of rewinding");
		setFrozen(false, false);
		return false;  // Unpausing the game due to checkpoint.
	}

	rewindState.deleting = false;

	// Determine how much to rewind / advance time.
	if (rewindInput < -.95 && rewindState.holdingFor <= 0) {
		rewindState.holdingFor -= elapsed;
	} else if (rewindInput > .95 && rewindState.holdingFor >= 0) {
		rewindState.holdingFor += elapsed;
	} else {
		rewindState.holdingFor = 0;
	}

	// Normalize rewindInput based on the threshold
	float excessInput = std::abs(rewindInput) - rewindThreshold;
	float normalRewindInput;
	if (excessInput > 0) {
		// Normalize only if rewindInput exceeds the threshold
		float normalizedInput = excessInput / (1.0 - rewindThreshold);
		normalRewindInput = (rewindInput < 0 ? -1 : 1) * normalizedInput;
	} else {
		normalRewindInput = 0;  // No effective input if below threshold
	}

	float factor = std::clamp(abs(rewindState.holdingFor) * 2, 1.0f, 10.0f);

	// How much (in seconds) to move "current" (positive or negative)
	float deltaElapsed = factor * elapsed * normalRewindInput; // full left = 2-5 seconds/second

	// if you are trying to use the matching axis more than the rewind axis
	// and you've haven't hit the threshold to unpause, don't rewind
	if ((matchingAxis != "None" && 2 * fabs(matchingInput) > fabs(rewindInput))) {
		deltaElapsed = 0;
	}

	if (deltaElapsed != 0) {
		rewindState.atCheckpoint = false;
	}

	rewindState.virtualTimeOffset = std::clamp(
		rewindState.virtualTimeOffset + deltaElapsed, -snapshotInterval * history.size(), .0f);

	float historyOffset = rewindState.virtualTimeOffset / snapshotInterval;
	size_t current = std::clamp<size_t>(
		history.size() + size_t(floor(historyOffset)), 0, history.size() - 1);
	if (current < (history.size() - 1) /* && NEED TO INTERPOLATE */) {
		float advancePct = 1 - (historyOffset - floor(historyOffset));
		latest = GameState(history.at(current), history.at(current+1), advancePct);
		return true; // Apply new state.
	}
	latest = history.at(current);
	return true; // Apply new state.
}

void CheckpointPlugin::record(ServerWrapper sw)
{
	float currentTime = sw.GetSecondsElapsed();
	float elapsed = currentTime - lastRecordTime;
	if (elapsed < 0) {
		elapsed = snapshotInterval;
	}
	if (elapsed < snapshotInterval) {
		return;
	}
	// This cannot be event-based since goals may be disabled.
	if (playingFromCheckpoint && (resetOnGoal || resetOnBallGround)) {
		auto ball = sw.GetBall();
		if (ball.IsNull()) {
			return;
		}
		auto ballLoc = ball.GetLocation();
		auto ballRad = ball.GetRadius();
		if ((resetOnGoal && sw.IsInGoal(ballLoc)) ||
			(resetOnBallGround && ballLoc.Z < ballRad + 5)) {
			if (nextInsteadOfReset && !hasQuickCheckpoint && checkpoints.size() > 0) {
				if (randomizeLoads) {
					loadRandomCheckpoint();
					return;
				}
				curCheckpoint++;
				if (curCheckpoint == checkpoints.size()) {
					curCheckpoint = 0;
				}
				loadCurCheckpoint();
				return;
			}
			loadLatestCheckpoint();
			return;
		}
	}

	lastRecordTime = currentTime;
	if (dodgeExpiration != 0) {
		// If the timer expires or if the player double-jumps or gets a reset,
		// clear the jump timer so we don't take the player's dodge.
		auto c = playerCar(gameWrapper);
		if (carAlive(c) && (currentTime > dodgeExpiration ||
				  c.GetbDoubleJumped() ||
				  c.GetNumWheelContacts() == 4)) {
			c.SetbJumped(true);
			c.SetbDoubleJumped(true);
			dodgeExpiration = 0;
		}
	}

	if (freezeBall) {
		latest.ball.apply(sw.GetBall());
	}

	// TODO: use a ring buffer?
	if (history.size() == maxHistory) {
		history.erase(history.begin());
	}
	if (dodgeExpiration == 0) {
		history.emplace_back(gameWrapper);
	} else {
		history.emplace_back(gameWrapper, MAX_DODGE_TIME - currentTime + dodgeExpiration);
	}
	if (botCamera && inMatch()) {
		pollBotCamera(std::min(elapsed, 0.1f));
	}
	if (restorePads && inMatch()) {
		pollBoostPads(std::min(elapsed, 0.1f));
		history.back().pads = captureBoostPads();
	}
}

void show(CanvasWrapper canvas, Vector2 *loc, std::string s) {
	static const float scale = 1.5f;
	canvas.SetPosition(*loc);
	canvas.DrawString(s, scale, scale);
	loc->Y += 20;
}

void CheckpointPlugin::Render(CanvasWrapper canvas) {
	if (!enabled()) {
		return;
	}
	if (debug) {
		canvas.SetColor('\xff', '\xff', '\xff', '\xdc');
		auto screenSize = canvas.GetSize();
		Vector2 loc = { (int)(screenSize.X * 0.08), (int)(screenSize.Y * 0.08) };
		show(canvas, &loc, "rewindMode: " + std::to_string(rewindMode));
		show(canvas, &loc, "atCheckpoint: " + std::to_string(rewindState.atCheckpoint));
		show(canvas, &loc, "justDeletedCheckpoint: " + std::to_string(rewindState.justDeletedCheckpoint));
		show(canvas, &loc, "justLoadedQuickCheckpoint: " + std::to_string(rewindState.justLoadedQuickCheckpoint));
		show(canvas, &loc, "hasQuickCheckpoint: " + std::to_string(hasQuickCheckpoint));
		show(canvas, &loc, "virtualTimeOffset: " + std::to_string(rewindState.virtualTimeOffset));
		show(canvas, &loc, "buttonsDown: " + std::to_string(rewindState.buttonsDown));
		size_t current = std::clamp<size_t>(
			history.size() + size_t(ceil(rewindState.virtualTimeOffset / snapshotInterval)),
			0, history.size() - 1);
		show(canvas, &loc, "current: " + std::to_string(current));
	}
	if (!rewindMode) {
		return;
	}
	if (rewindState.deleting) {
		auto screenSize = canvas.GetSize();
		Vector2 loc = { (int)(screenSize.X * 0.80), (int)(screenSize.Y * 0.08) };
		loc.X = int(screenSize.X * .70);
		canvas.SetPosition(loc);
		canvas.SetColor('\xff', '\xff', '\xff', '\xdc');
		canvas.DrawString("Press again to delete...", 5, 5);
		return;
	}
	if (rewindState.justDeletedCheckpoint) {
		auto screenSize = canvas.GetSize();
		Vector2 loc = { (int)(screenSize.X * 0.80), (int)(screenSize.Y * 0.08) };
		loc.X = int(screenSize.X * .70);
		canvas.SetPosition(loc);
		canvas.SetColor('\xff', '\xff', '\xff', '\xdc');
		canvas.DrawString("Checkpoint deleted!", 5, 5);
		return;
	}
	if (rewindState.atCheckpoint) {
		auto screenSize = canvas.GetSize();
		std::string l = "";
		if (locks.size() > curCheckpoint && locks[curCheckpoint]) {
			l = " (L)";
		}
		if (matchStoreActive && curCheckpoint < checkpoints.size()) {
			const GameState& cp = checkpoints[curCheckpoint];
			if (cp.team < 0) {
				l += " (team ?)";   // saved before teams were recorded: cpt_checkpoint_team
			} else if (shownFlipped(cp)) {
				l += " (flipped)";  // saved on the other team, turned around for this one
			}
		}
		Vector2 loc = { (int)(screenSize.X * 0.80), (int)(screenSize.Y * 0.08) };
		canvas.SetPosition(loc + Vector2{ 5,5 });
		canvas.SetColor(0, 0, 0, 100);
		canvas.DrawString(std::to_string(curCheckpoint + 1) +
			" | " +
			std::to_string(checkpoints.size()) + l, 6, 6);
		canvas.SetPosition(loc);
		canvas.SetColor('\xff', '\xff', '\xff', '\xdc');
		canvas.DrawString(std::to_string(curCheckpoint + 1) +
			" | " +
			std::to_string(checkpoints.size()) + l, 6, 6);
	}
}

// Prevent loading an unknown version's save file.
constexpr uint32_t SAVE_FILE_VERSION = 1;
// Match checkpoints hold a variable number of cars; they use their own file and version.
// Version 3 added the boost pads, version 4 the team the checkpoint was saved on.
constexpr uint32_t MATCH_SAVE_FILE_VERSION = 4;
constexpr uint32_t MATCH_SAVE_FILE_VERSION_OLDEST = 2;

void CheckpointPlugin::loadCheckpointFile() {
	checkpoints.clear();
	locks.clear();
	std::filesystem::path file = storeFile();
	// Match checkpoints saved before every team size had its own file are all in the
	// file without a team size; a team size without a file yet takes its own from there.
	std::error_code ec;
	bool shared = matchStoreActive && !std::filesystem::exists(file, ec);
	if (shared) {
		file = gameWrapper->GetDataFolder() / cvarManager->getCvar("cpt_match_filename").getStringValue();
	}
	std::ifstream in(file, std::ios::binary);
	uint32_t version = 0;
	readPOD(in, version);
	if (matchStoreActive ? (version < MATCH_SAVE_FILE_VERSION_OLDEST || version > MATCH_SAVE_FILE_VERSION) : version != SAVE_FILE_VERSION) {
		in.close();
		log("could not load save file with version " + std::to_string(version));
		return;
	}
	int32_t numSaves = 0;
	readPOD(in, numSaves);
	for (int32_t i = 0; i < numSaves; i++) {
		if (matchStoreActive) {
			checkpoints.push_back(GameState::readMatch(in, version));
		} else {
			checkpoints.emplace_back(in);
		}
	}
	int32_t numLocks = 0; // older save files did not have this data; initialize to 0.
	readPOD(in, numLocks);
	for (int32_t i = 0; i < numLocks; i++) {
		bool locked;
		readPOD(in, locked);
		locks.push_back(locked);
	}
	in.close();
	if (shared) {
		std::vector<GameState> own;
		std::vector<bool> ownLocks;
		for (size_t i = 0; i < checkpoints.size(); i++) {
			if (teamKey(checkpoints[i]) == storeKey) {
				own.push_back(checkpoints[i]);
				ownLocks.push_back(i < locks.size() && locks[i]);
			}
		}
		checkpoints = own;
		locks = ownLocks;
	}
}

void CheckpointPlugin::saveCheckpointFile() {
	std::ofstream out(storeFile(), std::ios::binary | std::ios::out | std::ios::trunc);
	auto ver = matchStoreActive ? MATCH_SAVE_FILE_VERSION : SAVE_FILE_VERSION;
	writePOD(out, ver);
	auto size = int32_t(checkpoints.size());
	writePOD(out, size);
	for (auto& fav : checkpoints) {
		if (matchStoreActive) {
			fav.writeMatch(out);
		} else {
			fav.write(out);
		}
	}
	size = int32_t(locks.size());
	writePOD(out, size);
	for (bool l : locks) {
		writePOD(out, l);
	}
	out.close();
}
