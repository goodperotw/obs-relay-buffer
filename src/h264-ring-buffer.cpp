#include "h264-ring-buffer.hpp"

#include <obs-module.h>

H264RingBuffer::H264RingBuffer() = default;

H264RingBuffer::~H264RingBuffer()
{
	clear();
}

void H264RingBuffer::push(const encoder_packet *packet)
{
	if (!packet || !packet->data || packet->size == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);

	// 1. deep copy packet data
	CachedH264Packet cached;
	cached.pts = packet->pts;
	cached.dts = packet->dts;
	cached.size = static_cast<uint32_t>(packet->size);
	cached.type = packet->type;
	cached.track_idx = static_cast<uint32_t>(packet->track_idx);
	cached.timebase_num = packet->timebase_num;
	cached.timebase_den = packet->timebase_den;
	cached.keyframe = packet->keyframe; // NEW
	cached.data.assign(packet->data, packet->data + packet->size);

	// 2. record first pts as base
	if (first_pts_ == -1) {
		first_pts_ = cached.pts;
	}

	// 3. push to buffer
	buffer_.push_back(std::move(cached));

	// 4. clear data which exceed 60s
	//    calculate distance between first pts and last pts (in ms)
	while (!buffer_.empty()) {
		const auto &front = buffer_.front();
		int64_t pts_diff = cached.pts - front.pts;

		if (pts_diff < 0) {
			pts_diff = 0;
		}

		int64_t duration_ms =
			(pts_diff * cached.timebase_num * 1000) / cached.timebase_den;

		if (duration_ms > max_duration_ms_) {
			buffer_.pop_front();
			if (!buffer_.empty()) {
				first_pts_ = buffer_.front().pts;
			} else {
				first_pts_ = -1;
			}
		} else {
			break; // already fit 60s window, stop clearing
		}
	}
}

std::vector<CachedH264Packet> H264RingBuffer::get_snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::vector<CachedH264Packet>(buffer_.begin(), buffer_.end());
}

// NEW: cheap stats query, no packet data copy
BufferStats H264RingBuffer::get_stats() const
{
	std::lock_guard<std::mutex> lock(mutex_);

	BufferStats stats;
	if (buffer_.empty()) {
		return stats;
	}

	stats.packet_count = buffer_.size();

	const auto &front = buffer_.front();
	const auto &back = buffer_.back();

	int64_t pts_diff = back.pts - front.pts;
	if (pts_diff < 0) {
		pts_diff = 0;
	}

	if (back.timebase_den > 0) {
		stats.duration_sec = (double)(pts_diff * back.timebase_num) /
				     (double)back.timebase_den;
	}

	for (const auto &pkt : buffer_) {
		stats.total_bytes += pkt.size;
		if (pkt.keyframe) {
			stats.keyframe_count++;
			if (stats.first_keyframe_pts < 0) {
				stats.first_keyframe_pts = pkt.pts;
			}
			stats.last_keyframe_pts = pkt.pts;
		}
	}

	stats.has_decodable_start = (stats.keyframe_count > 0);

	return stats;
}

void H264RingBuffer::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	buffer_.clear();
	first_pts_ = -1;
}

bool H264RingBuffer::is_ready() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return !buffer_.empty();
}
