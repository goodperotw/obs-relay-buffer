#pragma once

#include "h264-decoder.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Task 3.4a: decode a whole snapshot into an in-memory YUV frame store.
// Task 3.4b: expose duration / expected frame count for UI mapping.

class FrameStore {
public:
	void addFrame(YuvFrame &&f);

	size_t size() const { return m_frames.size(); }
	const YuvFrame *at(size_t index) const;
	const YuvFrame *findByPts(int64_t pts) const;
	int64_t firstPts() const;
	int64_t lastPts() const;
	uint64_t memoryBytes() const;
	void clear() { m_frames.clear(); }

private:
	std::vector<YuvFrame> m_frames;
};

struct PlaybackProgress {
	size_t total_packets = 0;
	size_t packets_consumed = 0;
	size_t frames_decoded = 0;
	bool decoding_done = false;
	bool decode_failed = false;
};

struct PlaybackStats {
	size_t frame_count = 0;
	uint64_t memory_bytes = 0;
	uint64_t decode_ms = 0;
	int64_t first_pts = -1;
	int64_t last_pts = -1;
	int width = 0;
	int height = 0;
};

class PlaybackSession {
public:
	PlaybackSession();
	~PlaybackSession();

	bool startFromSnapshot(std::vector<CachedH264Packet> snapshot,
			       int target_w = 0, int target_h = 0);
	void stop();

	// ---- Thread-safe queries ----
	PlaybackProgress progress() const;
	bool isDone() const { return m_done.load(); }
	bool isFailed() const { return m_failed.load(); }

	bool waitForDecode(int timeout_ms = -1);

	const FrameStore &frames() const { return m_frames; }

	// Copy one frame out (thread-safe during decode).
	bool getFrame(size_t index, YuvFrame &out) const;

	size_t frameCount() const;

	PlaybackStats stats() const;

	// ---- Task 3.4b: UI mapping helpers ----
	// Total duration of the snapshot (seconds), computed from the first
	// keyframe to the last packet. Known immediately after startFromSnapshot().
	double durationSec() const { return m_duration_sec; }

	// Expected number of frames after decode completes (= packets from
	// first keyframe to end). Known immediately after startFromSnapshot().
	int64_t expectedFrameCount() const { return m_expected_frames; }

private:
	void decodeLoop();

	std::vector<CachedH264Packet> m_packets;
	int m_target_w = 0;
	int m_target_h = 0;

	FrameStore m_frames;
	mutable std::mutex m_frames_mutex;

	std::thread m_thread;
	std::atomic<bool> m_cancel{false};
	std::atomic<bool> m_done{false};
	std::atomic<bool> m_failed{false};

	std::atomic<size_t> m_packets_consumed{0};
	std::atomic<size_t> m_frames_decoded{0};
	std::atomic<uint64_t> m_decode_ms{0};

	int m_out_w = 0;
	int m_out_h = 0;

	// Task 3.4b
	double m_duration_sec = 0.0;
	int64_t m_expected_frames = 0;
};
