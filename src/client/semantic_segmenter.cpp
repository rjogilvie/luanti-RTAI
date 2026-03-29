// Luanti Semantic Segmenter
// Per-pixel semantic segmentation via raycast grid
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "semantic_segmenter.h"
#include "camera.h"
#include "clientenvironment.h"
#include "nodedef.h"
#include "map.h"
#include "voxelalgorithms.h"
#include "constants.h"
#include "log.h"

#include <ICameraSceneNode.h>
#include <cmath>

namespace tracking {

SemanticSegmenter::SemanticSegmenter()
{
}

void SemanticSegmenter::initialize(u32 grid_size, float max_ray_distance)
{
	m_grid_size = grid_size;
	m_max_ray_distance = max_ray_distance;

	m_content_grid.resize(grid_size * grid_size, CONTENT_AIR);
	m_color_grid.resize(grid_size * grid_size * 3, 0);

	m_active = true;
	infostream << "[SemanticSeg] Initialized (" << grid_size << "x" << grid_size
	           << ", max_dist=" << max_ray_distance << " nodes)" << std::endl;
}

void SemanticSegmenter::capture(Camera *camera, ClientEnvironment *env, const NodeDefManager *ndef)
{
	if (!m_active || !camera || !env || !ndef)
		return;

	Map &map = env->getMap();

	// Get camera offset for converting back to world node positions
	v3s16 cam_offset = camera->getOffset();

	scene::ICameraSceneNode *cam_node = camera->getCameraNode();
	if (!cam_node)
		return;

	// Use the scene node position as ray origin - this is the exact position
	// the view matrix was built from, ensuring consistency
	v3f cam_pos = cam_node->getAbsolutePosition();

	// Extract camera axes directly from the view matrix
	// This is guaranteed to match what Irrlicht used for rendering.
	// Irrlicht LH view matrix (row-major): column 0=right, 1=up, 2=forward
	//   M[0]=right.X  M[1]=up.X  M[2]=fwd.X
	//   M[4]=right.Y  M[5]=up.Y  M[6]=fwd.Y
	//   M[8]=right.Z  M[9]=up.Z  M[10]=fwd.Z
	core::matrix4 view = cam_node->getViewMatrix();
	const f32 *V = view.pointer();
	v3f right(V[0], V[4], V[8]);
	v3f up(V[1], V[5], V[9]);
	v3f forward(V[2], V[6], V[10]);

	// Get FOV from camera
	f32 fov_y = camera->getFovY();
	f32 fov_x = camera->getFovX();
	float half_tan_x = std::tan(fov_x * 0.5f);
	float half_tan_y = std::tan(fov_y * 0.5f);

	// Cast rays through each pixel of the grid
	for (u32 gy = 0; gy < m_grid_size; gy++) {
		for (u32 gx = 0; gx < m_grid_size; gx++) {
			// Map grid position to normalized coordinates (-1 to 1)
			float norm_x = ((gx + 0.5f) / m_grid_size) * 2.0f - 1.0f;
			float norm_y = 1.0f - ((gy + 0.5f) / m_grid_size) * 2.0f;

			// Compute ray direction using FOV and camera axes from view matrix
			// In view space: (norm_x * half_tan_x, norm_y * half_tan_y, 1.0)
			// Transform to world space using the camera basis vectors
			v3f ray_dir = forward
				+ right * (norm_x * half_tan_x)
				+ up * (norm_y * half_tan_y);
			ray_dir.normalize();

			// Ray in camera-offset space, scaled to max distance in BS units
			v3f ray_start = cam_pos;
			v3f ray_vec = ray_dir * (m_max_ray_distance * BS);

			// Use VoxelLineIterator to walk along the ray
			// VoxelLineIterator expects positions in node coordinates (divided by BS)
			v3f start_node_pos = ray_start / BS;
			v3f line_vec_nodes = ray_vec / BS;

			voxalgo::VoxelLineIterator it(start_node_pos, line_vec_nodes);

			content_t hit_content = CONTENT_AIR;
			u32 grid_idx = gy * m_grid_size + gx;

			// Walk along the ray
			while (it.hasNext()) {
				v3s16 node_pos = it.m_current_node_pos + cam_offset;

				bool valid = false;
				MapNode node = map.getNode(node_pos, &valid);

				if (valid) {
					content_t c = node.getContent();
					if (c != CONTENT_AIR && c != CONTENT_IGNORE) {
						// Check if this node type is actually drawable/visible
						const ContentFeatures &f = ndef->get(c);
						if (f.drawtype != NDT_AIRLIKE) {
							hit_content = c;
							break;
						}
					}
				}

				it.next();
			}

			// Store content_t
			m_content_grid[grid_idx] = hit_content;

			// Map to color via minimap_color
			u32 color_idx = grid_idx * 3;
			if (hit_content != CONTENT_AIR && hit_content != CONTENT_IGNORE) {
				const ContentFeatures &f = ndef->get(hit_content);
#if CHECK_CLIENT_BUILD()
				video::SColor c = f.minimap_color;
				m_color_grid[color_idx + 0] = c.getRed();
				m_color_grid[color_idx + 1] = c.getGreen();
				m_color_grid[color_idx + 2] = c.getBlue();
#else
				// Fallback: hash the content_t to a color
				m_color_grid[color_idx + 0] = (hit_content * 37) & 0xFF;
				m_color_grid[color_idx + 1] = (hit_content * 73) & 0xFF;
				m_color_grid[color_idx + 2] = (hit_content * 127) & 0xFF;
#endif
			} else {
				// Sky / air = black
				m_color_grid[color_idx + 0] = 0;
				m_color_grid[color_idx + 1] = 0;
				m_color_grid[color_idx + 2] = 0;
			}
		}
	}
}

std::string SemanticSegmenter::buildContentMapJSON(const NodeDefManager *ndef) const
{
	if (!ndef)
		return "{}";

	std::string json = "{\n";
	bool first = true;

	// Iterate through all registered content types
	for (content_t c = 0; c < CONTENT_MAX; c++) {
		const ContentFeatures &f = ndef->get(c);
		if (f.name.empty() || f.name == "unknown" || f.name == "ignore")
			continue;

		if (!first)
			json += ",\n";
		first = false;

#if CHECK_CLIENT_BUILD()
		video::SColor color = f.minimap_color;
		json += "  \"" + std::to_string(c) + "\": {"
			"\"name\": \"" + f.name + "\", "
			"\"color\": [" +
			std::to_string(color.getRed()) + ", " +
			std::to_string(color.getGreen()) + ", " +
			std::to_string(color.getBlue()) + "]}";
#else
		json += "  \"" + std::to_string(c) + "\": {"
			"\"name\": \"" + f.name + "\", "
			"\"color\": [0, 0, 0]}";
#endif
	}

	json += "\n}\n";
	return json;
}

} // namespace tracking
