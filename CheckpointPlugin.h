/*
 * Copyright (c) 2021
 * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "utils/parser.h"
#include "state.h"

#include "version.h"

#include <filesystem>
#include <map>

constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);
constexpr float MAX_DODGE_TIME = 1.2f;

template<typename T>
void writePOD(std::ostream& out, const T& t) {
	out.write(reinterpret_cast<const char*>(&t), sizeof(T));
}

template<typename T>
void readPOD(std::istream& in, T& t) {
	T temp;
	in.read(reinterpret_cast<char*>(&temp), sizeof(T));
	if (in.eof()) {
		return;
	}
	t = temp;
}

// Rotator uses ints instead of floats.  Floats are better.
struct Rot {
	float Pitch, Yaw, Roll;
};

// TODO: make this a full-on "RewindMode" class with functions for operations
struct RewindState {
	bool atCheckpoint = false;
	float virtualTimeOffset = 0; // Delta from end of buffer to "now"
	bool justDeletedCheckpoint = false;
	bool justLoadedQuickCheckpoint = false;
	float holdingFor = 0;
	bool deleting = false;
	int buttonsDown = 0x7f;
};

// A boost pad of the current match.  The SDK cannot list them, so they are learned
// from the game's pickup / respawn events.
struct BoostPad {
	std::uintptr_t addr = 0;
	Vector location;
	float delay = 0;      // the pad's own respawn delay
	bool down = false;
	bool managed = false; // put down by a restore: the game will not respawn it, we do
	float remaining = 0;  // seconds of play until it is back
};

class CheckpointPlugin : public BakkesMod::Plugin::BakkesModPlugin {
	//Boilerplate
	virtual void onLoad();
	void copyShot(std::vector<std::string> command);
	void mirrorState(std::vector<std::string> command);
	void checkpointTeam(std::vector<std::string> command);
	void deleteAllCheckpoints(std::vector<std::string> command);
	void randCheckpoint(std::vector<std::string> command);
	void pasteShot(std::vector<std::string> command);
	void freezeBallUnfreezeCar(std::vector<std::string> command);
	void ballInFront(std::vector<std::string> command);
	virtual void onUnload();
	void doCheckpoint(std::vector<std::string> command);
	void lockCheckpoint(std::vector<std::string> command);
	void prevCheckpoint(std::vector<std::string> command);
	void nextCheckpoint(std::vector<std::string> command);

private:
	RewindState rewindState;
	std::vector<GameState> history;
	GameState latest;
	std::vector<GameState> checkpoints;
	std::vector<bool> locks;
	size_t curCheckpoint = 0;
	bool rewindMode = false;
	bool freezeBall = false;
	float dodgeExpiration = 0;
	bool hasQuickCheckpoint = false;
	GameState quickCheckpoint;
	float lastRecordTime = 0;
	float lastRewindTime = 0;
	std::vector<GameState> gameHistory;
	int carNum = 0;
	bool playingFromCheckpoint = false;

	// Freeplay, every workshop map and every team size of offline matches (exhibition /
	// RLBot) keep their own checkpoints, each in its own file.  checkpoints, locks and
	// curCheckpoint above are always the store of the current mode.
	bool matchStoreActive = false; // the store holds match checkpoints (all cars, clock, pads)
	std::string storeKey;          // "" freeplay, "map_<name>" workshop map, "<N>v<M>" match
	std::map<std::string, size_t> storePositions; // curCheckpoint of the stores left behind
	std::string mapName;           // the freeplay map last looked at ...
	std::string mapKey;            // ... and its storeKey
	// A checkpoint load was requested during a goal replay or kickoff countdown.
	bool pendingMatchLoad = false;
	std::vector<BoostPad> pads;
	std::uintptr_t padsGameEvent = 0;
	bool restorePads = true;
	int padSettleTicks = 0;

	// Settings:
	bool deleteFutureHistory = false;
	bool ignorePNNotFrozen = false;
	bool ignorePrev = false;
	bool ignoreNext = false;
	bool ignoreFreezeBall = false;
	bool disableTraining = false;
	bool disableWorkshop = false;
	bool debug = false;
	bool resetOnGoal = false;
	bool resetOnBallGround = false;
	bool nextInsteadOfReset = false;
	bool mirrorLoads = false;
	bool randomizeLoads = false;
	bool showBoost = false;
	bool matchEnabled = true;
	// Never score while frozen / rewinding (scrubbing can carry the ball through the net).
	bool noGoalsFrozen = true;
	bool goalsSuppressed = false;   // we turned freeplay goal scoring off and must turn it back on
	bool savedEnableGoal = true;    // its value before we did
	// Offline matches: after resuming, hold the situation for a moment so bots (Nexto
	// decides every 8 ticks from the current state and its own last actions) take the
	// new situation in before physics runs. Otherwise a load that is very different
	// from where play was has the bot acting on the old picture for its first moves.
	int settleMs = 300;
	bool settling = false;
	float settleUntil = 0;
	// Match checkpoints saved on the other team are loaded turned around, so the shot
	// stays on the same side of the field relative to the player.
	bool matchTeamAware = true;

	void addBind(std::string key, std::string cmd);
	void removeBind(std::string key, std::string cmd);
	void OnPreAsync(std::string funcName);
	void registerVarianceCVars();
	void registerBindingCVars();
	void captureBindKey(std::vector<std::string> params);
	void removeBindKeys(std::vector<std::string> params);
	void applyBindKeys(std::vector<std::string> params);
	void resetDefaultBindKeys(std::vector<std::string> params);
	GameState applyVariance(GameState& s);
	bool rewind(ServerWrapper sw);
	void loadCheckpointFile();
	void saveCheckpointFile();
	void Render(CanvasWrapper canvas);
	void record(ServerWrapper sw);
	void loadLatestCheckpoint();
	void loadCurCheckpoint();
	void loadRandomCheckpoint();
	void loadGameState(const GameState&);
	void log(std::string s);
	void boolvar(std::string name, std::string desc, bool* var);
	std::unique_ptr<GameState> getReplayGameState();
	void setFrozen(bool car, bool ball);
	void suppressGoals(bool on);
	GameState keepBallOutOfGoal(const GameState& s);
	void writeSettingsFile();
	bool enabled();
	bool enabledLoads();
	bool inMatch();
	bool matchBlocked();
	void syncStore();
	std::string currentStoreKey(bool match);
	std::filesystem::path storeFile();
	GameState forLoad(GameState s);
	bool shownFlipped(const GameState& s);
	void registerBoostPadHooks();
	void noteBoostPad(ActorWrapper pad);
	void forgetBoostPads();
	void pollBoostPads(float elapsed);
	std::vector<PadState> captureBoostPads();
	void applyBoostPads(const GameState& s);
};
