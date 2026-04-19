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
#include "clientobject.h"
#include "voxelalgorithms.h"
#include "util/numeric.h"
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
	actionstream << "[DatasetGen] Activated (walk_speed=" << m_walk_speed
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

	case State::GAZING:
		m_gaze_phase = GazePhase::ACQUIRE;
		m_gaze_phase_timer = 0.0f;
		m_gaze_drift_phase = randomFloat(0.0f, 2.0f * M_PI);
		m_state_duration = 10.0f; // safety timeout
		break;

	case State::RECOVERING:
		m_state_duration = 4.0f;
		// Turn 90-180 degrees away from trouble
		m_target_yaw = m_current_yaw + randomFloat(90.0f, 180.0f) * (randomInt(0, 1) ? 1.0f : -1.0f);
		m_recovery_attempts++;
		m_recovery_fly = false;
		actionstream << "[DatasetGen] Recovering (attempt " << m_recovery_attempts << ")" << std::endl;
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
		actionstream << "[DatasetGen] Fall detected, recovering" << std::endl;
		enterState(State::RECOVERING);
		m_recovery_fly = true; // use fly to get back up
	}

	// Stuck detection: if horizontal position hasn't changed for >2s
	if (m_state != State::RECOVERING && m_state != State::TURNING &&
		checkStuck(player, dtime)) {
		actionstream << "[DatasetGen] Stuck detected, recovering" << std::endl;
		enterState(State::RECOVERING);
	}

	// Liquid detection
	if (m_state != State::RECOVERING && player->in_liquid) {
		actionstream << "[DatasetGen] In liquid, recovering" << std::endl;
		enterState(State::RECOVERING);
	}

	// --- State transitions on timeout ---
	if (m_state_timer >= m_state_duration) {
		switch (m_state) {
		case State::WALKING:
			m_walk_cycles++;
			if (m_walk_cycles % 3 == 0) {
				enterState(State::LOOKING);
			} else if (randomFloat(0.0f, 1.0f) < 0.35f) {
				enterState(State::GAZING);
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
		case State::GAZING:
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
	case State::GAZING:
		updateGazing(control, player, env, dtime);
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
		actionstream << "[DatasetGen] Cliff ahead, turning" << std::endl;
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

// --- Gazing implementation ---

bool DatasetGenerator::isInterestingNode(content_t c, ClientEnvironment *env) const
{
	if (c == CONTENT_AIR || c == CONTENT_IGNORE)
		return false;

	const NodeDefManager *ndef = env->getGameDef()->getNodeDefManager();
	const ContentFeatures &f = ndef->get(c);

	if (f.name.empty())
		return false;

	// Skip basic ground content (stone, dirt, sand, gravel)
	if (f.is_ground_content && f.drawtype == NDT_NORMAL)
		return false;

	// Plants, torches, signs, fire, fences, rails
	if (f.drawtype == NDT_PLANTLIKE || f.drawtype == NDT_PLANTLIKE_ROOTED ||
	    f.drawtype == NDT_TORCHLIKE || f.drawtype == NDT_FIRELIKE ||
	    f.drawtype == NDT_SIGNLIKE || f.drawtype == NDT_FENCELIKE ||
	    f.drawtype == NDT_RAILLIKE)
		return true;

	// Light-emitting nodes
	if (f.light_source > 5)
		return true;

	// Liquids
	if (f.drawtype == NDT_LIQUID || f.drawtype == NDT_FLOWINGLIQUID)
		return true;

	// Non-ground structures (trees, leaves, ores in glass, etc.)
	if (!f.is_ground_content)
		return true;

	return false;
}

bool DatasetGenerator::scanNearbyBlocks(LocalPlayer *player, ClientEnvironment *env,
                                         GazeTarget &best)
{
	Map &map = env->getMap();
	const NodeDefManager *ndef = env->getGameDef()->getNodeDefManager();
	v3f player_pos = player->getPosition();
	v3s16 player_node = floatToInt(player_pos, BS);

	best.interest = 0.0f;
	int scan_radius = 8;
	int num_samples = 80;

	for (int i = 0; i < num_samples; i++) {
		v3s16 offset(
			randomInt(-scan_radius, scan_radius),
			randomInt(-3, 5),
			randomInt(-scan_radius, scan_radius)
		);

		float dist = std::sqrt((float)(offset.X * offset.X +
		                                offset.Y * offset.Y +
		                                offset.Z * offset.Z));
		if (dist < 3.0f || dist > scan_radius)
			continue;

		v3s16 check_pos = player_node + offset;
		bool valid = false;
		MapNode node = map.getNode(check_pos, &valid);
		if (!valid)
			continue;

		content_t c = node.getContent();
		if (!isInterestingNode(c, env))
			continue;

		const ContentFeatures &f = ndef->get(c);

		float interest = 1.0f / (dist + 1.0f);
		if (f.light_source > 0)
			interest += f.light_source * 0.05f;
		if (f.drawtype == NDT_PLANTLIKE || f.drawtype == NDT_FIRELIKE ||
		    f.drawtype == NDT_TORCHLIKE)
			interest += 0.3f;

		if (interest > best.interest) {
			best.position = intToFloat(check_pos, BS) + v3f(BS * 0.5f, BS * 0.5f, BS * 0.5f);
			best.is_entity = false;
			best.entity_id = 0;
			best.interest = interest;
		}
	}

	return best.interest > 0.0f;
}

bool DatasetGenerator::scanNearbyEntities(LocalPlayer *player, ClientEnvironment *env,
                                           GazeTarget &best)
{
	v3f player_pos = player->getPosition();

	std::vector<DistanceSortedActiveObject> objects;
	env->getActiveObjects(player_pos, 20.0f * BS, objects);

	for (const auto &sorted_obj : objects) {
		ClientActiveObject *obj = sorted_obj.obj;
		if (!obj || obj->isLocalPlayer())
			continue;

		v3f obj_pos = obj->getPosition();
		float dist = (obj_pos - player_pos).getLength() / BS;

		if (dist < 2.0f || dist > 20.0f)
			continue;

		// Entities are always interesting, 2x base score vs blocks
		float interest = 2.0f / (dist + 1.0f);

		if (interest > best.interest) {
			best.position = obj_pos;
			best.is_entity = true;
			best.entity_id = obj->getId();
			best.interest = interest;
		}
	}

	return best.is_entity && best.interest > 0.0f;
}

bool DatasetGenerator::hasLineOfSight(const v3f &from_nodes, const v3f &to_nodes,
                                       ClientEnvironment *env)
{
	Map &map = env->getMap();
	const NodeDefManager *ndef = env->getGameDef()->getNodeDefManager();

	v3f line_vec = to_nodes - from_nodes;
	voxalgo::VoxelLineIterator it(from_nodes, line_vec);

	// Skip first node (player position)
	if (it.hasNext())
		it.next();

	while (it.hasNext()) {
		v3s16 node_pos = it.m_current_node_pos;
		bool valid = false;
		MapNode node = map.getNode(node_pos, &valid);
		if (valid) {
			content_t c = node.getContent();
			if (c != CONTENT_AIR && c != CONTENT_IGNORE) {
				const ContentFeatures &f = ndef->get(c);
				// Check if we reached the target
				v3f node_center = intToFloat(node_pos, 1.0f) + v3f(0.5f, 0.5f, 0.5f);
				if ((node_center - to_nodes / BS).getLength() < 1.5f)
					return true;
				// Hit solid opaque node before target
				if (f.drawtype == NDT_NORMAL && !f.light_propagates)
					return false;
			}
		}
		it.next();
	}
	return true;
}

bool DatasetGenerator::findGazeTarget(LocalPlayer *player, ClientEnvironment *env,
                                       GazeTarget &out)
{
	GazeTarget block_target = {};
	GazeTarget entity_target = {};

	bool found_block = scanNearbyBlocks(player, env, block_target);
	bool found_entity = scanNearbyEntities(player, env, entity_target);

	// Prefer entities but take whichever scores higher
	if (found_entity && entity_target.interest >= block_target.interest * 0.7f) {
		out = entity_target;
	} else if (found_block) {
		out = block_target;
	} else {
		return false;
	}

	// Verify line of sight
	v3f player_eye = player->getPosition() + v3f(0, 1.625f * BS, 0);
	v3f from_nodes = player_eye / BS;
	v3f to_nodes = out.position / BS;

	return hasLineOfSight(from_nodes, to_nodes, env);
}

void DatasetGenerator::computeGazeAngles(const v3f &player_pos, const v3f &target_pos,
                                          float &out_yaw, float &out_pitch)
{
	v3f diff = target_pos - player_pos;

	// Luanti yaw: 0 = +Z, X = -sin(yaw), Z = +cos(yaw)
	out_yaw = std::atan2(-diff.X, diff.Z) * 180.0f / M_PI;

	// Pitch: positive = looking down, negative = looking up
	float horiz_dist = std::sqrt(diff.X * diff.X + diff.Z * diff.Z);
	out_pitch = std::atan2(-diff.Y, horiz_dist) * 180.0f / M_PI;
}

void DatasetGenerator::updateEntityTargetPosition(ClientEnvironment *env)
{
	if (!m_gaze_is_entity || !env)
		return;

	std::vector<DistanceSortedActiveObject> objects;
	env->getActiveObjects(m_gaze_target_pos, 30.0f * BS, objects);

	for (const auto &sorted_obj : objects) {
		ClientActiveObject *obj = sorted_obj.obj;
		if (obj && obj->getId() == m_gaze_entity_id) {
			m_gaze_target_pos = obj->getPosition();
			return;
		}
	}

	// Entity gone — stare at last known position
	m_gaze_is_entity = false;
}

void DatasetGenerator::updateGazing(PlayerControl &control, LocalPlayer *player,
                                     ClientEnvironment *env, float dtime)
{
	m_gaze_phase_timer += dtime;

	switch (m_gaze_phase) {
	case GazePhase::ACQUIRE: {
		GazeTarget target;
		if (!findGazeTarget(player, env, target)) {
			enterState(State::TURNING);
			return;
		}

		m_gaze_target_pos = target.position;
		m_gaze_is_entity = target.is_entity;
		m_gaze_entity_id = target.entity_id;
		m_gaze_approach_duration = randomFloat(2.0f, 4.0f);
		m_gaze_stare_duration = randomFloat(1.0f, 3.0f);
		m_state_duration = m_gaze_approach_duration + m_gaze_stare_duration + 0.5f;

		v3f player_eye = player->getPosition() + v3f(0, 1.625f * BS, 0);
		computeGazeAngles(player_eye, m_gaze_target_pos, m_target_yaw, m_target_pitch);

		m_gaze_phase = GazePhase::APPROACH;
		m_gaze_phase_timer = 0.0f;

		actionstream << "[DatasetGen] Gazing at "
		           << (m_gaze_is_entity ? "entity" : "block")
		           << " at (" << m_gaze_target_pos.X / BS << ", "
		           << m_gaze_target_pos.Y / BS << ", "
		           << m_gaze_target_pos.Z / BS << ")" << std::endl;
		break;
	}

	case GazePhase::APPROACH: {
		if (m_gaze_is_entity)
			updateEntityTargetPosition(env);

		// Recompute target angles each frame (tracking)
		v3f player_eye = player->getPosition() + v3f(0, 1.625f * BS, 0);
		computeGazeAngles(player_eye, m_gaze_target_pos, m_target_yaw, m_target_pitch);

		// Smooth interpolation toward target
		float yaw_diff = wrapDegrees_180(m_target_yaw - m_current_yaw);
		m_current_yaw += yaw_diff * std::min(dtime * 3.0f, 1.0f);

		float pitch_diff = m_target_pitch - m_current_pitch;
		m_current_pitch += pitch_diff * std::min(dtime * 2.5f, 1.0f);

		// Walk slowly toward target
		control.movement_speed = m_walk_speed * 0.3f;
		control.movement_direction = 0.0f;
		control.direction_keys = 1; // forward

		if (player->touching_ground && randomInt(0, 40) == 0)
			control.jump = true;

		if (m_gaze_phase_timer >= m_gaze_approach_duration) {
			m_gaze_phase = GazePhase::STARE;
			m_gaze_phase_timer = 0.0f;
		}
		break;
	}

	case GazePhase::STARE: {
		if (m_gaze_is_entity)
			updateEntityTargetPosition(env);

		v3f player_eye = player->getPosition() + v3f(0, 1.625f * BS, 0);
		computeGazeAngles(player_eye, m_gaze_target_pos, m_target_yaw, m_target_pitch);

		// Smooth tracking with natural drift
		m_gaze_drift_phase += dtime * 1.5f;
		float drift_yaw = 3.0f * std::sin(m_gaze_drift_phase);
		float drift_pitch = 2.0f * std::sin(m_gaze_drift_phase * 0.7f + 1.0f);

		float yaw_diff = wrapDegrees_180((m_target_yaw + drift_yaw) - m_current_yaw);
		m_current_yaw += yaw_diff * std::min(dtime * 2.0f, 1.0f);

		float pitch_diff = (m_target_pitch + drift_pitch) - m_current_pitch;
		m_current_pitch += pitch_diff * std::min(dtime * 1.5f, 1.0f);

		// Near-stationary
		control.movement_speed = m_walk_speed * 0.05f;
		control.movement_direction = 0.0f;
		control.direction_keys = 1;

		if (m_gaze_phase_timer >= m_gaze_stare_duration) {
			m_state_timer = m_state_duration; // trigger state transition
		}
		break;
	}
	}
}

} // namespace tracking
