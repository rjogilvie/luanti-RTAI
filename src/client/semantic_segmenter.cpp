// Luanti Semantic Segmenter
// Per-pixel semantic segmentation via raycast grid
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "semantic_segmenter.h"
#include "camera.h"
#include "clientenvironment.h"
#include "client.h"
#include "nodedef.h"
#include "map.h"
#include "voxelalgorithms.h"
#include "constants.h"
#include "log.h"
#include "clientobject.h"
#include "content_cao.h"
#include "activeobject.h"

#include <ICameraSceneNode.h>
#include <cmath>
#include <chrono>
#include <algorithm>

// Entity content_t IDs start at this offset
static constexpr content_t ENTITY_ID_BASE = 0xE000;

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
	m_depth_grid.resize(grid_size * grid_size, max_ray_distance);
	m_instance_grid.resize(grid_size * grid_size, 0);

	m_active = true;
	infostream << "[SemanticSeg] Initialized (" << grid_size << "x" << grid_size
	           << ", max_dist=" << max_ray_distance << " nodes)" << std::endl;
}

void SemanticSegmenter::capture(Camera *camera, ClientEnvironment *env, const NodeDefManager *ndef)
{
	if (!m_active || !camera || !env || !ndef)
		return;

	auto t0 = std::chrono::high_resolution_clock::now();

	Map &map = env->getMap();

	v3s16 cam_offset = camera->getOffset();

	scene::ICameraSceneNode *cam_node = camera->getCameraNode();
	if (!cam_node)
		return;

	v3f cam_pos = cam_node->getAbsolutePosition();

	// Extract camera axes from the view matrix
	core::matrix4 view = cam_node->getViewMatrix();
	const f32 *V = view.pointer();
	v3f right(V[0], V[4], V[8]);
	v3f up(V[1], V[5], V[9]);
	v3f forward(V[2], V[6], V[10]);

	f32 fov_x = camera->getFovX();
	f32 fov_y = camera->getFovY();
	float half_tan_x = std::tan(fov_x * 0.5f);
	float half_tan_y = std::tan(fov_y * 0.5f);

	v3f start_nodes = cam_pos / BS;

	// Cast rays through each pixel of the grid
	for (u32 gy = 0; gy < m_grid_size; gy++) {
		for (u32 gx = 0; gx < m_grid_size; gx++) {
			float norm_x = ((gx + 0.5f) / m_grid_size) * 2.0f - 1.0f;
			float norm_y = 1.0f - ((gy + 0.5f) / m_grid_size) * 2.0f;

			v3f ray_dir = forward
				+ right * (norm_x * half_tan_x)
				+ up * (norm_y * half_tan_y);
			ray_dir.normalize();

			v3f ray_vec = ray_dir * (m_max_ray_distance * BS);

			v3f line_vec_nodes = ray_vec / BS;
			voxalgo::VoxelLineIterator it(start_nodes, line_vec_nodes);

			content_t hit_content = CONTENT_AIR;
			float hit_depth = m_max_ray_distance;
			u32 hit_instance = 0;
			u32 grid_idx = gy * m_grid_size + gx;

			while (it.hasNext()) {
				v3s16 node_pos = it.m_current_node_pos + cam_offset;

				bool valid = false;
				MapNode node = map.getNode(node_pos, &valid);

				if (valid) {
					content_t c = node.getContent();
					if (c != CONTENT_AIR && c != CONTENT_IGNORE) {
						const ContentFeatures &f = ndef->get(c);
						if (f.drawtype != NDT_AIRLIKE) {
							hit_content = c;
							// Depth: distance from camera to hit node center
							v3f hit_pos(it.m_current_node_pos.X + 0.5f,
							            it.m_current_node_pos.Y + 0.5f,
							            it.m_current_node_pos.Z + 0.5f);
							hit_depth = hit_pos.getDistanceFrom(start_nodes);
							// Instance ID: spatial hash of world position
							hit_instance = static_cast<u32>(
								(node_pos.X * 73856093u) ^
								(node_pos.Y * 19349663u) ^
								(node_pos.Z * 83492791u));
							break;
						}
					}
				}

				it.next();
			}

			m_content_grid[grid_idx] = hit_content;
			m_depth_grid[grid_idx] = hit_depth;
			m_instance_grid[grid_idx] = hit_instance;

			// Map to color
			u32 color_idx = grid_idx * 3;
			if (hit_content != CONTENT_AIR && hit_content != CONTENT_IGNORE) {
				const ContentFeatures &f = ndef->get(hit_content);
#if CHECK_CLIENT_BUILD()
				video::SColor c = f.minimap_color;
				m_color_grid[color_idx + 0] = c.getRed();
				m_color_grid[color_idx + 1] = c.getGreen();
				m_color_grid[color_idx + 2] = c.getBlue();
#else
				m_color_grid[color_idx + 0] = (hit_content * 37) & 0xFF;
				m_color_grid[color_idx + 1] = (hit_content * 73) & 0xFF;
				m_color_grid[color_idx + 2] = (hit_content * 127) & 0xFF;
#endif
			} else {
				m_color_grid[color_idx + 0] = 0;
				m_color_grid[color_idx + 1] = 0;
				m_color_grid[color_idx + 2] = 0;
			}
		}
	}

	// Overlay entities on top of voxel segmentation
	overlayEntities(camera, env, ndef);

	auto t1 = std::chrono::high_resolution_clock::now();
	float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
	if (m_capture_count % 60 == 0) {
		infostream << "[SemanticSeg] " << m_grid_size << "x" << m_grid_size
		           << " capture: " << ms << "ms" << std::endl;
	}
	m_capture_count++;
}

