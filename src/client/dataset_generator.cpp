// Luanti Dataset Generator
// Procedural walking agent for dataset generation
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "dataset_generator.h"
#include "localplayer.h"
#include "clientenvironment.h"
#include "client.h"
#include "map.h"
#include "nodedef.h"
#include "constants.h"
#include "log.h"
#include <cmath>

namespace tracking {

DatasetGenerator::DatasetGenerator()
	: m_rng(std::random_device{}())
{
}

void DatasetGenerator::activate(float walk_speed)
{
	m_walk_speed = std::clamp(walk_speed, 0.1f, 1.0f);
	m_active = true;
	m_current_yaw = randomFloat(0.0f, 360.0f);
	enterState(State::WALKING);
	m_state_duration = randomFloat(1.5f, 3.0f); // short first walk
	infostream << "[DatasetGen] Activated (walk_speed=" << m_walk_speed
	           << ", initial_yaw=" << m_current_yaw << ")" << std::endl;
}

float DatasetGenerator::randomFloat(float min, float max)
{
	std::uniform_real_distribution<float> dist(min, max);
	return dist(m_rng);
}

int DatasetGenerator::randomInt(int min, int max)
{
	std::uniform_int_distribution<int> dist(min, max);
	return dist(m_rng);
}

void DatasetGenerator::enterState(State state)
{
	m_state = state;
	m_state_timer = 0.0f;

	switch (state) {
	case State::WALKING:
		m_state_duration = randomFloat(2.0f, 5.0f);
		m_recovery_attempts = 0;
		break;

	case State::TURNING:
		m_state_duration = randomFloat(0.5f, 1.5f);
		m_target_yaw = m_current_yaw + randomFloat(40.0f, 160.0f) * (randomInt(0, 1) ? 1.0f : -1.0f);
		break;

	case State::LOOKING:
		m_state_duration = randomFloat(1.5f, 3.0f);
		m_target_pitch = randomFloat(-15.0f, 45.0f);
		// Random yaw drift while looking
		m_target_yaw = m_current_yaw + randomFloat(-40.0f, 40.0f);
		break;

	case State::RECOVERING:
		m_state_duration = 4.0f;
		// Turn 90-180 degrees away from trouble
		m_target_yaw = m_current_yaw + randomFloat(90.0f, 180.0f) * (randomInt(0, 1) ? 1.0f : -1.0f);
		m_recovery_attempts++;
		m_recovery_fly = false;
		infostream << "[DatasetGen] Recovering (attempt " << m_recovery_attempts << ")" << std::endl;
		break;
	}
}

bool DatasetGenerator::checkFalling(LocalPlayer *player, float dtime)
{
	float y = player->getPosition().Y;
	float dy = y - m_last_y;
	m_last_y = y;

	if (player->touching_ground) {
		m_fall_timer = 0.0f;
		m_last_ground_pos = player->getPosition();
		return false;
	}

	// Falling = Y decreasing and not on ground
	if (dy < -0.5f) {
		m_fall_timer += dtime;
	} else {
		m_fall_timer = std::max(0.0f, m_fall_timer - dtime);
	}

	return m_fall_timer > 0.5f;
}

bool DatasetGenerator::checkStuck(LocalPlayer *player, float dtime)
{
	v3f pos = player->getPosition();

	// Only check horizontal distance (Y changes from jumping/falling don't count)
	float dx = pos.X - m_last_position.X;
	float dz = pos.Z - m_last_position.Z;
	float horiz_dist = std::sqrt(dx * dx + dz * dz);

	if (horiz_dist > 2.0f) {
		m_last_position = pos;
		m_stuck_timer = 0.0f;
		return false;
	}

	m_stuck_timer += dtime;
	return m_stuck_timer > 2.0f;
}

bool DatasetGenerator::checkDropAhead(LocalPlayer *player, ClientEnvironment *env)
{
	if (!env)
		return false;

	v3f pos = player->getPosition();
	float yaw_rad = m_current_yaw * M_PI / 180.0f;

	// Check 3 nodes ahead in the walking direction
	float check_dist = 3.0f * BS;
	float ahead_x = pos.X - std::sin(yaw_rad) * check_dist;
	float ahead_z = pos.Z + std::cos(yaw_rad) * check_dist;

	// Convert to node coordinates
	v3s16 ahead_node = floatToInt(v3f(ahead_x, pos.Y, ahead_z), BS);

	// Check if there's solid ground within 3 nodes below the ahead position
	Map &map = env->getMap();
	const NodeDefManager *ndef = env->getGameDef()->getNodeDefManager();

	for (int dy = 0; dy >= -3; dy--) {
		v3s16 check_pos = ahead_node + v3s16(0, dy, 0);
		bool valid = false;
		MapNode node = map.getNode(check_pos, &valid);
		if (valid && node.getContent() != CONTENT_AIR &&
			node.getContent() != CONTENT_IGNORE) {
			const ContentFeatures &f = ndef->get(node);
			if (f.walkable)
				return false; // ground found within 3 nodes drop -- safe
		}
	}

	return true; // no ground ahead -- cliff detected
}

void DatasetGenerator::update(PlayerControl &control, LocalPlayer *player,
                               ClientEnvironment *env, float dtime)
{
	if (!m_active || !player)
		return;

	m_state_timer += dtime;
	m_total_time += dtime;

	// --- Hazard detection (overrides current state) ---

	// Falling detection: if falling for >0.5s, recover
	if (m_state != State::RECOVERING && checkFalling(player, dtime)) {
		infostream << "[DatasetGen] Fall detected, recovering" << std::endl;
		enterState(State::RECOVERING);
		m_recovery_fly = true; // use fly to get back up
	}

	// Stuck detection: if horizontal position hasn't changed for >2s
	if (m_state != State::RECOVERING && m_state != State::TURNING &&
		checkStuck(player, dtime)) {
		infostream << "[DatasetGen] Stuck detected, recovering" << std::endl;
		enterState(State::RECOVERING);
	}

	// Liquid detection
	if (m_state != State::RECOVERING && player->in_liquid) {
		infostream << "[DatasetGen] In liquid, recovering" << std::endl;
		enterState(State::RECOVERING);
	}

	// --- State transitions on timeout ---
	if (m_state_timer >= m_state_duration) {
		switch (m_state) {
		case State::WALKING:
			m_walk_cycles++;
			// Alternate between turning and looking
			if (m_walk_cycles % 3 == 0) {
				enterState(State::LOOKING);
			} else {
				enterState(State::TURNING);
			}
			break;
		case State::TURNING:
			enterState(State::WALKING);
			break;
		case State::LOOKING:
			enterState(State::TURNING);
			break;
		case State::RECOVERING:
			enterState(State::WALKING);
			break;
		}
	}

	// --- Execute current state ---
	switch (m_state) {
	case State::WALKING:
		updateWalking(control, player, env, dtime);
		break;
	case State::TURNING:
		updateTurning(control, dtime);
		break;
	case State::LOOKING:
		updateLooking(control, dtime);
		break;
	case State::RECOVERING:
		updateRecovering(control, player, dtime);
		break;
	}

	// Always apply current yaw and pitch
	control.yaw = m_current_yaw;
	control.pitch = m_current_pitch;
}

void DatasetGenerator::updateWalking(PlayerControl &control, LocalPlayer *player,
                                      ClientEnvironment *env, float dtime)
{
	// Check for cliff ahead -- if detected, turn instead of walking off
	if (checkDropAhead(player, env)) {
		infostream << "[DatasetGen] Cliff ahead, turning" << std::endl;
		enterState(State::TURNING);
		return;
	}

	// Walk forward
	control.movement_speed = m_walk_speed;
	control.movement_direction = 0.0f;
	control.direction_keys = 1; // forward

	// Jump frequently to climb over 1-block obstacles
	if (player->touching_ground && randomInt(0, 30) == 0) {
		control.jump = true;
	}

	// Pitch variation: oscillate between looking slightly up and more down
	float pitch_base = 15.0f; // look slightly downward on average
	float pitch_variation = 12.0f * std::sin(m_state_timer * 0.8f);
	m_current_pitch = std::clamp(pitch_base + pitch_variation, -10.0f, 35.0f);
}

void DatasetGenerator::updateTurning(PlayerControl &control, float dtime)
{
	// Smoothly interpolate yaw toward target
	m_current_yaw += (m_target_yaw - m_current_yaw) * std::min(dtime * 3.0f, 1.0f);

	// Walk slowly while turning
	control.movement_speed = m_walk_speed * 0.3f;
	control.movement_direction = 0.0f;
	control.direction_keys = 1; // forward
}

void DatasetGenerator::updateLooking(PlayerControl &control, float dtime)
{
	// Smoothly interpolate pitch and yaw toward targets
	m_current_pitch += (m_target_pitch - m_current_pitch) * std::min(dtime * 2.0f, 1.0f);
	m_current_yaw += (m_target_yaw - m_current_yaw) * std::min(dtime * 1.5f, 1.0f);

	// Walk very slowly
	control.movement_speed = m_walk_speed * 0.15f;
	control.movement_direction = 0.0f;
	control.direction_keys = 1; // forward
}

void DatasetGenerator::updateRecovering(PlayerControl &control, LocalPlayer *player, float dtime)
{
	float phase = m_state_timer / m_state_duration;

	if (m_recovery_fly && phase < 0.5f) {
		// Phase 1: Fly upward toward last known ground position
		// Enable jump to gain height (server will apply upward velocity)
		control.jump = true;
		control.movement_speed = 0.0f;

		// Turn toward a new direction
		m_current_yaw += (m_target_yaw - m_current_yaw) * std::min(dtime * 4.0f, 1.0f);

		// If we're back near ground level, stop flying
		if (player->touching_ground && m_state_timer > 0.5f) {
			m_recovery_fly = false;
		}
	} else if (phase < 0.3f) {
		// Phase 1 (non-fly): Jump and turn
		control.jump = true;
		m_current_yaw += (m_target_yaw - m_current_yaw) * std::min(dtime * 5.0f, 1.0f);
		control.movement_speed = 0.0f;
	} else {
		// Phase 2: Walk in new direction
		control.movement_speed = m_walk_speed;
		control.movement_direction = 0.0f;
		control.direction_keys = 1; // forward
		// Keep jumping to get over obstacles
		if (player->touching_ground) {
			control.jump = true;
		}
	}

	// After many recovery attempts, try walking backward
	if (m_recovery_attempts >= 4) {
		control.direction_keys = 2; // backward
		control.movement_direction = M_PI;
		m_current_yaw += dtime * 45.0f; // spin to find a way out
	}

	// Look forward to see where we're going
	m_current_pitch = 10.0f;
}

} // namespace tracking
