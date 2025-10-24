// Luanti Tracking Export
// Integration with visual tracking pipeline for RL training
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "tracking_export.h"
#include "localplayer.h"
#include "client.h"
#include "log.h"

#include <IVideoDriver.h>
#include <memory>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <cmath>

// Include tracking pipeline headers OUTSIDE namespace
#ifdef ENABLE_TRACKING_EXPORT
#include "minetest_bridge.h"
#include "frame_distributor.h"
#include "session_coordinator.h"
#include "frame_formats.h"
#include "network_transport.h"
#include "action_distributor.h"
#include "target_control.h"
#endif

namespace tracking {

struct TrackingExporter::Impl {
#ifdef ENABLE_TRACKING_EXPORT
	// Local mode (shared memory)
	std::unique_ptr<MinetestBridge> bridge;

	// Network mode (UDP frame broadcasting)
	std::unique_ptr<network::FrameDistributor> frame_distributor;

	// Camera control (UDP receiver for agent control)
	std::unique_ptr<network::UDPSocket> camera_control_socket;
	uint16_t camera_control_port = 8000;

	// Target control (UDP receiver for target commands)
	std::unique_ptr<network::UDPSocket> target_control_socket;
	uint16_t target_control_port = 8002;

	// Active targets (target_id -> TargetPacket)
	std::unordered_map<uint64_t, network::TargetPacket> active_targets;

	// Target motion state (for pattern updates)
	struct TargetState {
		uint64_t start_time_us = 0;
		float initial_x = 0.0f;
		float initial_y = 0.0f;
	};
	std::unordered_map<uint64_t, TargetState> target_states;

