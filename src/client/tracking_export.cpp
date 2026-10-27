// Luanti Tracking Export
// Integration with visual tracking pipeline for RL training
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "tracking_export.h"
#include "localplayer.h"
#include "client.h"
#include "camera.h"
#include "clientenvironment.h"
#include "map.h"
#include "nodedef.h"
#include "constants.h"
#include "util/numeric.h"
#include "settings.h"
#include "log.h"

#include <IVideoDriver.h>
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <cmath>

// Include tracking pipeline headers OUTSIDE namespace
#ifdef ENABLE_TRACKING_EXPORT
#include "minetest_bridge.h"
#include "frame_producer.h"
#include "frame_formats.h"
#include "network_transport.h"
#include "target_control.h"
#include "reward_signal.h"
#include "reward_protocol.h"
#endif

namespace tracking {

struct TrackingExporter::Impl {
#ifdef ENABLE_TRACKING_EXPORT
	// Local mode (shared memory)
	std::unique_ptr<MinetestBridge> bridge;

	// Network mode (FrameProducer - bidirectional peer-to-peer)
	std::unique_ptr<network::FrameProducer> frame_producer;

	// Target control (UDP receiver for target commands - separate from FrameProducer)
	std::unique_ptr<network::UDPSocket> target_control_socket;
	uint16_t target_control_port = 8002;

	// Active targets (target_id -> TargetPacket)
	std::unordered_map<uint64_t, network::TargetPacket> active_targets;

	// Target motion state (for pattern updates)
	struct TargetState {
		uint64_t start_time_us = 0;
		float initial_x = 0.0f;
		float initial_y = 0.0f;
		bool was_acquired = false;  // Track if target was previously centered (for acquisition rewards)

		// WORLD/VIEW anchors, in world node coordinates. Resolved on the first
		// render (VIEW needs the camera); motion moves along axis_u/axis_v.
		bool world_resolved = false;
		v3f world_origin;
		v3f world_current;
		v3f axis_u;
		v3f axis_v;
	};
	std::unordered_map<uint64_t, TargetState> target_states;

	// Mode flag
	bool network_mode = false;  // false = local (shared memory), true = network

	// Reward storage (network mode)
	std::deque<RewardInfo> reward_history;
	float cumulative_reward = 0.0f;
	uint64_t reward_count = 0;

	// Per-frame reward stamp, sent ahead of each frame in network mode.
	// stamp_cumulative_reward is a double so 0.01 steps stay exact over long runs.
	std::vector<network::RewardSourceTotal> pending_sources;
	float pending_frame_reward = 0.0f;
	double stamp_cumulative_reward = 0.0;
	uint32_t last_applied_action_seq = 0;  // GazeCommand::sequence_number of the last action received

