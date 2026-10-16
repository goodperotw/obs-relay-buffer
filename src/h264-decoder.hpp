#pragma once

#include "h264-ring-buffer.hpp"

#include <cstdint>
#include <vector>

// ---------- Task 3.3b / 3.3c: stateless one-shot API (kept for regression) ----------

struct DecodedFrameInfo {
	bool valid = false;
	int width = 0;
	int height = 0;
	int format = -1;     // AVPixelFormat
	int64_t pts = -1;
	int64_t dts = -1;
	int decoded_index = -1;
};

struct DecodedRgbFrame {
	bool valid = false;
	int width = 0;
	int height = 0;
	int stride = 0;
	int64_t pts = -1;
	std::vector<uint8_t> rgb;
};

// ---------- Task 3.4a: packed YUV420P frame ----------
//
// Layout:
//   Y: width * height                 (stride == width)
//   U: (width/2) * (height/2)         (stride == width/2)
//   V: (width/2) * (height/2)         (stride == width/2)
// All planes are contiguous inside 'planes'.
struct YuvFrame {
	int64_t pts = 0;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> planes;
};

// ---------- Decoder ----------

class H264Decoder {
public:
	H264Decoder() = default;
	~H264Decoder();

	// Stateless one-shot (3.3b/3.3c). Each call opens & closes its own context.
	DecodedFrameInfo decode_first_frame(const std::vector<CachedH264Packet> &packets,
					    int max_packets_to_feed = 10) const;
	DecodedRgbFrame decode_first_frame_rgb(const std::vector<CachedH264Packet> &packets,
					       int max_packets_to_feed = 10) const;

	// ---------- Task 3.4a: stateful / streaming API ----------
	//
	// Open a persistent decoder. If target_w/target_h > 0, every decoded
	// frame is scaled to that size (YUV420P) via libswscale. If 0, frames
	// come out at source size.
	//
	// Returns false on failure. Safe to call when already closed.
	bool open(int target_w = 0, int target_h = 0);

	// Close and release everything. Safe to call multiple times.
	void close();

	// Feed one packet. Any newly decoded frames (already scaled if a target
	// size was given) are appended to 'out'. Returns false only on fatal
	// decoder errors; recoverable send errors are swallowed.
	bool decode_packet(const CachedH264Packet &pkt, std::vector<YuvFrame> &out);

	// Flush the decoder (send a null packet). Appends remaining frames.
	void flush(std::vector<YuvFrame> &out);

	// Source dimensions, valid after the first decoded frame.
	int sourceWidth() const { return m_src_w; }
	int sourceHeight() const { return m_src_h; }

private:
	// Internal helper: after avcodec_receive_frame succeeds, scale + pack.
	void emitFrame(std::vector<YuvFrame> &out);

	// State
	struct AVCodecContext *m_ctx = nullptr;
	struct AVPacket *m_pkt = nullptr;
	struct AVFrame *m_frame = nullptr;
	struct SwsContext *m_sws = nullptr;

	int m_target_w = 0;
	int m_target_h = 0;

	int m_src_w = 0;
	int m_src_h = 0;
	int m_src_fmt = -1;

	// Cache keys so we know when to rebuild m_sws.
	int m_sws_src_fmt = -1;
	int m_sws_dst_w = 0;
	int m_sws_dst_h = 0;
};
