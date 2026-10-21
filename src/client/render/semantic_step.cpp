// Luanti Semantic Render Step
// GPU-based semantic segmentation via second render pass
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "semantic_step.h"
#include "client/client.h"
#include "client/clientmap.h"
#include "client/camera.h"
#include "log.h"
#include <IVideoDriver.h>
#include <ICameraSceneNode.h>
#include <ISceneManager.h>
#include <IImage.h>
#include <chrono>

DrawSemanticMap *DrawSemanticMap::s_instance = nullptr;

DrawSemanticMap::DrawSemanticMap(Client *client, TextureBuffer *buffer)
	: m_client(client), m_buffer(buffer)
{
	s_instance = this;
}

void DrawSemanticMap::run(PipelineContext &context)
{
	if (m_target)
		m_target->activate(context);

	auto *driver = context.device->getVideoDriver();

	// Set camera transforms -- reuse from the active camera (already set by Draw3D)
	auto *cam = context.device->getSceneManager()->getActiveCamera();
	if (cam) {
		driver->setTransform(video::ETS_VIEW, cam->getViewMatrix());
		driver->setTransform(video::ETS_PROJECTION, cam->getProjectionMatrix());
	}
	driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);

	// Render semantic meshes
	context.client->getEnv().getClientMap().renderSemanticMap(driver);

	// Read back the FBO
	readbackFBO(context);

	// Restore the screen framebuffer so subsequent steps (DrawHUD, screenshot)
	// render to/read from the screen, not the semantic FBO
	driver->setRenderTarget(nullptr, false, false);
}

void DrawSemanticMap::readbackFBO(PipelineContext &context)
{
	if (!m_buffer)
		return;

	video::ITexture *tex = m_buffer->getTexture(0);
	if (!tex)
		return;

	auto *driver = context.device->getVideoDriver();
	auto size = tex->getSize();

	video::IImage *img = driver->createImage(tex,
		core::position2di(0, 0), core::dimension2du(size.Width, size.Height));
	if (!img)
		return;

	m_width = size.Width;
	m_height = size.Height;
	m_pixels.resize(m_width * m_height * 3);

	// Fast readback: get raw data pointer and convert format
	// IImage format is typically ECF_A8R8G8B8 (BGRA in memory on little-endian)
	const u8 *src = (const u8 *)img->getData();
	auto fmt = img->getColorFormat();

	if (fmt == video::ECF_A8R8G8B8) {
		// BGRA layout in memory
		for (u32 i = 0; i < m_width * m_height; i++) {
			m_pixels[i * 3 + 0] = src[i * 4 + 2]; // R
			m_pixels[i * 3 + 1] = src[i * 4 + 1]; // G
			m_pixels[i * 3 + 2] = src[i * 4 + 0]; // B
		}
	} else {
		// Fallback: per-pixel (slow but correct for any format)
		for (u32 y = 0; y < m_height; y++) {
			for (u32 x = 0; x < m_width; x++) {
				video::SColor c = img->getPixel(x, y);
				u32 idx = (y * m_width + x) * 3;
				m_pixels[idx + 0] = c.getRed();
				m_pixels[idx + 1] = c.getGreen();
				m_pixels[idx + 2] = c.getBlue();
			}
		}
	}

	img->drop();
}