	// Tracking reward shape (tracking_reward_* settings, read at network init)
	bool tracking_reward_gaussian = true;
	float tracking_reward_sigma = 20.0f;  // viewport pixels
	float tracking_reward_scale = 1.0f;
#endif
	bool active = false;
	uint32_t current_session_id = 0;
};

TrackingExporter::TrackingExporter()
	: m_impl(new Impl())
{
}

TrackingExporter::~TrackingExporter()
{
	shutdown();
	delete m_impl;
}

bool TrackingExporter::initializeLocal(const std::string& shared_memory_name)
{
#ifdef ENABLE_TRACKING_EXPORT
	infostream << "[Tracking] Initializing tracking export system (LOCAL MODE)..." << std::endl;

	// Create bridge
	m_impl->bridge = MinetestBridgeFactory::Create();
	if (!m_impl->bridge) {
		errorstream << "[Tracking] Failed to create MinetestBridge" << std::endl;
		return false;
	}

	// Initialize shared memory
	if (!m_impl->bridge->Initialize(shared_memory_name)) {
		errorstream << "[Tracking] Failed to initialize shared memory: "
		            << shared_memory_name << std::endl;
		m_impl->bridge.reset();
		return false;
	}

	m_impl->network_mode = false;
	m_impl->active = true;
	infostream << "[Tracking] ✓ Tracking export initialized (LOCAL MODE)" << std::endl;
	infostream << "[Tracking]   Shared memory: " << shared_memory_name << std::endl;
	infostream << "[Tracking]   Ready to export frames and player state" << std::endl;

	return true;
#else
	warningstream << "[Tracking] Tracking export not compiled in (ENABLE_TRACKING_EXPORT=OFF)"
	              << std::endl;
	return false;
#endif
}

bool TrackingExporter::initializeNetwork(const std::string& server_host,
                                          uint16_t tcp_port,
                                          const std::string& client_id)
{
#ifdef ENABLE_TRACKING_EXPORT
	infostream << "[Tracking] Initializing tracking export (NETWORK MODE)" << std::endl;
	infostream << "[Tracking]   Server: " << server_host << ":" << tcp_port << std::endl;
	infostream << "[Tracking]   Client ID: " << client_id << std::endl;

	// Configure FrameProducer
	network::ProducerConfig config;
	config.server_host = server_host;
	config.tcp_port = tcp_port;
	config.client_id = client_id;
	config.enable_compression = true;
	config.compression_type = network::CompressionType::ZSTD;
	config.heartbeat_interval_ms = 1000;
	config.enable_health_monitoring = true;
	config.keepalive_interval_ms = 5000;
	config.udp_frames_port = 0;   // Let OS choose
	config.udp_actions_port = 0;  // Let OS choose

	// Create FrameProducer
	m_impl->frame_producer = std::make_unique<network::FrameProducer>(config);

	// Connect to NetworkServer
	if (!m_impl->frame_producer->Connect()) {
		errorstream << "[Tracking] Failed to connect to NetworkServer at "
		            << server_host << ":" << tcp_port << std::endl;
		m_impl->frame_producer.reset();
		return false;
	}

	// Get session ID
	m_impl->current_session_id = m_impl->frame_producer->GetSessionID();

	infostream << "[Tracking] ✓ Connected to NetworkServer" << std::endl;
	infostream << "[Tracking]   Session ID: " << m_impl->current_session_id << std::endl;
	infostream << "[Tracking]   Compression: ZSTD enabled" << std::endl;

	m_impl->network_mode = true;
	m_impl->active = true;

	m_impl->tracking_reward_gaussian =
		g_settings->get("tracking_reward_shape") != "linear";
	m_impl->tracking_reward_sigma =
		std::max(g_settings->getFloat("tracking_reward_sigma"), 1.0f);
	m_impl->tracking_reward_scale = g_settings->getFloat("tracking_reward_scale");
	infostream << "[Tracking]   Tracking reward: "
	           << (m_impl->tracking_reward_gaussian ? "gaussian" : "linear")
	           << " scale=" << m_impl->tracking_reward_scale;
	if (m_impl->tracking_reward_gaussian)
		infostream << " sigma=" << m_impl->tracking_reward_sigma << "px";
	infostream << std::endl;

	// Initialize target control (unchanged)
	m_impl->target_control_socket = std::make_unique<network::UDPSocket>();
	network::Address target_bind_addr;
	target_bind_addr.host = "0.0.0.0";
	target_bind_addr.port = m_impl->target_control_port;

	if (m_impl->target_control_socket->Bind(target_bind_addr)) {
		m_impl->target_control_socket->SetNonBlocking(true);
		infostream << "[Tracking] ✓ Target control listening on UDP port "
		           << m_impl->target_control_port << std::endl;
	} else {
		warningstream << "[Tracking] ✗ Failed to bind target control port "
		              << m_impl->target_control_port << std::endl;
		m_impl->target_control_socket.reset();
	}

	return true;
#else
	warningstream << "[Tracking] Tracking export not compiled in (ENABLE_TRACKING_EXPORT=OFF)"
	              << std::endl;
	return false;
#endif
}

void TrackingExporter::shutdown()
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active)
		return;

	infostream << "[Tracking] Shutting down tracking export..." << std::endl;
	infostream << "[Tracking]   Mode: " << (m_impl->network_mode ? "NETWORK" : "LOCAL") << std::endl;
	infostream << "[Tracking]   Total frames exported: " << m_frames_exported << std::endl;
	infostream << "[Tracking]   Total frames failed: " << m_frames_failed << std::endl;

	// Shutdown network components
	if (m_impl->network_mode && m_impl->frame_producer) {
		infostream << "[Tracking] Disconnecting FrameProducer..." << std::endl;
		m_impl->frame_producer->Disconnect();
		m_impl->frame_producer.reset();
	}

	// Shutdown target control socket (still separate from FrameProducer)
	if (m_impl->target_control_socket) {
		infostream << "[Tracking] Closing target control socket..." << std::endl;
		m_impl->target_control_socket->Close();
		m_impl->target_control_socket.reset();
	}

	// Shutdown local mode components
	if (m_impl->bridge) {
		infostream << "[Tracking] Shutting down shared memory bridge..." << std::endl;
		m_impl->bridge->Shutdown();
		m_impl->bridge.reset();
	}

	m_impl->active = false;
	m_impl->network_mode = false;

	if (m_pixel_buffer) {
		delete[] m_pixel_buffer;
		m_pixel_buffer = nullptr;
	}

	infostream << "[Tracking] ✓ Tracking export shutdown complete" << std::endl;
#endif
}

bool TrackingExporter::isActive() const
{
	return m_impl->active;
}

bool TrackingExporter::isNetworkMode() const
{
#ifdef ENABLE_TRACKING_EXPORT
	return m_impl->network_mode;
#else
	return false;
#endif
}

