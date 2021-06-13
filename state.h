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
};

class CarState {
public:
	ActorState actorState;
	float boostAmount;
	bool hasDodge;
	float lastJumped; // cannot apply; used to reset dodge in record().

	CarState();
	CarState(CarWrapper c);
	CarState(CarState lh, CarState rh, float percent);
	CarState(std::istream& in);

	void write(std::ostream& out) const;
	void apply(CarWrapper c) const;
};

class GameState {
public:
	ActorState ball;
	CarState car;

	GameState();
	GameState(ServerWrapper sw);
	GameState(const GameState& lh, const GameState& rh, float percent);
	GameState(std::istream& in);
	GameState(std::string str);

	void write(std::ostream& out) const;
	void apply(ServerWrapper sw) const;
	const std::string toString() const;
};
