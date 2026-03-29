// Luanti Frame Dumper
// Saves RGB frames, semantic masks, and metadata to disk
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "mapnode.h"
#include <string>
#include <fstream>
#include <vector>

namespace tracking {

/**
 * FrameDumper - Writes video frames, semantic masks, and metadata to disk
 *
 * Outputs:
 *   rgb/frame_NNNNNN.ppm          - Full-resolution RGB video frames
 *   semantic_color/frame_NNNNNN.ppm - Semantic mask as RGB color image
 *   semantic_raw/frame_NNNNNN.bin - Raw content_t IDs (uint16 binary)
 *   metadata.jsonl                - Per-frame metadata (position, orientation)
 *   content_map.json              - content_t -> {name, color} mapping
 */
class FrameDumper {
public:
	FrameDumper();
	~FrameDumper();

	/**
	 * Initialize the frame dumper
	 * @param output_dir Base directory for output files
	 * @param frame_skip Dump every Nth frame (1 = every frame)
	 * @param max_frames Stop after this many dumped frames (0 = unlimited)
	 * @return true if directories were created successfully
	 */
	bool initialize(const std::string &output_dir, u32 frame_skip = 1, u64 max_frames = 0);

	bool isActive() const { return m_active; }
	bool isFinished() const { return m_max_frames > 0 && m_frames_dumped >= m_max_frames; }
	u64 framesDumped() const { return m_frames_dumped; }

	/**
	 * Dump an RGB frame to disk as PPM
	 * @param pixels RGB pixel data (width * height * 3)
	 * @param width Frame width
	 * @param height Frame height
	 * @param frame_num Frame sequence number
	 */
	void dumpRGBFrame(const u8 *pixels, u32 width, u32 height, u64 frame_num);

	/**
	 * Dump semantic color mask to disk as PPM
	 * @param rgb_pixels RGB semantic color data
	 * @param width Grid width
	 * @param height Grid height
	 * @param frame_num Frame sequence number
	 */
	void dumpSemanticColor(const u8 *rgb_pixels, u32 width, u32 height, u64 frame_num);

	/**
	 * Dump raw semantic content IDs to disk as binary
	 * @param content_ids Raw content_t values
	 * @param width Grid width
	 * @param height Grid height
	 * @param frame_num Frame sequence number
	 */
	void dumpSemanticRaw(const content_t *content_ids, u32 width, u32 height, u64 frame_num);

	/**
	 * Write per-frame metadata as a JSONL line
	 */
	void writeFrameMetadata(u64 frame_num, u64 timestamp_us,
	                        float pos_x, float pos_y, float pos_z,
	                        float yaw, float pitch);

	/**
	 * Write the content_t -> {name, color} mapping JSON file
	 */
	void writeContentMap(const std::string &json_content);

	/**
	 * Check if this frame should be dumped (respects frame_skip)
	 */
	bool shouldDumpFrame(u64 frame_num) const;

private:
	void writePPM(const std::string &path, const u8 *rgb, u32 width, u32 height);
	std::string formatFrameNum(u64 frame_num) const;

	bool m_active = false;
	std::string m_output_dir;
	u32 m_frame_skip = 1;
	u64 m_max_frames = 0;
	u64 m_frames_dumped = 0;
	bool m_content_map_written = false;

	std::ofstream m_metadata_file;
};

} // namespace tracking