void TrackingExporter::exportFramebuffer(video::IVideoDriver* driver)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !driver)
		return;

	// Check that we have the appropriate backend initialized
	if (m_impl->network_mode && !m_impl->frame_producer)
		return;
	if (!m_impl->network_mode && !m_impl->bridge)
		return;

	// Get screen size
	const core::dimension2du screen_size = driver->getScreenSize();
	u32 width = screen_size.Width;
	u32 height = screen_size.Height;

	// Allocate or reallocate pixel buffer if size changed
	if (width != m_frame_width || height != m_frame_height) {
		if (m_pixel_buffer) {
			delete[] m_pixel_buffer;
		}
		m_frame_width = width;
		m_frame_height = height;
		m_pixel_buffer = new u8[width * height * 3]; // RGB

		infostream << "[Tracking] Frame size changed to " << width << "x" << height << std::endl;
	}

	// Create image from screen
	video::IImage* image = driver->createScreenShot(video::ECF_R8G8B8);
	if (!image) {
		m_frames_failed++;
		if (m_frames_failed % 100 == 1) {
			errorstream << "[Tracking] Failed to capture framebuffer" << std::endl;
		}
		return;
	}

	// Copy image data to our buffer
	const u8* image_data = (const u8*)image->getData();
	memcpy(m_pixel_buffer, image_data, width * height * 3);
	image->drop();

	// Convert to grayscale for tracking pipeline (256x256 expected)
	// If frame is larger, we'll downsample or just send the first 256x256
	std::vector<u8> grayscale_data;

	u32 export_width = std::min(width, 256u);
	u32 export_height = std::min(height, 256u);

	grayscale_data.resize(export_width * export_height);

	for (u32 y = 0; y < export_height; y++) {
		for (u32 x = 0; x < export_width; x++) {
			u32 src_idx = (y * width + x) * 3;
			u32 dst_idx = y * export_width + x;

			// Convert RGB to grayscale (standard luminance formula)
			u8 r = m_pixel_buffer[src_idx + 0];
			u8 g = m_pixel_buffer[src_idx + 1];
			u8 b = m_pixel_buffer[src_idx + 2];

			grayscale_data[dst_idx] = (u8)((r * 0.299f) + (g * 0.587f) + (b * 0.114f));
		}
	}

	// DEBUG: Log exportFramebuffer invocation
	if (m_frames_exported % 60 == 0) {  // Log every 60 frames (~1 second at 60 FPS)
		infostream << "[TrackingExporter] exportFramebuffer called (frame " << m_frames_exported
		           << ", mode=" << (m_impl->network_mode ? "NETWORK" : "LOCAL") << ")" << std::endl;
	}

	// Send frame data based on mode
	if (m_impl->network_mode) {
		// DEBUG: Log network mode frame export attempt
		if (m_frames_exported < 5 || m_frames_exported % 60 == 0) {
			infostream << "[TrackingExporter] Attempting to send frame " << m_frames_exported
			           << " via network (session_id=" << m_impl->current_session_id << ")" << std::endl;
		}

		// Network mode: broadcast via UDP to all connected clients
		// Convert to FrameFixed256 format
		FrameFixed256 frame = {};  // Zero-initialize
		frame.session_id = m_impl->current_session_id;
		frame.sequence_number = static_cast<uint32_t>(m_frames_exported);
		frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
		frame.viewport_center_x = 0.0f;  // TODO: Get from player position
		frame.viewport_center_y = 0.0f;
		frame.viewport_scale = 1.0f;

		// Copy grayscale data to frame buffer (2D array)
		// Note: FrameFixed256.data is uint8_t[256][256]
		if (export_width == 256 && export_height == 256) {
			// Perfect size - direct copy
			for (u32 y = 0; y < 256; y++) {
				memcpy(frame.data[y], &grayscale_data[y * 256], 256);
			}
		} else {
			// Crop or zero-pad if not exactly 256x256
			for (u32 y = 0; y < 256; y++) {
				if (y < export_height) {
					u32 copy_width = std::min(export_width, 256u);
					memcpy(frame.data[y], &grayscale_data[y * export_width], copy_width);
					// Zero-pad remaining if needed
					if (copy_width < 256) {
						memset(&frame.data[y][copy_width], 0, 256 - copy_width);
					}
				} else {
					// Zero-fill rows beyond captured height
					memset(frame.data[y], 0, 256);
				}
			}
		}

		// Send this frame's reward stamp first, so the receiver has it when the
		// frame completes. Pending rewards are cleared even if the send fails;
		// the cumulative total keeps the receiver's step rewards exact.
		network::FrameRewardStamp stamp;
		stamp.frame_sequence = frame.sequence_number;
		stamp.last_action_seq = m_impl->last_applied_action_seq;
		stamp.producer_session_id = m_impl->current_session_id;
		stamp.timestamp_us = frame.timestamp_us;
		stamp.cumulative_reward = m_impl->stamp_cumulative_reward;
		stamp.frame_reward = m_impl->pending_frame_reward;
		stamp.sources = std::move(m_impl->pending_sources);
		m_impl->pending_sources.clear();
		m_impl->pending_frame_reward = 0.0f;
		m_impl->frame_producer->SendFrameRewards(stamp);

		// Send frame via P2P connection to paired receiver
		bool send_result = m_impl->frame_producer->SendFrame(frame);

		// DEBUG: Log SendFrame result
		if (!send_result) {
			m_frames_failed++;
			if (m_frames_failed <= 10 || m_frames_failed % 60 == 1) {
				errorstream << "[TrackingExporter] SendFrame FAILED (frame " << m_frames_exported
				            << ", total_failures=" << m_frames_failed << ")" << std::endl;
			}
			return;
		} else if (m_frames_exported < 5 || m_frames_exported % 60 == 0) {
			infostream << "[TrackingExporter] SendFrame SUCCESS (frame " << m_frames_exported << ")" << std::endl;
		}
	} else {
		// Local mode: write to shared memory via MinetestBridge
		m_impl->bridge->SetFrameData(export_width, export_height, grayscale_data);
	}

	m_frames_exported++;

	// Log statistics periodically
	if (m_frames_exported % 300 == 0) { // Every ~5 seconds at 60 FPS
		const char* mode_str = m_impl->network_mode ? "NETWORK" : "LOCAL";
		infostream << "[Tracking] Exported " << m_frames_exported << " frames "
		           << "(Mode: " << mode_str << ", " << export_width << "x" << export_height << ")" << std::endl;
	}
