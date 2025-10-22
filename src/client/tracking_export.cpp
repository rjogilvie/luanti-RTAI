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

// Include tracking pipeline headers OUTSIDE namespace
#ifdef ENABLE_TRACKING_EXPORT
#include "minetest_bridge.h"
#include "frame_distributor.h"
#include "session_coordinator.h"
#include "frame_formats.h"
#endif

namespace tracking {

struct TrackingExporter::Impl {
#ifdef ENABLE_TRACKING_EXPORT
	// Local mode (shared memory)
	std::unique_ptr<MinetestBridge> bridge;

	// Network mode (UDP frame broadcasting)
	std::unique_ptr<network::FrameDistributor> frame_distributor;

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

} // namespace tracking

