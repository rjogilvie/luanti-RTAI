// Luanti Tracking Export
// Integration with visual tracking pipeline for RL training
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include <string>

namespace video {
	class IVideoDriver;
}

class LocalPlayer;
class Client;

namespace tracking {

/**
 * TrackingExporter - Exports game state and framebuffer to tracking pipeline
 *
 * This class integrates Luanti with the visual tracking pipeline by:
 * 1. Capturing rendered framebuffers using OpenGL
 * 2. Exporting player state (health, position, orientation)
 * 3. Receiving agent actions from RL algorithms
 * 4. Managing communication (shared memory OR network)
 *
 * Supports two modes:
 * - Local mode: Uses POSIX shared memory for single-machine training
 * - Network mode: Uses UDP/TCP for distributed multi-machine training
 */
class TrackingExporter {
public:
	TrackingExporter();
	~TrackingExporter();

	/**
	 * Initialize the tracking export system in LOCAL mode
	 * Uses POSIX shared memory for single-machine communication
	 *
	 * @param shared_memory_name Name of shared memory segment (default: "tracking_viewport")
	 * @return true if initialization succeeded
	 */
	bool initializeLocal(const std::string& shared_memory_name = "tracking_viewport");

	/**
	 * Initialize the tracking export system in NETWORK mode
	 * Uses UDP for frame distribution and TCP for session management
	 *
	 * @param port Port to bind for network services (default: 8000)
	 * @param bind_addr Address to bind to (default: "0.0.0.0" - all interfaces)
	 * @return true if initialization succeeded
	 */
	bool initializeNetwork(int port = 8000, const std::string& bind_addr = "0.0.0.0");

	/**
	 * Legacy initialize method - defaults to local mode
	 * Deprecated: Use initializeLocal() or initializeNetwork() explicitly
	 */
	bool initialize(const std::string& shared_memory_name = "tracking_viewport") {
		return initializeLocal(shared_memory_name);
	}

	/**
	 * Shutdown the tracking export system
	 */
	void shutdown();

	/**
	 * Check if tracking export is active
	 */
	bool isActive() const;

	/**
	 * Check if network mode is enabled
	 * @return true if using network distribution, false if using shared memory
	 */
	bool isNetworkMode() const;

	/**
	 * Export the current framebuffer (to shared memory OR network, depending on mode)
	 * Called after driver->endScene()
	 *
	 * @param driver Irrlicht video driver
	 */
	void exportFramebuffer(video::IVideoDriver* driver);

	/**
	 * Export current player state to shared memory
	 * Called each frame in game loop
	 *
	 * @param player Local player instance
	 * @param client Client instance for additional state
	 */
	void exportPlayerState(const LocalPlayer* player, const Client* client);

	/**
	 * Get agent actions from shared memory
	 * Modifies player control inputs based on RL agent commands
	 * Called before input processing
	 *
	 * @param player Local player instance to modify
	 * @return true if agent actions were applied
	 */
	bool getAgentActions(LocalPlayer* player);

	/**
	 * Register a reward event from game logic
	 * Used for RL training
	 *
	 * @param event_type Type of event ("damage", "target_acquired", etc.)
	 * @param reward_value Reward value (positive or negative)
	 */
	void registerRewardEvent(const std::string& event_type, float reward_value);

private:
	struct Impl;
	Impl* m_impl = nullptr;

	// Frame capture buffer
	u32 m_frame_width = 0;
	u32 m_frame_height = 0;
	u8* m_pixel_buffer = nullptr;

	// Statistics
	u64 m_frames_exported = 0;
	u64 m_frames_failed = 0;
};

} // namespace tracking

