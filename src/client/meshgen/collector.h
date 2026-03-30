// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2018 numzero, Lobachevskiy Vitaliy <numzer0@yandex.ru>

#pragma once
#include <array>
#include <vector>
#include "irrlichttypes.h"
#include "irr_v3d.h"
#include "mapnode.h"
#include <S3DVertex.h>
#include "client/tile.h"

struct PreMeshBuffer
{
	TileLayer layer;
	std::vector<u16> indices;
	std::vector<video::S3DVertex> vertices;
	std::vector<content_t> vertex_content_ids; // parallel to vertices: node type per vertex

	PreMeshBuffer() = default;
	explicit PreMeshBuffer(const TileLayer &layer) : layer(layer) {}

	/// @brief Colorizes vertices as indicated by tile layer
	void applyTileColor()
	{
		video::SColor tc = layer.color;
		if (tc == video::SColor(0xFFFFFFFF))
			return;
		for (auto &vertex : vertices) {
			video::SColor *c = &vertex.Color;
			c->set(c->getAlpha(),
				c->getRed() * tc.getRed() / 255U,
				c->getGreen() * tc.getGreen() / 255U,
				c->getBlue() * tc.getBlue() / 255U);
		}
	}
};

struct MeshCollector
{
	std::array<std::vector<PreMeshBuffer>, MAX_TILE_LAYERS> prebuffers;
	// bounding sphere radius and center
	f32 m_bounding_radius_sq = 0.0f;
	v3f m_center_pos;
	v3f offset;

	// center_pos: pos to use for bounding-sphere, in BS-space
	// offset: offset added to vertices
	MeshCollector(const v3f center_pos, v3f offset = v3f()) : m_center_pos(center_pos), offset(offset) {}

	// Append vertices without content_t tracking (backward-compatible)
	void append(const TileSpec &material,
			const video::S3DVertex *vertices, u32 numVertices,
			const u16 *indices, u32 numIndices);

	// Append vertices with content_t tracking for semantic segmentation
	void append(const TileSpec &material,
			const video::S3DVertex *vertices, u32 numVertices,
			const u16 *indices, u32 numIndices,
			content_t content_id);

private:
	void append(const TileLayer &material,
			const video::S3DVertex *vertices, u32 numVertices,
			const u16 *indices, u32 numIndices,
			u8 layernum, content_t content_id = CONTENT_IGNORE);

	PreMeshBuffer &findBuffer(const TileLayer &layer, u8 layernum, u32 numVertices);
};
