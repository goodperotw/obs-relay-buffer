#pragma once

#include <obs.h>
#include <mutex>
#include <deque>
#include <vector>
#include <cstdint>
#include <cstddef>

// store deep copied data
struct CachedH264Packet {
	int64_t pts;
	int64_t dts;
	uint32_t size;
	uint32_t type;
	uint32_t track_idx;
	int64_t timebase_num;
	int64_t timebase_den;
	bool keyframe;             // NEW: IDR / keyframe flag
	std::vector<uint8_t> data; // deep copied h264 payload
};

// NEW: lightweight stats for UI / replay readiness (no packet data copy)
struct BufferStats {
	size_t packet_count = 0;
	double duration_sec = 0.0;
	uint64_t total_bytes = 0;
	size_t keyframe_count = 0;
	int64_t first_keyframe_pts = -1;
	int64_t last_keyframe_pts = -1;
	bool has_decodable_start = false;
};

class H264RingBuffer {
public:
	H264RingBuffer();
	~H264RingBuffer();

	// push new packet into buffer, and clear data exceed 60s window
	void push(const encoder_packet *packet);

	// get all packets in buffer (for replay)
	std::vector<CachedH264Packet> get_snapshot() const;

	// NEW: cheap stats query, does not copy packet data
	BufferStats get_stats() const;

	// clear buffer
	void clear();

	// check if there're enough data in buffer (at least 1 IDR)
	bool is_ready() const;

private:
	std::deque<CachedH264Packet> buffer_;
	mutable std::mutex mutex_;

	int64_t first_pts_ = -1;
	int64_t max_duration_ms_ = 60000; // 60s
};
