/*
 * Copyright (c) 2021
 * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pch.h"
#include "CheckpointPlugin.h"

#include "bakkesmod/wrappers/GameObject/VehiclePickupWrapper.h"

// Respawn delay given to a pad that a restore puts down, so the game leaves it down
// until pollBoostPads() brings it back at the restored time.
constexpr float PAD_HOLD_DELAY = 100000.0f;
// Saved pad locations are matched to the pads of the current match within this distance.
constexpr float PAD_MATCH_DIST = 100.0f;


static BoostPad* findPad(std::vector<BoostPad>& pads, std::uintptr_t addr) {
	for (auto& p : pads) {
		if (p.addr == addr) {
			return &p;
		}
	}
	return nullptr;
}

void CheckpointPlugin::registerBoostPadHooks() {
	// Fires for every pad at each kickoff and whenever one comes back.
	gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.VehiclePickup_TA.OnSpawn",
		[this](ActorWrapper caller, void* params, std::string eventName) {
			noteBoostPad(caller);
			if (BoostPad* p = findPad(pads, caller.memory_address)) {
				p->down = false;
				p->managed = false;
			}
		});
	gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.VehiclePickup_TA.OnPickUp",
		[this](ActorWrapper caller, void* params, std::string eventName) {
			noteBoostPad(caller);
			if (BoostPad* p = findPad(pads, caller.memory_address)) {
				p->down = true;
				p->managed = false;
				p->remaining = p->delay;
			}
		});
	// The pads go away with the match.
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed",
		[this](std::string eventName) {
			forgetBoostPads();
		});
}

void CheckpointPlugin::forgetBoostPads() {
	pads.clear();
	padsGameEvent = 0;
}

void CheckpointPlugin::noteBoostPad(ActorWrapper pad) {
	if (pad.IsNull() || !inMatch()) {
		return;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull()) {
		return;
	}
	if (sw.memory_address != padsGameEvent) {
		pads.clear();
		padsGameEvent = sw.memory_address;
	}
	for (auto& p : pads) {
		if (p.addr == pad.memory_address) {
			return;
		}
	}
	BoostPad p;
	p.addr = pad.memory_address;
	p.location = pad.GetLocation();
	p.delay = VehiclePickupWrapper(p.addr).GetRespawnDelay();
	if (p.delay < 0.5f || p.delay > 60.0f) {
		return; // not a pad in its normal state
	}
	pads.push_back(p);
}

// Runs the respawn clocks while playing, and brings back the pads a restore has put
// down (the game brings back the others).  elapsed is the play time since the last call.
void CheckpointPlugin::pollBoostPads(float elapsed) {
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull() || sw.memory_address != padsGameEvent) {
		forgetBoostPads();
		return;
	}
	for (auto& p : pads) {
		if (!p.down) {
			continue;
		}
		p.remaining -= elapsed;
		if (p.managed && p.remaining <= 0) {
			VehiclePickupWrapper(p.addr).Respawn2(); // OnSpawn marks it as back
		}
	}
}

std::vector<PadState> CheckpointPlugin::captureBoostPads() {
	std::vector<PadState> out;
	for (auto& p : pads) {
		if (p.down) {
			PadState s;
			s.location = p.location;
			s.remaining = std::max(p.remaining, 0.0f);
			out.push_back(s);
		}
	}
	return out;
}

// Puts every pad into the state saved in s.  A pad that should be down is taken by
// the player's car with the game's own respawn switched off; pollBoostPads() brings
// it back when its saved time is up, so the time does not run while frozen.
void CheckpointPlugin::applyBoostPads(const GameState& s) {
	if (!restorePads || !inMatch()) {
		return;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	CarWrapper car = playerCar(gameWrapper);
	if (sw.IsNull() || sw.memory_address != padsGameEvent || car.IsNull() || car.GetBoostComponent().IsNull()) {
		return;
	}
	float boost = car.GetBoostComponent().GetCurrentBoostAmount();
	for (size_t i = 0; i < pads.size(); i++) {
		const PadState* want = nullptr;
		for (auto& d : s.pads) {
			if ((d.location - pads[i].location).magnitude() < PAD_MATCH_DIST) {
				want = &d;
				break;
			}
		}
		VehiclePickupWrapper w(pads[i].addr);
		if (want == nullptr) {
			if (pads[i].down) {
				w.Respawn2();
			}
			continue;
		}
		if (!(pads[i].down && pads[i].managed)) {
			if (pads[i].down) {
				w.Respawn2(); // drops the game's own respawn timer
			}
			float delay = pads[i].delay;
			car.GetBoostComponent().SetCurrentBoostAmount(0);
			w.SetRespawnDelay(PAD_HOLD_DELAY);
			w.Pickup2(car);
			w.SetRespawnDelay(delay);
		}
		pads[i].down = true;
		pads[i].managed = true;
		pads[i].remaining = want->remaining;
	}
	car.GetBoostComponent().SetCurrentBoostAmount(boost);
}
