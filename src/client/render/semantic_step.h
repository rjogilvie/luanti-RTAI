// Luanti Semantic Render Step
// GPU-based semantic segmentation via second render pass
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "pipeline.h"
#include <vector>

class Client;

/**
 * DrawSemanticMap - Render step that produces full-resolution semantic segmentation
 *
 * Re-renders the scene using semantic meshes (flat content_t-encoded vertex colors)
 * to an offscreen FBO, then reads back the pixels. Each pixel encodes content_t as:
 *   R = (content_t >> 8) & 0xFF
 *   G = content_t & 0xFF
 *   B = 0
 * Decode: content_t = (R << 8) | G
 */
class DrawSemanticMap : public RenderStep
{
public:
	DrawSemanticMap(Client *client, TextureBuffer *buffer);

	void setRenderSource(RenderSource *) override {}
	void setRenderTarget(RenderTarget *target) override { m_target = target; }
	void reset(PipelineContext &context) override {}
	void run(PipelineContext &context) override;

	/// Get the RGB readback buffer (content_t encoded in R,G channels)
	const u8 *getPixels() const { return m_pixels.data(); }
	u32 getWidth() const { return m_width; }
	u32 getHeight() const { return m_height; }
	bool hasData() const { return !m_pixels.empty(); }

	/// Global accessor (only one instance exists at a time)
	static DrawSemanticMap *getInstance() { return s_instance; }

private:
	void readbackFBO(PipelineContext &context);

	Client *m_client;
	TextureBuffer *m_buffer;
	RenderTarget *m_target = nullptr;

	std::vector<u8> m_pixels; // RGB readback
	u32 m_width = 0;
	u32 m_height = 0;

	static DrawSemanticMap *s_instance;
};
