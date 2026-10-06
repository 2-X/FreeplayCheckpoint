/*
 * Copyright (c) 2021
 * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pch.h"
#include "CheckpointPlugin.h"

// Offline matches: give every bot the camera settings of the local player (the host).
//
// Camera settings live on each player's PRI.  Bots (RLBot's Nexto, the game's own
// bots) get the game's default camera, which is what you see when you spectate one
// or watch a goal replay from its car.  Copying the host's settings onto the bot
// PRIs makes those views look like your own camera.  RLBot has no camera options,
// so this is the only place it can be done.

// How often the host's settings are compared with what the bots have, so a change
// made in the settings menu mid-match reaches the bots without an event hook.
constexpr float BOT_CAMERA_POLL_SECONDS = 1.0f;

static bool sameCamera(const ProfileCameraSettings& a, const ProfileCameraSettings& b) {
	return a.FOV == b.FOV && a.Height == b.Height && a.Pitch == b.Pitch &&
		a.Distance == b.Distance && a.Stiffness == b.Stiffness &&
		a.SwivelSpeed == b.SwivelSpeed && a.TransitionSpeed == b.TransitionSpeed;
}

void CheckpointPlugin::registerBotCameraHooks() {
	auto cv = cvarManager->registerCvar("cpt_match_bot_camera", "1",
		"If set, bots in offline matches get your camera settings (seen when you spectate a bot or watch a goal replay from its car)",
		true, true, 0, true, 1);
	cv.addOnValueChanged([this](std::string old, CVarWrapper now) {
		botCamera = now.getBoolValue();
		botCameraValid = false; // re-apply (or stop applying) from scratch
		if (botCamera) {
			copyCameraToBots(true);
		}
	});
	cvarManager->registerNotifier("cpt_copy_camera_to_bots",
		[this](std::vector<std::string> command) { copyCameraToBots(true); },
		"Gives every bot in the current offline match your camera settings now", PERMISSION_ALL);

	// Every kickoff countdown (first one included): all PRIs exist by then, and a
	// match restarted by RLBot gets fresh PRIs with default cameras again.
	gameWrapper->HookEventPost("Function GameEvent_TA.Countdown.BeginState",
		[this](std::string eventName) { copyCameraToBots(true); });
	// The round going live, in case a PRI was still being set up during the countdown.
	gameWrapper->HookEventPost("Function GameEvent_Soccar_TA.Active.StartRound",
		[this](std::string eventName) { copyCameraToBots(true); });
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed",
		[this](std::string eventName) { botCameraValid = false; });
}

// Called every tick from OnPreAsync during offline matches; does the real work about
// once a second.  Picks up the host changing their camera settings mid-match.
void CheckpointPlugin::pollBotCamera(float elapsed) {
	botCameraPollIn -= elapsed;
	if (botCameraPollIn > 0) {
		return;
	}
	botCameraPollIn = BOT_CAMERA_POLL_SECONDS;
	copyCameraToBots(false);
}

// Copies the local player's camera settings onto every PRI that is not a local
// player (bots; a second split-screen human keeps their own).  With force=false
// nothing is written unless the host's settings differ from what was last applied.
void CheckpointPlugin::copyCameraToBots(bool force) {
	if (!botCamera || !inMatch()) {
		return;
	}
	ServerWrapper sw = gameWrapper->GetGameEventAsServer();
	if (sw.IsNull()) {
		return;
	}
	ArrayWrapper<PriWrapper> pris = sw.GetPRIs();
	ProfileCameraSettings mine{};
	bool found = false;
	for (int i = 0; i < pris.Count(); i++) {
		PriWrapper pri = pris.Get(i);
		if (!pri.IsNull() && pri.IsLocalPlayerPRI()) {
			mine = pri.GetCameraSettings();
			found = true;
			break;
		}
	}
	if (!found) {
		return;
	}
	if (!force && botCameraValid && sameCamera(mine, botCameraApplied)) {
		return;
	}
	int bots = 0;
	for (int i = 0; i < pris.Count(); i++) {
		PriWrapper pri = pris.Get(i);
		if (pri.IsNull() || pri.IsLocalPlayerPRI()) {
			continue;
		}
		pri.SetCameraSettings(mine);
		bots++;
	}
	botCameraApplied = mine;
	botCameraValid = true;
	log(fmt::format("bot camera: {} car(s) set to FOV {} dist {} height {} angle {} stiff {} swivel {} trans {}",
		bots, mine.FOV, mine.Distance, mine.Height, mine.Pitch, mine.Stiffness, mine.SwivelSpeed, mine.TransitionSpeed));
}