#endif
}

void TrackingExporter::exportPlayerState(const LocalPlayer* player, const Client* client)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !m_impl->bridge || !player)
		return;

	// Get player position
	v3f pos = player->getPosition();
	m_impl->bridge->SetViewportCenter(pos.X, pos.Z);

	// TODO: Export additional state:
	// - Health: player->hp
	// - Yaw/Pitch: player->getYaw(), player->getPitch()
	// - Breath: player->getBreath()
	// - Velocity, etc.
	//
	// This would require extending MinetestBridge API or using
	// reward events to communicate this data
#endif
}

bool TrackingExporter::getAgentActions(LocalPlayer* player)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !m_impl->bridge || !player)
		return false;

	// TODO: Read agent actions from shared memory and apply to player
	// This requires extending MinetestBridge API to communicate:
	// - Movement (forward, back, left, right)
	// - Looking (yaw, pitch)
	// - Actions (jump, dig, place)
	//
	// For now, this is a placeholder

	return false;
#else
	return false;
#endif
}

// Reward calculation helpers (from REWARD_DESIGN.md)
namespace {

// Calculate tracking accuracy reward based on distance to target
// linear:   reward = k * (1.0 - dist / max_dist)
//           where max_dist = diagonal of viewport (sqrt(256^2 + 256^2) ≈ 362 pixels)
// gaussian: reward = k * exp(-dist^2 / (2 * sigma^2))
// The linear shape changes by only ~10% between a target near the centre and
// a centred one, too little for an agent to learn from; the gaussian rewards
// centring itself.
float CalculateTrackingAccuracyReward(float target_x, float target_y,
                                       float viewport_center_x, float viewport_center_y,
                                       bool gaussian, float sigma, float k_tracking)
{
	constexpr float MAX_DISTANCE = 362.0f;  // sqrt(256*256 + 256*256)

	float dx = target_x - viewport_center_x;
	float dy = target_y - viewport_center_y;
	float distance_sq = dx * dx + dy * dy;

	if (gaussian)
		return k_tracking * std::exp(-distance_sq / (2.0f * sigma * sigma));

	float normalized_distance = std::min(std::sqrt(distance_sq) / MAX_DISTANCE, 1.0f);
	return k_tracking * (1.0f - normalized_distance);
}

// Calculate survival reward (simple per-frame reward for being alive)
// Encourages agent to stay alive and engaged
float CalculateSurvivalReward(float k_survival = 0.01f)
{
	return k_survival;
}

// Calculate health change reward
// Positive for healing, negative for damage
float CalculateHealthChangeReward(float health_delta, float k_health = 0.1f)
{
	return k_health * health_delta;
}

// Calculate target acquisition reward (sparse reward for centering target)
// Returns positive reward if target is within acquisition threshold
bool CheckTargetAcquisition(float target_x, float target_y,
                            float viewport_center_x, float viewport_center_y,
                            float acquisition_threshold = 30.0f)
{
	float dx = target_x - viewport_center_x;
	float dy = target_y - viewport_center_y;
	float distance = std::sqrt(dx * dx + dy * dy);

	return distance < acquisition_threshold;
}

#ifdef ENABLE_TRACKING_EXPORT
// Whether a motion pattern moves the target on its own
bool IsPatternMotion(network::TargetMotionPattern pattern)
{
	return pattern == network::MOTION_LINEAR ||
		pattern == network::MOTION_CIRCULAR ||
		pattern == network::MOTION_SINUSOIDAL;
}

// Displacement along the anchor's (u, v) motion axes after elapsed_sec
v2f MotionOffset(const network::TargetMotion &motion, float elapsed_sec)
{
	switch (motion.pattern) {
		case network::MOTION_LINEAR:
			return v2f(motion.velocity_x * elapsed_sec, motion.velocity_y * elapsed_sec);

		case network::MOTION_CIRCULAR: {
			float angle = 2.0f * M_PI * motion.frequency * elapsed_sec + motion.phase;
			return v2f(motion.amplitude * std::cos(angle), motion.amplitude * std::sin(angle));
		}

		case network::MOTION_SINUSOIDAL: {
			float phase = 2.0f * M_PI * motion.frequency * elapsed_sec + motion.phase;
			return v2f(motion.amplitude * std::sin(phase), 0.0f);
		}

		default:
			return v2f(0.0f, 0.0f);
	}
}

// True if a node that blocks light lies between eye and target (world nodes).
// Same criterion as Map::isOccluded; the nodes at both ends are skipped so a
// target placed inside or against a node stays visible.
bool IsWorldTargetOccluded(Client *client, v3f eye, v3f target)
{
	constexpr float STEP = 0.25f;
	constexpr float MAX_CHECK_DISTANCE = 256.0f;

	v3f direction = target - eye;
	float distance = std::min(direction.getLength(), MAX_CHECK_DISTANCE);
	if (distance < 1.0f)
		return false;
	direction.normalize();

	Map &map = client->getEnv().getMap();
	const NodeDefManager *ndef = client->ndef();
	for (float t = 0.5f; t < distance - 0.5f; t += STEP) {
		bool is_valid_position = false;
		MapNode node = map.getNode(floatToInt(eye + direction * t, 1.0f), &is_valid_position);
		if (is_valid_position && !ndef->getLightingFlags(node).light_propagates)
			return true;
	}
	return false;
}

// Draw one target marker centred on a screen position
void DrawTargetMarker(video::IVideoDriver *driver, const network::TargetPacket &target,
		int screen_x, int screen_y)
{
	// Create Irrlicht color from target appearance
	video::SColor color(
		target.appearance.alpha,
		target.appearance.red,
		target.appearance.green,
		target.appearance.blue
	);

	// Render based on target type
	switch (target.target_type) {
		case network::TARGET_CIRCLE: {
			// Draw circle as filled disk using small rectangles
			int radius = static_cast<int>(target.appearance.size / 2.0f);
			// Approximate circle with rectangles
			for (int dy = -radius; dy <= radius; dy++) {
				int width = static_cast<int>(std::sqrt(radius * radius - dy * dy));
				if (width > 0) {
					core::rect<s32> rect(
						screen_x - width,
						screen_y + dy,
						screen_x + width,
						screen_y + dy + 1
					);
					driver->draw2DRectangle(color, rect);
				}
			}
			break;
		}

		case network::TARGET_SQUARE: {
			// Draw filled square
			int half_size = static_cast<int>(target.appearance.size / 2.0f);
			core::rect<s32> rect(
				screen_x - half_size,
				screen_y - half_size,
				screen_x + half_size,
				screen_y + half_size
			);
			driver->draw2DRectangle(color, rect);
			break;
		}

		case network::TARGET_CROSS: {
			// Draw crosshair
			int half_size = static_cast<int>(target.appearance.size / 2.0f);
			int thickness = static_cast<int>(target.appearance.thickness);

			// Horizontal line
			core::rect<s32> h_line(
				screen_x - half_size,
				screen_y - thickness / 2,
				screen_x + half_size,
				screen_y + thickness / 2
			);
			driver->draw2DRectangle(color, h_line);

			// Vertical line
			core::rect<s32> v_line(
				screen_x - thickness / 2,
				screen_y - half_size,
				screen_x + thickness / 2,
				screen_y + half_size
			);
			driver->draw2DRectangle(color, v_line);
			break;
		}

		case network::TARGET_ENTITY:
			// Entity-based targets handled by Lua mod, not rendered here
			break;

		default:
			break;
	}
}
#endif

} // anonymous namespace

