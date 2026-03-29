// Luanti Dataset Generator
// Procedural walking agent for dataset generation
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "player.h"
#include <random>

class LocalPlayer;
class ClientEnvironment;

namespace tracking {

/**
 * DatasetGenerator - Procedural walking agent for automated dataset capture
 *
 * Injects synthetic PlayerControl to make the player walk around the world,
 * turn, look at things, and avoid getting stuck or falling.
 *
 * State machine:
 *   WALKING -> TURNING -> LOOKING -> WALKING (loop)
 *      |                                 ^
 *      +---> RECOVERING -----------------+
 *
 * Hazard detection:
 *   - Falling: Y velocity negative for >0.5s -> recover
 *   - Stuck: position unchanged for >2s -> turn and walk
 *   - Liquid: in_liquid detected -> turn away
 */
class DatasetGenerator {
public:
	DatasetGenerator();

	bool isActive() const { return m_active; }

	/**
	 * Activate dataset generation
	 * @param walk_speed Movement speed multiplier (0-1)
	 */
	void activate(float walk_speed = 1.0f);

	/**
	 * Update movement control for this frame
	 * @param control PlayerControl to modify
	 * @param player Local player for position/state queries
	 * @param env Client environment for terrain queries
	 * @param dtime Frame delta time in seconds
	 */
	void update(PlayerControl &control, LocalPlayer *player,
	            ClientEnvironment *env, float dtime);

private:
	enum class State {
		WALKING,
		TURNING,
		LOOKING,
		RECOVERING  // replaces UNSTICKING -- handles falls, stuck, liquid
	};

	void enterState(State state);
	void updateWalking(PlayerControl &control, LocalPlayer *player,
	                   ClientEnvironment *env, float dtime);
	void updateTurning(PlayerControl &control, float dtime);
	void updateLooking(PlayerControl &control, float dtime);
	void updateRecovering(PlayerControl &control, LocalPlayer *player, float dtime);

	// Hazard detection
	bool checkFalling(LocalPlayer *player, float dtime);
	bool checkStuck(LocalPlayer *player, float dtime);
	bool checkDropAhead(LocalPlayer *player, ClientEnvironment *env);

	// Helpers
	float randomFloat(float min, float max);
	int randomInt(int min, int max);

	bool m_active = false;
	State m_state = State::WALKING;

	// Timing
	float m_state_timer = 0.0f;
	float m_state_duration = 0.0f;
	float m_total_time = 0.0f;

	// Movement
	float m_walk_speed = 1.0f;
	float m_current_yaw = 0.0f;
	float m_current_pitch = 0.0f;
	float m_target_yaw = 0.0f;
	float m_target_pitch = 0.0f;

	// Stuck detection
	v3f m_last_position = v3f(0, 0, 0);
	float m_stuck_timer = 0.0f;

	// Fall detection
	float m_last_y = 0.0f;
	float m_fall_timer = 0.0f;      // time spent falling
	v3f m_last_ground_pos = v3f(0, 0, 0); // last position on solid ground

	// Recovery
	int m_recovery_attempts = 0;
	bool m_recovery_fly = false;     // using free_move to fly up

	// State tracking
	int m_walk_cycles = 0;

	// RNG
	std::mt19937 m_rng;
};

} // namespace tracking