	// Mode flag
	bool network_mode = false;  // false = local (shared memory), true = network
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

bool TrackingExporter::initializeNetwork(int port, const std::string& bind_addr,
                                          const std::string& broadcast_addr)
{
#ifdef ENABLE_TRACKING_EXPORT
	infostream << "[Tracking] Initializing tracking export system (NETWORK MODE)..." << std::endl;
	infostream << "[Tracking]   UDP Port: " << port << std::endl;
	infostream << "[Tracking]   Bind address: " << bind_addr << std::endl;
	infostream << "[Tracking]   Broadcast address: " << broadcast_addr << std::endl;

	// Create frame distributor (broadcasts frames via UDP)
	m_impl->frame_distributor = std::make_unique<network::FrameDistributor>();

	// Bind to any available port (sender doesn't need specific port)
	network::Address sender_addr;
	sender_addr.port = 0;  // 0 = let OS choose available port
	sender_addr.host = bind_addr;

	if (!m_impl->frame_distributor->Initialize(sender_addr)) {
		errorstream << "[Tracking] Failed to initialize frame distributor" << std::endl;
		return false;
	}

	// Enable broadcast mode (send TO destination port)
	network::Address bcast_addr;
	bcast_addr.port = port;  // Receivers listen on this port
	bcast_addr.host = broadcast_addr;  // Configurable broadcast address
	m_impl->frame_distributor->SetBroadcastMode(true, bcast_addr);

	// Enable compression for network mode (reduces bandwidth)
	m_impl->frame_distributor->SetCompressionEnabled(true);
	m_impl->frame_distributor->SetCompressionType(network::CompressionType::ZSTD);

	m_impl->network_mode = true;
	m_impl->active = true;

	infostream << "[Tracking] ✓ Tracking export initialized (NETWORK MODE)" << std::endl;
	infostream << "[Tracking]   Broadcasting TO: " << broadcast_addr << ":" << port << std::endl;
	infostream << "[Tracking]   Compression: ZSTD" << std::endl;
	infostream << "[Tracking]   Ready to broadcast frames to all listeners" << std::endl;
	infostream << "[Tracking]   (Receivers should bind to port " << port << ")" << std::endl;

	// Initialize camera control receiver (UDP)
	m_impl->camera_control_socket = std::make_unique<network::UDPSocket>();
	network::Address control_bind_addr;
	control_bind_addr.host = bind_addr;
	control_bind_addr.port = m_impl->camera_control_port;

	if (m_impl->camera_control_socket->Bind(control_bind_addr)) {
		m_impl->camera_control_socket->SetNonBlocking(true);
		infostream << "[Tracking] ✓ Camera control listening on UDP port "
		           << m_impl->camera_control_port << std::endl;
		infostream << "[Tracking]   Ready to receive camera control commands" << std::endl;
	} else {
		warningstream << "[Tracking] ✗ Failed to bind camera control port "
		              << m_impl->camera_control_port << std::endl;
		warningstream << "[Tracking]   Camera control will be unavailable" << std::endl;
		m_impl->camera_control_socket.reset();
	}

	// Initialize target control receiver (UDP)
	m_impl->target_control_socket = std::make_unique<network::UDPSocket>();
	network::Address target_bind_addr;
	target_bind_addr.host = bind_addr;
	target_bind_addr.port = m_impl->target_control_port;

	if (m_impl->target_control_socket->Bind(target_bind_addr)) {
		m_impl->target_control_socket->SetNonBlocking(true);
		infostream << "[Tracking] ✓ Target control listening on UDP port "
		           << m_impl->target_control_port << std::endl;
		infostream << "[Tracking]   Ready to receive target spawn/control commands" << std::endl;
	} else {
		warningstream << "[Tracking] ✗ Failed to bind target control port "
		              << m_impl->target_control_port << std::endl;
		warningstream << "[Tracking]   Target control will be unavailable" << std::endl;
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
	if (m_impl->network_mode && m_impl->frame_distributor) {
		infostream << "[Tracking] Stopping frame distributor..." << std::endl;
		// No explicit shutdown needed for FrameDistributor - just reset
		m_impl->frame_distributor.reset();
	}

	// Shutdown camera control socket
	if (m_impl->camera_control_socket) {
		infostream << "[Tracking] Closing camera control socket..." << std::endl;
		m_impl->camera_control_socket->Close();
		m_impl->camera_control_socket.reset();
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
	if (m_impl->network_mode && !m_impl->frame_distributor)
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

	// Send frame data based on mode
	if (m_impl->network_mode) {
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

		// Broadcast to all clients
		if (!m_impl->frame_distributor->BroadcastFrame(frame)) {
			m_frames_failed++;
			if (m_frames_failed % 100 == 1) {
				errorstream << "[Tracking] Failed to broadcast frame via network" << std::endl;
			}
			return;
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

bool TrackingExporter::applyCameraControl(LocalPlayer* player)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !m_impl->camera_control_socket || !player)
		return false;

	// Receive ActionPacket from UDP socket
	uint8_t buffer[512];  // Large enough for ActionPacket
	network::Address sender;
	int received = m_impl->camera_control_socket->ReceiveFrom(buffer, sizeof(buffer), sender);

	if (received < static_cast<int>(network::ActionPacket::PACKET_SIZE)) {
		return false;  // No packet or incomplete packet
	}

	// Deserialize ActionPacket
	auto packet_opt = network::ActionPacket::Deserialize(buffer, received);
	if (!packet_opt) {
		m_action_stats.actions_failed++;
		return false;
	}

	const network::ActionPacket& packet = *packet_opt;
	const GazeCommand& cmd = packet.command;

	// Update statistics
	m_action_stats.actions_received++;

	// Calculate latency (send_time → receive_time)
	auto now = std::chrono::high_resolution_clock::now();
	uint64_t receive_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();

	double latency_us = static_cast<double>(receive_time_us - packet.send_time_us);

	// Update latency statistics
	if (m_action_stats.actions_applied == 0) {
		// First action
		m_action_stats.average_latency_us = latency_us;
		m_action_stats.min_latency_us = latency_us;
		m_action_stats.max_latency_us = latency_us;
	} else {
		// Exponential moving average
		m_action_stats.average_latency_us = 0.9 * m_action_stats.average_latency_us + 0.1 * latency_us;

		// Update min/max
		if (latency_us < m_action_stats.min_latency_us) {
			m_action_stats.min_latency_us = latency_us;
		}
		if (latency_us > m_action_stats.max_latency_us) {
			m_action_stats.max_latency_us = latency_us;
		}
	}

	m_action_stats.last_action_time_us = receive_time_us;

	// Get current camera state
	float current_yaw = player->getYaw();
	float current_pitch = player->getPitch();

	// Apply command based on type
	bool applied = false;
	switch (cmd.type) {
		case CMD_SACCADE: {
			// Ballistic movement to target
			// Convert viewport coordinates (0-256) to camera angles
			// Viewport center (128, 128) = current look direction (0, 0)
			// Positive X = right, Positive Y = down

			float dx = cmd.target_x - 128.0f;  // Pixels from center
			float dy = cmd.target_y - 128.0f;

			// Convert to degrees (rough approximation: 1 pixel ≈ 0.5 degrees)
			float yaw_delta = dx * 0.5f;
			float pitch_delta = -dy * 0.5f;  // Invert Y axis

			player->setYaw(current_yaw + yaw_delta);
			player->setPitch(current_pitch + pitch_delta);
			applied = true;
			break;
		}

		case CMD_PURSUIT: {
			// Smooth tracking with velocity
			// Use velocity to predict where to look
			float yaw_delta = cmd.velocity_x * 0.5f;
			float pitch_delta = -cmd.velocity_y * 0.5f;

			player->setYaw(current_yaw + yaw_delta);
			player->setPitch(current_pitch + pitch_delta);
			applied = true;
			break;
		}

		case CMD_FIXATION: {
			// Maintain current position (with small corrections)
			// Target represents desired fixation point
			float dx = cmd.target_x - 128.0f;
			float dy = cmd.target_y - 128.0f;

			// Apply small correction (scaled down for stability)
			float yaw_delta = dx * 0.1f;
			float pitch_delta = -dy * 0.1f;

			player->setYaw(current_yaw + yaw_delta);
			player->setPitch(current_pitch + pitch_delta);
			applied = true;
			break;
		}

		case CMD_DRIFT: {
			// Natural drift correction
			// Similar to fixation but even smaller corrections
			float dx = cmd.target_x - 128.0f;
			float dy = cmd.target_y - 128.0f;

			float yaw_delta = dx * 0.05f;
			float pitch_delta = -dy * 0.05f;

			player->setYaw(current_yaw + yaw_delta);
			player->setPitch(current_pitch + pitch_delta);
			applied = true;
			break;
		}

		case CMD_RESET: {
			// Return to center (0, 0)
			player->setYaw(0.0f);
			player->setPitch(0.0f);
			applied = true;
			break;
		}

		case CMD_NONE:
		default:
			// No action
			break;
	}

	if (applied) {
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
	if (!m_impl->active || !m_impl->bridge)
		return;

	MinetestBridge::GameRewardEvent event;
	event.event_type = event_type;
	event.reward_value = reward_value;
	event.target_id = 0; // No target for general events
	event.timestamp_us = 0; // Will be set by bridge

	m_impl->bridge->RegisterRewardEvent(event);
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

		if (received < static_cast<int>(network::TargetPacket::PACKET_SIZE)) {
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
				           << " at (" << packet.position_x << "," << packet.position_y << ")"
				           << " type=" << network::GetTargetTypeName(packet.target_type) << std::endl;
				break;
			}

			case network::TARGET_CMD_UPDATE: {
				// Update existing target
				auto it = m_impl->active_targets.find(packet.target_id);
				if (it != m_impl->active_targets.end()) {
					it->second = packet;
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
				}
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

void TrackingExporter::renderTargets(video::IVideoDriver* driver)
{
#ifdef ENABLE_TRACKING_EXPORT
	if (!m_impl->active || !driver || m_impl->active_targets.empty()) {
		return;
	}

	// Get current time for motion pattern updates
	auto now = std::chrono::high_resolution_clock::now();
	uint64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
		now.time_since_epoch()).count();

	// Update and render each target
	for (auto& pair : m_impl->active_targets) {
		uint64_t target_id = pair.first;
		network::TargetPacket& target = pair.second;
		const auto& state_it = m_impl->target_states.find(target_id);

		if (state_it == m_impl->target_states.end()) {
			continue;  // No state for this target
		}

		const Impl::TargetState& state = state_it->second;

		// Update position based on motion pattern
		float render_x = target.position_x;
		float render_y = target.position_y;

		if (target.motion.pattern != network::MOTION_STATIC) {
			float elapsed_sec = (current_time_us - state.start_time_us) / 1000000.0f;

			switch (target.motion.pattern) {
				case network::MOTION_LINEAR: {
					render_x = state.initial_x + target.motion.velocity_x * elapsed_sec;
					render_y = state.initial_y + target.motion.velocity_y * elapsed_sec;
					break;
				}

				case network::MOTION_CIRCULAR: {
					float angle = 2.0f * M_PI * target.motion.frequency * elapsed_sec + target.motion.phase;
					render_x = state.initial_x + target.motion.amplitude * std::cos(angle);
					render_y = state.initial_y + target.motion.amplitude * std::sin(angle);
					break;
				}

				case network::MOTION_SINUSOIDAL: {
					float phase = 2.0f * M_PI * target.motion.frequency * elapsed_sec + target.motion.phase;
					render_x = state.initial_x + target.motion.amplitude * std::sin(phase);
					render_y = state.initial_y;
					break;
				}

				case network::MOTION_RANDOM:
					// Random walk would require storing random state - skip for now
					break;

				case network::MOTION_CUSTOM:
					// Use position from packet directly
					break;

				default:
					break;
			}

			// Update target position for next frame
			target.position_x = render_x;
			target.position_y = render_y;
		}

		// Clamp to viewport bounds (0-256)
		render_x = std::max(0.0f, std::min(256.0f, render_x));
		render_y = std::max(0.0f, std::min(256.0f, render_y));

		// Convert viewport coordinates (0-256) to screen coordinates
		// Viewport is rendered in top-left corner at 256x256
		int screen_x = static_cast<int>(render_x);
		int screen_y = static_cast<int>(render_y);

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
}

} // namespace tracking