bool TrackingExporter::applyCameraControl(float &camera_yaw, float &camera_pitch,
		float fov_x, float fov_y)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active)
		return false;

	// Network mode: poll action from FrameProducer
	// Local mode: not implemented yet (would use MinetestBridge)
	if (!m_impl->network_mode || !m_impl->frame_producer)
		return false;

	// Poll for action command from FrameProducer
	auto cmd_opt = m_impl->frame_producer->PollAction();
	if (!cmd_opt) {
		return false;  // No action available
	}

	const GazeCommand& cmd = *cmd_opt;

	// Frames rendered from now on reflect this action (reported in reward stamps)
	m_impl->last_applied_action_seq = cmd.sequence_number;

	// Update statistics
	m_action_stats.actions_received++;

	// Note: Latency tracking would require timestamp in GazeCommand
	// For now, just track receive time
	auto now = std::chrono::high_resolution_clock::now();
	uint64_t receive_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();
	m_action_stats.last_action_time_us = receive_time_us;

	// Get current camera state
	float current_yaw = camera_yaw;
	float current_pitch = camera_pitch;
	float new_yaw = current_yaw;
	float new_pitch = current_pitch;

	// Apply command based on type
	bool applied = false;
	switch (cmd.type) {
		case CMD_SACCADE: {
			// Ballistic movement to target
			// Convert viewport coordinates (0-256) to camera angles
			// Viewport center (128, 128) = current look direction (0, 0)
			// Positive X = right, Positive Y = down
			// Luanti: yaw decreases turning right, positive pitch looks down

			float dx = cmd.target_x - 128.0f;  // Pixels from center
			float dy = cmd.target_y - 128.0f;

			// Turn by the angle the pixel subtends through the perspective
			// projection, so the saccade lands on what was drawn there
			// (yaw and pitch are treated independently; exact on the axes)
			float yaw_delta = -std::atan(dx / 128.0f * std::tan(fov_x / 2.0f)) * core::RADTODEG;
			float pitch_delta = std::atan(dy / 128.0f * std::tan(fov_y / 2.0f)) * core::RADTODEG;

			new_yaw = current_yaw + yaw_delta;
			new_pitch = current_pitch + pitch_delta;
			applied = true;
			break;
		}

		case CMD_PURSUIT: {
			// Smooth tracking with velocity
			// Use velocity to predict where to look
			float yaw_delta = -cmd.velocity_x * 0.5f;
			float pitch_delta = cmd.velocity_y * 0.5f;

			new_yaw = current_yaw + yaw_delta;
			new_pitch = current_pitch + pitch_delta;
			applied = true;
			break;
		}

		case CMD_FIXATION: {
			// Maintain current position (with small corrections)
			// Target represents desired fixation point
			float dx = cmd.target_x - 128.0f;
			float dy = cmd.target_y - 128.0f;

			// Apply small correction (scaled down for stability)
			float yaw_delta = -dx * 0.1f;
			float pitch_delta = dy * 0.1f;

			new_yaw = current_yaw + yaw_delta;
			new_pitch = current_pitch + pitch_delta;
			applied = true;
			break;
		}

		case CMD_DRIFT: {
			// Natural drift correction
			// Similar to fixation but even smaller corrections
			float dx = cmd.target_x - 128.0f;
			float dy = cmd.target_y - 128.0f;

			float yaw_delta = -dx * 0.05f;
			float pitch_delta = dy * 0.05f;

			new_yaw = current_yaw + yaw_delta;
			new_pitch = current_pitch + pitch_delta;
			applied = true;
			break;
		}

		case CMD_RESET: {
			// Return to center (0, 0)
			new_yaw = 0.0f;
			new_pitch = 0.0f;
			applied = true;
			break;
		}

		case CMD_NONE:
		default:
			// No action
			break;
	}

	if (applied) {
		camera_yaw = new_yaw;
		camera_pitch = std::max(-90.0f, std::min(90.0f, new_pitch));
		m_action_stats.actions_applied++;
	} else {
		m_action_stats.actions_failed++;
	}

	return applied;
