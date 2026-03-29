// Luanti Frame Dumper
// Saves RGB frames, semantic masks, and metadata to disk
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "frame_dumper.h"
#include "log.h"
#include "filesys.h"

#include <sstream>
#include <iomanip>
#include <chrono>

namespace tracking {

FrameDumper::FrameDumper()
{
}

FrameDumper::~FrameDumper()
{
	if (m_metadata_file.is_open())
		m_metadata_file.close();
}

bool FrameDumper::initialize(const std::string &output_dir, u32 frame_skip, u64 max_frames)
{
	m_output_dir = output_dir;
	m_frame_skip = std::max(1u, frame_skip);
	m_max_frames = max_frames;

	// Create directory structure
	bool ok = true;
	ok = ok && fs::CreateDir(output_dir);
	ok = ok && fs::CreateDir(output_dir + "/rgb");
	ok = ok && fs::CreateDir(output_dir + "/semantic_color");
	ok = ok && fs::CreateDir(output_dir + "/semantic_raw");

	if (!ok) {
		errorstream << "[FrameDumper] Failed to create output directories at: "
		            << output_dir << std::endl;
		return false;
	}

	// Open metadata file
	m_metadata_file.open(output_dir + "/metadata.jsonl", std::ios::out | std::ios::trunc);
	if (!m_metadata_file.is_open()) {
		errorstream << "[FrameDumper] Failed to open metadata file" << std::endl;
		return false;
	}

	m_active = true;
	infostream << "[FrameDumper] Initialized (output=" << output_dir
	           << ", skip=" << frame_skip
	           << ", max=" << (max_frames > 0 ? std::to_string(max_frames) : "unlimited")
	           << ")" << std::endl;
	return true;
}

bool FrameDumper::shouldDumpFrame(u64 frame_num) const
{
	if (!m_active || isFinished())
		return false;
	return (frame_num % m_frame_skip) == 0;
}

std::string FrameDumper::formatFrameNum(u64 frame_num) const
{
	std::ostringstream ss;
	ss << std::setw(6) << std::setfill('0') << frame_num;
	return ss.str();
}

void FrameDumper::writePPM(const std::string &path, const u8 *rgb, u32 width, u32 height)
{
	std::ofstream file(path, std::ios::out | std::ios::binary);
	if (!file.is_open()) {
		errorstream << "[FrameDumper] Failed to write: " << path << std::endl;
		return;
	}

	// PPM header (P6 = binary RGB)
	file << "P6\n" << width << " " << height << "\n255\n";
	file.write(reinterpret_cast<const char*>(rgb), width * height * 3);
	file.close();
}

void FrameDumper::dumpRGBFrame(const u8 *pixels, u32 width, u32 height, u64 frame_num)
{
	if (!m_active || !pixels)
		return;

	std::string path = m_output_dir + "/rgb/frame_" + formatFrameNum(frame_num) + ".ppm";
	writePPM(path, pixels, width, height);
}

void FrameDumper::dumpSemanticColor(const u8 *rgb_pixels, u32 width, u32 height, u64 frame_num)
{
	if (!m_active || !rgb_pixels)
		return;

	std::string path = m_output_dir + "/semantic_color/frame_" + formatFrameNum(frame_num) + ".ppm";
	writePPM(path, rgb_pixels, width, height);
}

void FrameDumper::dumpSemanticRaw(const content_t *content_ids, u32 width, u32 height, u64 frame_num)
{
	if (!m_active || !content_ids)
		return;

	std::string path = m_output_dir + "/semantic_raw/frame_" + formatFrameNum(frame_num) + ".bin";
	std::ofstream file(path, std::ios::out | std::ios::binary);
	if (!file.is_open()) {
		errorstream << "[FrameDumper] Failed to write: " << path << std::endl;
		return;
	}

	// Write grid dimensions as header (2x uint32)
	file.write(reinterpret_cast<const char*>(&width), sizeof(u32));
	file.write(reinterpret_cast<const char*>(&height), sizeof(u32));
	// Write content_t array
	file.write(reinterpret_cast<const char*>(content_ids), width * height * sizeof(content_t));
	file.close();
}

void FrameDumper::writeFrameMetadata(u64 frame_num, u64 timestamp_us,
                                      float pos_x, float pos_y, float pos_z,
                                      float yaw, float pitch)
{
	if (!m_active || !m_metadata_file.is_open())
		return;

	m_metadata_file << "{\"frame\": " << frame_num
	                << ", \"timestamp_us\": " << timestamp_us
	                << ", \"pos\": [" << pos_x << ", " << pos_y << ", " << pos_z << "]"
	                << ", \"yaw\": " << yaw
	                << ", \"pitch\": " << pitch
	                << "}\n";

	// Flush periodically to avoid data loss
	m_frames_dumped++;
	if (m_frames_dumped % 30 == 0) {
		m_metadata_file.flush();
	}

	// Log progress
	if (m_frames_dumped % 300 == 0) {
		infostream << "[FrameDumper] Dumped " << m_frames_dumped << " frames" << std::endl;
	}

	// Check if we've reached the limit
	if (m_max_frames > 0 && m_frames_dumped >= m_max_frames) {
		infostream << "[FrameDumper] Reached max frames (" << m_max_frames
		           << "), stopping capture" << std::endl;
		m_metadata_file.flush();
	}
}

void FrameDumper::writeContentMap(const std::string &json_content)
{
	if (!m_active || m_content_map_written)
		return;

	std::string path = m_output_dir + "/content_map.json";
	std::ofstream file(path, std::ios::out);
	if (!file.is_open()) {
		errorstream << "[FrameDumper] Failed to write content map: " << path << std::endl;
		return;
	}

	file << json_content;
	file.close();
	m_content_map_written = true;
	infostream << "[FrameDumper] Wrote content map to: " << path << std::endl;
}

} // namespace tracking
