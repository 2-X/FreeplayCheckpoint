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

#include <map>

class ActorState {
public:
	Vector location;
	Vector velocity;
	Rotator rotation;
	Vector angVelocity;

	ActorState();
	ActorState(ActorWrapper a);
	ActorState(ActorState lh, ActorState rh, float percent);
	ActorState(std::istream& in);

	void write(std::ostream& out) const;
	void apply(ActorWrapper a) const;
	ActorState mirror() const;
	// The same situation seen from the other end of the field (turned half a turn
	// around the center): what a shot saved on one team is on the other team.
	ActorState flipSides() const;
};

class CarState {
public:
	ActorState actorState;
	float boostAmount;
	bool hasDodge;
	float lastJumped; // cannot apply; used to reset dodge in record().
	long boosting;

	CarState();
	CarState(CarWrapper c);
	CarState(CarWrapper c, float lastJumpedTime);
	CarState(CarState lh, CarState rh, float percent);
	CarState(std::istream& in);

	void write(std::ostream& out) const;
	void apply(CarWrapper c, bool showBoost) const;
	CarState mirror() const;
	CarState flipSides() const;
};

// Another car in an offline match (bot or other player), relative to the local player.
class OtherCarState {
public:
	bool ally = false;    // on the local player's team
	bool present = false; // false while demolished; nothing to apply
	CarState state;
};

// A boost pad that is picked up, and how long until it is back.
class PadState {
public:
	Vector location;
	float remaining = 0;
};

// A car that can be read and moved: it exists and is not demolished.  A demolished
// car's actor lingers (hidden, its physics gone) until the game destroys it a moment
// later, and its PRI keeps pointing at it meanwhile; reading its components or moving
// it crashes the game.  Demolitions are learned from the game's events
// (CheckpointPlugin::registerDemolitionHooks).
bool carAlive(CarWrapper c);
// Returns true if the car was not known to be demolished yet.
bool noteCarDemolished(std::uintptr_t car);
void noteCarSpawned(std::uintptr_t car);
void forgetDemolishedCars();

// Cars held where they are while frozen because the frozen state has nothing for
// them (they were demolished when it was recorded), by actor address.
using ParkedCars = std::map<std::uintptr_t, ActorState>;

// True in offline exhibition matches (including RLBot matches); never in online games.
bool isOfflineMatch(std::shared_ptr<GameWrapper> gw);
// The team sizes of the current offline match, the local player's team first.
// False while the local player has not joined a team yet.
bool matchTeamSizes(std::shared_ptr<GameWrapper> gw, int& own, int& opponents);
// The car driven by the local player: freeplay's car, or the human's car in a match.
CarWrapper playerCar(std::shared_ptr<GameWrapper> gw);
// The local player's team: 0 blue, 1 orange, -1 while not on a team.
int playerTeam(std::shared_ptr<GameWrapper> gw);

class GameState {
public:
	ActorState ball;
	CarState car;
	float time; // -1 if not in a timed mode
	std::vector<OtherCarState> others; // offline matches only: every car but the local player's
	std::vector<PadState> pads;        // offline matches only: the boost pads that are picked up
	// Offline matches only: the team (0 blue, 1 orange) whose side of the field this
	// state is seen from - the local player's team when it was captured.  -1 when
	// unknown (freeplay, and match checkpoints saved before this was recorded).
	int team = -1;

	GameState();
	GameState(std::shared_ptr<GameWrapper> gw);
	GameState(std::shared_ptr<GameWrapper> gw, float lastJumpedTime);
	GameState(CarWrapper cw, BallWrapper bw);
	GameState(const GameState& lh, const GameState& rh, float percent);
	GameState(std::istream& in);
	GameState(std::string str);

	void write(std::ostream& out) const;
	// Match checkpoints: also carry the clock, the team, the other cars and the pads
	// (version: the match save file version, which says which of these are there).
	static GameState readMatch(std::istream& in, uint32_t version);
	void writeMatch(std::ostream& out) const;
	// Puts the game into this state.  In an offline match a car that is alive now but
	// was demolished when the state was recorded is held where it is if `parked` is
	// given (while frozen), and left alone otherwise (resuming play).
	void apply(std::shared_ptr<GameWrapper> gw, bool showBoost, ParkedCars* parked = nullptr) const;
	const std::string toString() const;
	GameState mirror() const;
	// The whole situation turned around to the other team's side (see ActorState).
	GameState flipSides() const;
};