#else
	return false;
#endif
}

void TrackingExporter::registerRewardEvent(const std::string& event_type, float reward_value)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active)
		return;

	if (m_impl->network_mode) {
		// Network mode: Build reward info and send via TCP
		RewardInfo reward;
		reward.immediate_reward = reward_value;
		reward.cumulative_reward = m_impl->cumulative_reward + reward_value;
		reward.source = event_type;
		reward.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
		reward.type = RewardType::DENSE;  // Default to dense, can be overridden
		reward.session_id = m_impl->current_session_id;
		reward.is_terminal = false;

		// Update cumulative reward
		m_impl->cumulative_reward += reward_value;
		m_impl->reward_count++;

		// Store in history (keep last 1000 rewards to avoid memory growth)
		m_impl->reward_history.push_back(reward);
		if (m_impl->reward_history.size() > 1000) {
			m_impl->reward_history.pop_front();
		}

		// Accumulate into the next frame's reward stamp (sent in exportFramebuffer)
		m_impl->stamp_cumulative_reward += reward_value;
		m_impl->pending_frame_reward += reward_value;
		auto src_it = std::find_if(m_impl->pending_sources.begin(), m_impl->pending_sources.end(),
			[&event_type](const network::RewardSourceTotal &s) { return s.source == event_type; });
		if (src_it == m_impl->pending_sources.end()) {
			m_impl->pending_sources.push_back({event_type, reward_value, 1});
		} else {
			src_it->reward += reward_value;
			src_it->count++;
		}

		// Log periodically (every 100 rewards)
		if (m_impl->reward_count % 100 == 1) {
			infostream << "[Tracking] Rewards: count=" << m_impl->reward_count
			           << ", cumulative=" << m_impl->cumulative_reward
			           << ", latest_source=" << event_type
			           << ", latest_value=" << reward_value << std::endl;
		}
	} else {
		// Local mode: Use MinetestBridge
		if (!m_impl->bridge)
			return;

		MinetestBridge::GameRewardEvent event;
		event.event_type = event_type;
		event.reward_value = reward_value;
		event.target_id = 0; // No target for general events
		event.timestamp_us = 0; // Will be set by bridge

		m_impl->bridge->RegisterRewardEvent(event);
	}
#endif
}

