// Luanti Semantic Segmenter
// Per-pixel semantic segmentation via raycast grid
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "mapnode.h"
#include <vector>
#include <string>

class Camera;
class ClientEnvironment;
class NodeDefManager;
class ClientActiveObject;

namespace tracking {

/**
 * SemanticSegmenter - Produces per-pixel semantic labels via raycasting
 *
 * After each frame render, casts a grid of rays from the camera into the
 * voxel world. For each ray hit, records the content_t (node type ID) of
 * the first non-air node. Produces both raw content_t grid and RGB color
 * visualization using ContentFeatures::minimap_color.
 */
class SemanticSegmenter {
public:
	SemanticSegmenter();

	/**
	 * Initialize the segmenter
	 * @param grid_size Resolution of the segmentation grid (e.g., 128 for 128x128)
	 * @param max_ray_distance Maximum raycast distance in nodes
	 */
	void initialize(u32 grid_size = 128, float max_ray_distance = 100.0f);

	bool isActive() const { return m_active; }

	/**
	 * Capture semantic segmentation for the current frame
	 * @param camera Camera for position/direction/FOV
	 * @param env Client environment for map access
	 * @param ndef Node definition manager for content features
	 */
	void capture(Camera *camera, ClientEnvironment *env, const NodeDefManager *ndef);

	/**
	 * Get the raw content_t grid (grid_size x grid_size)
	 * Values 0-0xDFFF are node content_t IDs.
	 * Values 0xE000+ are entity type IDs (0xE000 + ActiveObjectType).
	 */
	const std::vector<content_t>& getContentGrid() const { return m_content_grid; }

	/**
	 * Get the RGB color grid (grid_size x grid_size x 3)
	 */
	const std::vector<u8>& getColorGrid() const { return m_color_grid; }

	/**
	 * Get the depth grid (grid_size x grid_size)
	 * Distance from camera to hit surface in node units.
	 * Sky/miss pixels contain m_max_ray_distance.
	 */
	const std::vector<float>& getDepthGrid() const { return m_depth_grid; }

	/**
	 * Get the instance ID grid (grid_size x grid_size)
	 * Spatially-stable hash for voxels (same block = same ID across frames).
	 * Entity object IDs for active objects. 0 = sky/miss.
	 */
	const std::vector<u32>& getInstanceGrid() const { return m_instance_grid; }

	u32 getGridSize() const { return m_grid_size; }

	/**
	 * Build content_t -> {name, color} mapping as JSON string
	 * Includes both node types and entity type entries.
	 */
	std::string buildContentMapJSON(const NodeDefManager *ndef) const;

private:
	void overlayEntities(Camera *camera, ClientEnvironment *env, const NodeDefManager *ndef);

	bool m_active = false;
	u32 m_grid_size = 128;
	float m_max_ray_distance = 100.0f;
	u64 m_capture_count = 0;

	// Output buffers
	std::vector<content_t> m_content_grid;  // grid_size * grid_size
	std::vector<u8> m_color_grid;           // grid_size * grid_size * 3 (RGB)
	std::vector<float> m_depth_grid;        // grid_size * grid_size (node units)
	std::vector<u32> m_instance_grid;       // grid_size * grid_size
};

} // namespace tracking
