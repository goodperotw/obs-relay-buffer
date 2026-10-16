#include "playback-session.hpp"

#include <obs-module.h>
#include "plugin-support.h"

#include <chrono>
#include <algorithm>

// ============================================================================
// FrameStore
// ============================================================================

void FrameStore::addFrame(YuvFrame &&f)
{
	if (!m_frames.empty() && f.pts < m_frames.back().pts) {
		obs_log(LOG_WARNING, "[T3.4a] FrameStore: out-of-order pts (%lld < %lld)",
			(long long)f.pts, (long long)m_frames.back().pts);
	}
	m_frames.push_back(std::move(f));
}

const YuvFrame *FrameStore::at(size_t index) const
{
	if (index >= m_frames.size()) return nullptr;
	return &m_frames[index];
}

const YuvFrame *FrameStore::findByPts(int64_t pts) const
{
	if (m_frames.empty()) return nullptr;

	size_t lo = 0, hi = m_frames.size();
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (m_frames[mid].pts <= pts) lo = mid + 1;
		else hi = mid;
	}
	if (lo == 0) return &m_frames.front();
	return &m_frames[lo - 1];
}

int64_t FrameStore::firstPts() const
{
	return m_frames.empty() ? -1 : m_frames.front().pts;
}

int64_t FrameStore::lastPts() const
{
	return m_frames.empty() ? -1 : m_frames.back().pts;
}

uint64_t FrameStore::memoryBytes() const
{
	uint64_t total = 0;
	for (const auto &f : m_frames) {
		total += f.planes.capacity();
	}
	return total;
}

// ============================================================================
// PlaybackSession
// ============================================================================

PlaybackSession::PlaybackSession() = default;

PlaybackSession::~PlaybackSession()
{
	stop();
}

bool PlaybackSession::startFromSnapshot(std::vector<CachedH264Packet> snapshot,
					int target_w, int target_h)
{
	if (m_thread.joinable()) {
		obs_log(LOG_WARNING, "[T3.4a] startFromSnapshot: thread already running");
		return false;
	}
	if (snapshot.empty()) {
		obs_log(LOG_WARNING, "[T3.4a] startFromSnapshot: empty snapshot");
		return false;
	}

	m_packets = std::move(snapshot);
	m_target_w = target_w;
	m_target_h = target_h;

	m_cancel = false;
	m_done = false;
	m_failed = false;
	m_packets_consumed = 0;
	m_frames_decoded = 0;
	m_decode_ms = 0;

	m_frames.clear();

	// --- Task 3.4b: compute duration + expected frame count immediately ---
	m_duration_sec = 0.0;
	m_expected_frames = 0;

	{
		size_t kf_idx = 0;
		for (size_t i = 0; i < m_packets.size(); ++i) {
			if (m_packets[i].keyframe) { kf_idx = i; break; }
		}
		m_expected_frames = static_cast<int64_t>(m_packets.size() - kf_idx);

		const CachedH264Packet &first = m_packets[kf_idx];
		const CachedH264Packet &last = m_packets.back();

		int64_t pts_diff = last.pts - first.pts;
		if (last.timebase_den > 0) {
			m_duration_sec = static_cast<double>((pts_diff + 1) * last.timebase_num) /
					 static_cast<double>(last.timebase_den);
		}

		obs_log(LOG_INFO,
			"[T3.4b] session prepared: duration=%.3f s, expected_frames=%lld, kf_idx=%llu",
			m_duration_sec, (long long)m_expected_frames,
			(unsigned long long)kf_idx);
	}
	// --- end Task 3.4b ---

	m_thread = std::thread([this]() { decodeLoop(); });
	return true;
}