TrackingExporter::ActionStats TrackingExporter::getActionStats() const
{
	return m_action_stats;
}

void TrackingExporter::resetActionStats()
{
	m_action_stats.Reset();
}

network::ProducerStats TrackingExporter::getProducerStats() const
{
#ifdef ENABLE_TRACKING_EXPORT
	if (m_impl->network_mode && m_impl->frame_producer) {
		return m_impl->frame_producer->GetStats();
	}
#endif
	// Return empty stats if not in network mode
	return network::ProducerStats{};
}

network::ConnectionHealthStats TrackingExporter::getConnectionHealth() const
{
#ifdef ENABLE_TRACKING_EXPORT
	if (m_impl->network_mode && m_impl->frame_producer) {
		return m_impl->frame_producer->GetConnectionHealth();
	}
#endif
	// Return empty health stats if not in network mode
	return network::ConnectionHealthStats{};
}

int TrackingExporter::processTargetCommands()
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !m_impl->network_mode || !m_impl->target_control_socket) {
		return 0;
	}

	int commands_processed = 0;
	uint8_t buffer[512];
	network::Address sender;

	// Process up to 10 commands per frame (prevent overload)
	for (int i = 0; i < 10; i++) {
		int received = m_impl->target_control_socket->ReceiveFrom(buffer, sizeof(buffer), sender);

		if (received < static_cast<int>(network::TargetPacket::MIN_PACKET_SIZE)) {
			break;  // No more packets or invalid size
		}

		// Deserialize TargetPacket
		auto packet_opt = network::TargetPacket::Deserialize(buffer, received);
		if (!packet_opt) {
			warningstream << "[Tracking] Failed to deserialize TargetPacket" << std::endl;
			continue;
		}

		const network::TargetPacket& packet = *packet_opt;

		// Process command
		switch (packet.command_type) {
			case network::TARGET_CMD_SPAWN: {
				// Add new target to active targets
				m_impl->active_targets[packet.target_id] = packet;

				// Initialize target state for motion patterns
				Impl::TargetState state;
				auto now = std::chrono::high_resolution_clock::now();
				state.start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
					now.time_since_epoch()).count();
				state.initial_x = packet.position_x;
				state.initial_y = packet.position_y;
				m_impl->target_states[packet.target_id] = state;

				infostream << "[Tracking] Target spawned: ID=" << packet.target_id
				           << " at (" << packet.position_x << "," << packet.position_y;
				if (packet.anchor != network::ANCHOR_SCREEN)
					infostream << "," << packet.position_z;
				infostream << ") anchor=" << network::GetTargetAnchorName(packet.anchor)
				           << " type=" << network::GetTargetTypeName(packet.target_type) << std::endl;
				break;
			}

			case network::TARGET_CMD_UPDATE: {
				// Update existing target
				auto it = m_impl->active_targets.find(packet.target_id);
				if (it != m_impl->active_targets.end()) {
					it->second = packet;
					// A world target's new position is re-resolved on the next render
					m_impl->target_states[packet.target_id].world_resolved = false;
				}
				break;
			}

			case network::TARGET_CMD_DELETE: {
				// Remove target
				m_impl->active_targets.erase(packet.target_id);
				m_impl->target_states.erase(packet.target_id);
				infostream << "[Tracking] Target deleted: ID=" << packet.target_id << std::endl;
				break;
			}

			case network::TARGET_CMD_PATTERN: {
				// Update motion pattern only
				auto it = m_impl->active_targets.find(packet.target_id);
				if (it != m_impl->active_targets.end()) {
					it->second.motion = packet.motion;

					// Reset motion state
					auto& state = m_impl->target_states[packet.target_id];
					auto now = std::chrono::high_resolution_clock::now();
					state.start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
						now.time_since_epoch()).count();
					state.initial_x = it->second.position_x;
					state.initial_y = it->second.position_y;
					// World targets start the new pattern from where they are now
					state.world_origin = state.world_current;
				}
				break;
			}

			case network::TARGET_CMD_CLEAR: {
				m_impl->active_targets.clear();
				m_impl->target_states.clear();
				infostream << "[Tracking] All targets cleared" << std::endl;
				break;
			}

			default:
				warningstream << "[Tracking] Unknown target command type: "
				              << static_cast<int>(packet.command_type) << std::endl;
				continue;
		}

		commands_processed++;
	}

	return commands_processed;
#else
	return 0;
#endif
}