void SemanticSegmenter::overlayEntities(Camera *camera, ClientEnvironment *env,
                                         const NodeDefManager *ndef)
{
	scene::ICameraSceneNode *cam_node = camera->getCameraNode();
	if (!cam_node)
		return;

	v3s16 cam_offset = camera->getOffset();
	v3f cam_pos_world = cam_node->getAbsolutePosition() + intToFloat(cam_offset, BS);

	// Get view-projection matrix for screen-space projection
	core::matrix4 proj = cam_node->getProjectionMatrix();
	core::matrix4 view = cam_node->getViewMatrix();

	// Get all active objects within render distance
	std::vector<DistanceSortedActiveObject> objects;
	env->getActiveObjects(cam_pos_world, m_max_ray_distance * BS, objects);

	for (const auto &sorted_obj : objects) {
		ClientActiveObject *obj = sorted_obj.obj;
		if (!obj)
			continue;

		aabb3f sel_box(v3f(0,0,0), v3f(0,0,0));
		if (!obj->getSelectionBox(&sel_box))
			continue;

		// Get object world position
		v3f obj_pos = obj->getPosition();

		// Transform bbox corners to camera-offset space
		v3f box_min = obj_pos + sel_box.MinEdge - intToFloat(cam_offset, BS);
		v3f box_max = obj_pos + sel_box.MaxEdge - intToFloat(cam_offset, BS);

		// Project all 8 corners to find screen-space AABB
		float screen_min_x = 1.0f, screen_max_x = -1.0f;
		float screen_min_y = 1.0f, screen_max_y = -1.0f;
		bool any_in_front = false;
		float min_depth = m_max_ray_distance;

		for (int corner = 0; corner < 8; corner++) {
			v3f p(
				(corner & 1) ? box_max.X : box_min.X,
				(corner & 2) ? box_max.Y : box_min.Y,
				(corner & 4) ? box_max.Z : box_min.Z
			);

			// Transform through view then projection
			f32 clip[4] = { p.X, p.Y, p.Z, 1.0f };
			view.multiplyWith1x4Matrix(clip);
			proj.multiplyWith1x4Matrix(clip);

			if (clip[3] <= 0.0f)
				continue; // behind camera

			any_in_front = true;
			float inv_w = 1.0f / clip[3];
			float ndc_x = clip[0] * inv_w;
			float ndc_y = clip[1] * inv_w;

			screen_min_x = std::min(screen_min_x, ndc_x);
			screen_max_x = std::max(screen_max_x, ndc_x);
			screen_min_y = std::min(screen_min_y, ndc_y);
			screen_max_y = std::max(screen_max_y, ndc_y);

			// Depth in node units
			float depth = p.getDistanceFrom(cam_node->getAbsolutePosition()) / BS;
			min_depth = std::min(min_depth, depth);
		}

		if (!any_in_front)
			continue;

		// Convert NDC to grid coordinates
		// NDC x: -1=left, +1=right -> grid x: 0..grid_size-1
		// NDC y: -1=bottom, +1=top -> grid y: 0=top, grid_size-1=bottom (flipped)
		int gx_min = std::max(0, (int)((screen_min_x * 0.5f + 0.5f) * m_grid_size));
		int gx_max = std::min((int)m_grid_size - 1, (int)((screen_max_x * 0.5f + 0.5f) * m_grid_size));
		int gy_min = std::max(0, (int)((0.5f - screen_max_y * 0.5f) * m_grid_size));
		int gy_max = std::min((int)m_grid_size - 1, (int)((0.5f - screen_min_y * 0.5f) * m_grid_size));

		if (gx_min > gx_max || gy_min > gy_max)
			continue;

		// Entity type ID
		content_t entity_content = ENTITY_ID_BASE + static_cast<content_t>(obj->getType());
		u32 entity_instance = static_cast<u32>(obj->getId());

		// Entity color by type
		u8 er, eg, eb;
		switch (obj->getType()) {
		case ACTIVEOBJECT_TYPE_PLAYER:
			er = 255; eg = 50; eb = 50; break;   // red
		case ACTIVEOBJECT_TYPE_LUAENTITY:
			er = 50; eg = 200; eb = 255; break;   // cyan
		default:
			er = 255; eg = 255; eb = 50; break;   // yellow
		}

		// Fill grid cells where entity is closer than voxel
		for (int gy = gy_min; gy <= gy_max; gy++) {
			for (int gx = gx_min; gx <= gx_max; gx++) {
				u32 idx = gy * m_grid_size + gx;
				if (min_depth < m_depth_grid[idx]) {
					m_content_grid[idx] = entity_content;
					m_depth_grid[idx] = min_depth;
					m_instance_grid[idx] = entity_instance;
					u32 ci = idx * 3;
					m_color_grid[ci + 0] = er;
					m_color_grid[ci + 1] = eg;
					m_color_grid[ci + 2] = eb;
				}
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

	// Node content types
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

	// Entity type entries
	struct EntityEntry { content_t id; const char *name; u8 r, g, b; };
	EntityEntry entities[] = {
		{ENTITY_ID_BASE + ACTIVEOBJECT_TYPE_PLAYER, "entity:player", 255, 50, 50},
		{ENTITY_ID_BASE + ACTIVEOBJECT_TYPE_LUAENTITY, "entity:lua_entity", 50, 200, 255},
		{ENTITY_ID_BASE + ACTIVEOBJECT_TYPE_GENERIC, "entity:generic", 255, 255, 50},
	};
	for (const auto &e : entities) {
		json += ",\n  \"" + std::to_string(e.id) + "\": {"
			"\"name\": \"" + e.name + "\", "
			"\"color\": [" + std::to_string(e.r) + ", " +
			std::to_string(e.g) + ", " + std::to_string(e.b) + "]}";
	}

	json += "\n}\n";
	return json;
}

} // namespace tracking