void PlaybackSession::stop()
{
	m_cancel = true;
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void PlaybackSession::decodeLoop()
{
	const auto t0 = std::chrono::steady_clock::now();

	int kf_idx = -1;
	for (size_t i = 0; i < m_packets.size(); ++i) {
		if (m_packets[i].keyframe) {
			kf_idx = static_cast<int>(i);
			break;
		}
	}
	if (kf_idx < 0) {
		obs_log(LOG_WARNING, "[T3.4a] no keyframe in snapshot");
		m_failed = true;
		m_done = true;
		return;
	}

	obs_log(LOG_INFO, "[T3.4a] decode start: %llu packets, first keyframe at index=%d (pts=%lld)",
		(unsigned long long)m_packets.size(), kf_idx,
		(long long)m_packets[kf_idx].pts);

	H264Decoder decoder;
	if (!decoder.open(m_target_w, m_target_h)) {
		obs_log(LOG_ERROR, "[T3.4a] decoder.open failed");
		m_failed = true;
		m_done = true;
		return;
	}

	std::vector<YuvFrame> out;
	for (size_t i = static_cast<size_t>(kf_idx); i < m_packets.size(); ++i) {
		if (m_cancel.load()) break;

		out.clear();
		decoder.decode_packet(m_packets[i], out);

		m_packets_consumed = i + 1;

		if (!out.empty()) {
			std::lock_guard<std::mutex> lk(m_frames_mutex);
			for (auto &f : out) {
				m_frames.addFrame(std::move(f));
			}
			m_frames_decoded = m_frames.size();
			if (m_out_w == 0 && m_frames.size() > 0) {
				const YuvFrame *first = m_frames.at(0);
				if (first) {
					m_out_w = first->width;
					m_out_h = first->height;
				}
			}
		}
	}

	if (!m_cancel.load()) {
		out.clear();
		decoder.flush(out);
		if (!out.empty()) {
			std::lock_guard<std::mutex> lk(m_frames_mutex);
			for (auto &f : out) {
				m_frames.addFrame(std::move(f));
			}
			m_frames_decoded = m_frames.size();
		}
	}

	decoder.close();

	const auto t1 = std::chrono::steady_clock::now();
	const uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	m_decode_ms = ms;

	m_done = true;

	obs_log(LOG_INFO,
		"[T3.4a] decode DONE: %llu frames, %llu packets, %llu ms, %llu MB, out=%dx%d",
		(unsigned long long)m_frames_decoded.load(),
		(unsigned long long)m_packets_consumed.load(),
		(unsigned long long)ms,
		(unsigned long long)(frames().memoryBytes() / (1024 * 1024)),
		m_out_w, m_out_h);
}

PlaybackProgress PlaybackSession::progress() const
{
	PlaybackProgress p;
	p.total_packets = m_packets.size();
	p.packets_consumed = m_packets_consumed.load();
	p.frames_decoded = m_frames_decoded.load();
	p.decoding_done = m_done.load();
	p.decode_failed = m_failed.load();
	return p;
}

bool PlaybackSession::waitForDecode(int timeout_ms)
{
	if (timeout_ms < 0) {
		if (m_thread.joinable()) m_thread.join();
		return true;
	}

	const auto t0 = std::chrono::steady_clock::now();
	while (!m_done.load()) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		if (elapsed >= timeout_ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return true;
}

bool PlaybackSession::getFrame(size_t index, YuvFrame &out) const
{
	std::lock_guard<std::mutex> lk(m_frames_mutex);
	const YuvFrame *f = m_frames.at(index);
	if (!f) return false;
	out = *f;
	return true;
}

size_t PlaybackSession::frameCount() const
{
	std::lock_guard<std::mutex> lk(m_frames_mutex);
	return m_frames.size();
}

PlaybackStats PlaybackSession::stats() const
{
	std::lock_guard<std::mutex> lk(m_frames_mutex);
	PlaybackStats s;
	s.frame_count = m_frames.size();
	s.memory_bytes = m_frames.memoryBytes();
	s.decode_ms = m_decode_ms.load();
	s.first_pts = m_frames.firstPts();
	s.last_pts = m_frames.lastPts();
	s.width = m_out_w;
	s.height = m_out_h;
	return s;
}