void TrackingExporter::renderTargets(video::IVideoDriver* driver, Camera* camera, Client* client)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !driver || m_impl->active_targets.empty()) {
		return;
	}

	// Get current time for motion pattern updates
	auto now = std::chrono::high_resolution_clock::now();
	uint64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();

	// Camera state for WORLD/VIEW targets (all positions in world nodes)
	scene::ICameraSceneNode *cam_node = camera ? camera->getCameraNode() : nullptr;
	core::matrix4 view_projection;
	v3f camera_offset, eye, forward, right, up;
	if (cam_node) {
		view_projection = cam_node->getProjectionMatrix();
		view_projection *= cam_node->getViewMatrix();
		camera_offset = intToFloat(camera->getOffset(), BS);
		eye = (cam_node->getAbsolutePosition() + camera_offset) / BS;
		forward = (cam_node->getTarget() - cam_node->getAbsolutePosition()).normalize();
		right = cam_node->getUpVector().crossProduct(forward).normalize();
		up = forward.crossProduct(right);
	}
	const v2u32 screen_size = driver->getScreenSize();

	// Update and render each target
	for (auto& pair : m_impl->active_targets) {
		uint64_t target_id = pair.first;
		network::TargetPacket& target = pair.second;
		auto state_it = m_impl->target_states.find(target_id);

		if (state_it == m_impl->target_states.end()) {
			continue;  // No state for this target
		}

		Impl::TargetState& state = state_it->second;
		float elapsed_sec = (current_time_us - state.start_time_us) / 1000000.0f;

		// Viewport position (0-256, centre 128,128) used for the reward, and
		// whether the target is in front of the camera at all
		float render_x = 0.0f;
		float render_y = 0.0f;
		bool in_front = true;

		if (target.anchor == network::ANCHOR_SCREEN) {
			// Update position based on motion pattern
			render_x = target.position_x;
			render_y = target.position_y;

			if (IsPatternMotion(target.motion.pattern)) {
				v2f offset = MotionOffset(target.motion, elapsed_sec);
				render_x = state.initial_x + offset.X;
				render_y = state.initial_y + offset.Y;

				// Update target position for next frame
				target.position_x = render_x;
				target.position_y = render_y;
			}

			// Clamp to viewport bounds (0-256)
			render_x = std::max(0.0f, std::min(256.0f, render_x));
			render_y = std::max(0.0f, std::min(256.0f, render_y));

			// Viewport is rendered in top-left corner at 256x256
			DrawTargetMarker(driver, target,
				static_cast<int>(render_x), static_cast<int>(render_y));
		} else {
			if (!cam_node)
				continue;

			if (!state.world_resolved) {
				v3f position(target.position_x, target.position_y, target.position_z);
				if (target.anchor == network::ANCHOR_VIEW) {
					state.world_origin = eye + right * position.X + up * position.Y + forward * position.Z;
					state.axis_u = right;
					state.axis_v = up;
				} else {
					state.world_origin = position;
					state.axis_u = v3f(1.0f, 0.0f, 0.0f);
					state.axis_v = v3f(0.0f, 1.0f, 0.0f);
				}
				state.world_resolved = true;
			}

			v3f world_pos = state.world_origin;
			if (IsPatternMotion(target.motion.pattern)) {
				v2f offset = MotionOffset(target.motion, elapsed_sec);
				world_pos += state.axis_u * offset.X + state.axis_v * offset.Y;
			}
			state.world_current = world_pos;

			// Project through the camera (same transform as Hud::calculateScreenPos)
			v3f scene_pos = world_pos * BS - camera_offset;
			f32 clip[4] = { scene_pos.X, scene_pos.Y, scene_pos.Z, 1.0f };
			view_projection.multiplyWith1x4Matrix(clip);
			in_front = clip[3] > 0.0f;

			if (in_front) {
				render_x = 128.0f * (1.0f + clip[0] / clip[3]);
				render_y = 128.0f * (1.0f - clip[1] / clip[3]);

				bool on_screen = render_x >= 0.0f && render_x <= 256.0f &&
					render_y >= 0.0f && render_y <= 256.0f;
				// Occlusion only hides the marker; the reward still follows the
				// target's position, as it would for an object behind a wall
				if (on_screen && !(client && IsWorldTargetOccluded(client, eye, world_pos))) {
					DrawTargetMarker(driver, target,
						static_cast<int>(render_x * screen_size.X / 256.0f),
						static_cast<int>(render_y * screen_size.Y / 256.0f));
				}
			}
		}

		// Calculate and register rewards for this target
		constexpr float VIEWPORT_CENTER_X = 128.0f;
		constexpr float VIEWPORT_CENTER_Y = 128.0f;

		// Tracking accuracy reward (dense, every frame); zero behind the camera
		float tracking_reward = in_front ? CalculateTrackingAccuracyReward(
			render_x, render_y, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y,
			m_impl->tracking_reward_gaussian, m_impl->tracking_reward_sigma,
			m_impl->tracking_reward_scale) : 0.0f;
		registerRewardEvent("tracking_accuracy", tracking_reward);

		// Target acquisition reward (sparse, only when first centered)
		bool is_acquired = in_front && CheckTargetAcquisition(
			render_x, render_y, VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y);

		if (is_acquired && !state.was_acquired) {
			// Target just became acquired - give sparse reward
			registerRewardEvent("target_acquisition", 1.0f);
			state.was_acquired = true;
		} else if (!is_acquired && state.was_acquired) {
			// Target left acquisition zone - reset flag
			state.was_acquired = false;
		}
	}
#endif
}

} // namespace tracking

