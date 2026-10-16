/*
OBS Relay Buffer Plugin
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>

#include <QTimer>
#include "playback-session.hpp"
#include <QAction>
#include <QPointer>

#include <cstring>

#include "playback-session.hpp"
#include "plugin-support.h"
#include "relay-preview-window.hpp"
#include "h264-ring-buffer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Relay Buffer MVP: Tools menu entry and independent preview window.";
}

namespace {

QPointer<RelayPreviewWindow> g_relay_window;
H264RingBuffer *g_ring_buffer = nullptr;

obs_output_t *g_relay_output = nullptr;
obs_encoder_t *g_relay_encoder = nullptr;

struct obs_output_info g_relay_output_info;

// ----------------------------------------------------------
// Virtual Output callbacks
// ----------------------------------------------------------

const char *relay_output_get_name(void *)
{
	return "Relay Buffer Output";
}

void *relay_output_create(obs_data_t *, obs_output_t *output)
{
	// 直接把 output 本身當做 private data 回傳。
	// 後續 start/stop callback 會拿到這個 pointer。
	return output;
}

void relay_output_destroy(void *)
{
	// 我們沒有額外配置 private data，所以不用釋放。
}

bool relay_output_start(void *data)
{
	obs_output_t *output = static_cast<obs_output_t *>(data);

	bool can_begin = obs_output_can_begin_data_capture(output, 0);
	obs_log(LOG_INFO, "relay_output_start: can_begin_data_capture=%s", can_begin ? "true" : "false");

	if (!can_begin) {
		return false;
	}

	bool init_encoders = obs_output_initialize_encoders(output, 0);
	obs_log(LOG_INFO, "relay_output_start: initialize_encoders=%s", init_encoders ? "true" : "false");

	if (!init_encoders) {
		return false;
	}

	obs_output_begin_data_capture(output, 0);
	obs_log(LOG_INFO, "relay_output_start: data capture begun");

	return true;
}

void relay_output_stop(void *data, uint64_t)
{
	obs_output_t *output = static_cast<obs_output_t *>(data);
	obs_output_end_data_capture(output);
	obs_log(LOG_INFO, "relay_output_stop: data capture ended");
}

// ----------------------------------------------------------
// Task 3.2: NAL scan diagnostics (temporary, observation only)
// ----------------------------------------------------------

// Find Annex B start code (00 00 01 or 00 00 00 01) starting from 'from'.
// On success returns true and sets 'pos' / 'sc_len'.
static bool find_start_code(const uint8_t *data, size_t size, size_t from, size_t &pos, size_t &sc_len)
{
	for (size_t i = from; i + 3 <= size; ++i) {
		if (data[i] != 0x00 || data[i + 1] != 0x00) {
			continue;
		}
		if (data[i + 2] == 0x01) {
			pos = i;
			sc_len = 3;
			return true;
		}
		if (i + 4 <= size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
			pos = i;
			sc_len = 4;
			return true;
		}
	}
	return false;
}

struct NalScanResult {
	bool annex_b = false; // did we find any Annex B start code?
	int total_nal = 0;
	int sps = 0;     // nal_type 7
	int pps = 0;     // nal_type 8
	int idr = 0;     // nal_type 5
	int non_idr = 0; // nal_type 1
	int other = 0;
};

static NalScanResult scan_nal_units(const uint8_t *data, size_t size)
{
	NalScanResult r;
	if (!data || size < 4) {
		return r;
	}

	size_t pos = 0;
	size_t sc_len = 0;
	if (!find_start_code(data, size, 0, pos, sc_len)) {
		return r; // no Annex B start code found
	}
	r.annex_b = true;

	size_t nal_start = pos + sc_len;
	size_t search_from = nal_start;

	while (true) {
		size_t next_pos = 0;
		size_t next_sc = 0;
		bool has_next = find_start_code(data, size, search_from, next_pos, next_sc);

		size_t nal_end = has_next ? next_pos : size;
		if (nal_end > nal_start) {
			// NAL header is the first byte after the start code.
			// forbidden_zero_bit(1) nal_ref_idc(2) nal_unit_type(5)
			uint8_t header = data[nal_start];
			int nal_type = header & 0x1F;

			++r.total_nal;
			switch (nal_type) {
			case 7:
				++r.sps;
				break;
			case 8:
				++r.pps;
				break;
			case 5:
				++r.idr;
				break;
			case 1:
				++r.non_idr;
				break;
			default:
				++r.other;
				break;
			}
		}

		if (!has_next) {
			break;
		}
		nal_start = next_pos + next_sc;
		search_from = nal_start;
	}

	return r;
}

void relay_receive_packet(void *, struct encoder_packet *packet)
{
	if (!g_ring_buffer || !packet) {
		return;
	}

	static int packet_count = 0;
	packet_count++;

	// --- Task 3.2 diagnostics (temporary) ---
	static int total_sps = 0;
	static int total_pps = 0;
	static int total_idr = 0;
	static int total_non_idr = 0;
	static int total_other = 0;
	static int keyframes_seen = 0;
	static int keyframes_with_sps_pps = 0;
	static int non_annexb_pkts = 0;

	if (packet->data && packet->size > 0) {
		NalScanResult r = scan_nal_units((const uint8_t *)packet->data, (size_t)packet->size);

		total_sps += r.sps;
		total_pps += r.pps;
		total_idr += r.idr;
		total_non_idr += r.non_idr;
		total_other += r.other;

		if (!r.annex_b) {
			non_annexb_pkts++;
		}

		if (packet->keyframe) {
			keyframes_seen++;
			if (r.sps > 0 && r.pps > 0) {
				keyframes_with_sps_pps++;
			}

			// --- Task 3.2b: keyframe interval statistics ---
			static int64_t prev_kf_pts = -1;
			static int64_t prev_kf_tb_num = 0;
			static int64_t prev_kf_tb_den = 0;
			static double interval_sum_sec = 0.0;
			static int interval_count = 0;

			if (prev_kf_pts >= 0 && prev_kf_tb_den > 0 && packet->timebase_den > 0) {
				int64_t pts_diff = packet->pts - prev_kf_pts;
				if (pts_diff > 0) {
					double interval_sec = (double)(pts_diff * packet->timebase_num) /
							      (double)packet->timebase_den;

					interval_sum_sec += interval_sec;
					interval_count++;

					// Log every 10 keyframe intervals.
					if (interval_count % 10 == 0) {
						double avg = interval_sum_sec / interval_count;
						obs_log(LOG_INFO,
							"[T3.2b] keyframe interval: last=%.3f s, avg=%.3f s over %d intervals",
							interval_sec, avg, interval_count);
					}
				}
			}

			prev_kf_pts = packet->pts;
			prev_kf_tb_num = packet->timebase_num;
			prev_kf_tb_den = packet->timebase_den;
			// --- end Task 3.2b ---

			// First few keyframes: print full detail so we can verify SPS/PPS.
			if (keyframes_seen <= 3) {
				obs_log(LOG_INFO,
					"[T3.2] keyframe #%d: size=%u annex_b=%d nal=%d "
					"SPS=%d PPS=%d IDR=%d non-IDR=%d other=%d",
					keyframes_seen, (unsigned)packet->size, r.annex_b ? 1 : 0, r.total_nal, r.sps,
					r.pps, r.idr, r.non_idr, r.other);
			}
		}
	}
	// --- end Task 3.2 diagnostics ---

	// Do not log every packet; log every 500 packets.
	if (packet_count % 500 == 0) {
		obs_log(LOG_INFO, "Received H264 packet #%d, type=%d, size=%u", packet_count, (int)packet->type,
			(unsigned)packet->size);

		obs_log(LOG_INFO,
			"[T3.2] cumul: SPS=%d PPS=%d IDR=%d non-IDR=%d other=%d | "
			"keyframes=%d with_sps_pps=%d | non_annexb_pkts=%d",
			total_sps, total_pps, total_idr, total_non_idr, total_other, keyframes_seen,
			keyframes_with_sps_pps, non_annexb_pkts);
	}

	g_ring_buffer->push(packet);
}

void init_output_info()
{
	std::memset(&g_relay_output_info, 0, sizeof(g_relay_output_info));

	g_relay_output_info.id = "relay_buffer_output";
	g_relay_output_info.flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_VIDEO;

	// 這一行很重要：告訴 OBS 這個 encoded output 要吃 h264 packet。
	g_relay_output_info.encoded_video_codecs = "h264";
	g_relay_output_info.encoded_audio_codecs = nullptr;

	g_relay_output_info.get_name = relay_output_get_name;
	g_relay_output_info.create = relay_output_create;
	g_relay_output_info.destroy = relay_output_destroy;
	g_relay_output_info.start = relay_output_start;
	g_relay_output_info.stop = relay_output_stop;
	g_relay_output_info.encoded_packet = relay_receive_packet;
}

// Compute a sensible encoder bitrate from the source video parameters.
// ~0.08 bpp is enough for clean 4K playback on a 1440x840 preview, and
// scales automatically if the OBS canvas changes later.
static int compute_target_bitrate_kbps(const struct video_output_info *voi)
{
	if (!voi || voi->width == 0 || voi->height == 0 || voi->fps_den == 0) {
		return 30000; // safe fallback
	}

	const double fps = static_cast<double>(voi->fps_num) /
			   static_cast<double>(voi->fps_den);
	const double pixels_per_sec =
		static_cast<double>(voi->width) * static_cast<double>(voi->height) * fps;

	constexpr double kBitsPerPixel = 0.08;
	int kbps = static_cast<int>(pixels_per_sec * kBitsPerPixel / 1000.0);

	// Clamp: never starve, never saturate the link.
	if (kbps < 5000) kbps = 5000;
	if (kbps > 80000) kbps = 80000;

	obs_log(LOG_INFO,
		"start_relay_capture: computed bitrate=%d kbps (%.2f bpp @ %ux%u %.2f fps)",
		kbps, kBitsPerPixel, (unsigned)voi->width, (unsigned)voi->height, fps);

	return kbps;
}

// ----------------------------------------------------------
// Capture lifecycle
// ----------------------------------------------------------

void start_relay_capture()
{
	if (g_relay_output) {
		obs_log(LOG_INFO, "start_relay_capture: already started");
		return;
	}

	if (!g_ring_buffer) {
		g_ring_buffer = new H264RingBuffer();
	}

	video_t *video = obs_get_video();
	if (!video) {
		obs_log(LOG_ERROR, "start_relay_capture: obs_get_video() returned NULL");
		return;
	}

	const struct video_output_info *voi = video_output_get_info(video);
	if (voi) {
		obs_log(LOG_INFO, "start_relay_capture: main video %ux%u @ %u/%u fps", (unsigned)voi->width,
			(unsigned)voi->height, (unsigned)voi->fps_num, (unsigned)voi->fps_den);
	} else {
		obs_log(LOG_WARNING, "start_relay_capture: video_output_get_info returned NULL");
	}

	// 建立 x264 H.264 encoder。
	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_string(enc_settings, "preset", "ultrafast");
	obs_data_set_string(enc_settings, "tune", "zerolatency");
	const int bitrate_kbps = compute_target_bitrate_kbps(voi);
	obs_data_set_int(enc_settings, "bitrate", bitrate_kbps);
	obs_data_set_int(enc_settings, "keyint_sec", 2);

	g_relay_encoder = obs_video_encoder_create("obs_x264", "relay_h264_enc", enc_settings, nullptr);
	obs_data_release(enc_settings);

	if (!g_relay_encoder) {
		obs_log(LOG_ERROR, "start_relay_capture: failed to create obs_x264 encoder");
		return;
	}

	// 把 encoder 接到 OBS 主影片輸出。
	obs_encoder_set_video(g_relay_encoder, video);

	// 建立我們的虛擬 encoded output。
	obs_data_t *out_settings = obs_data_create();
	g_relay_output = obs_output_create("relay_buffer_output", "relay_output", out_settings, nullptr);
	obs_data_release(out_settings);

	if (!g_relay_output) {
		obs_log(LOG_ERROR, "start_relay_capture: failed to create relay output");
		obs_encoder_release(g_relay_encoder);
		g_relay_encoder = nullptr;
		return;
	}

	// 把 encoder 掛給 output。
	obs_output_set_video_encoder(g_relay_output, g_relay_encoder);

	// 啟動 output。
	if (obs_output_start(g_relay_output)) {
		obs_log(LOG_INFO, "start_relay_capture: relay output started successfully");
	} else {
		obs_log(LOG_ERROR, "start_relay_capture: obs_output_start failed");

		obs_output_release(g_relay_output);
		g_relay_output = nullptr;

		obs_encoder_release(g_relay_encoder);
		g_relay_encoder = nullptr;
	}
}

void stop_relay_capture()
{
	if (g_relay_output) {
		obs_output_stop(g_relay_output);
		obs_output_release(g_relay_output);
		g_relay_output = nullptr;
		obs_log(LOG_INFO, "stop_relay_capture: relay output released");
	}

	if (g_relay_encoder) {
		obs_encoder_release(g_relay_encoder);
		g_relay_encoder = nullptr;
		obs_log(LOG_INFO, "stop_relay_capture: relay encoder released");
	}
}

// ----------------------------------------------------------
// UI / frontend events
// ----------------------------------------------------------

void destroy_relay_window()
{
	if (!g_relay_window) {
		return;
	}

	RelayPreviewWindow *window = g_relay_window.data();
	g_relay_window.clear();

	window->setAttribute(Qt::WA_DeleteOnClose, false);
	delete window;
}

void open_or_focus_relay_window()
{
	if (g_relay_window) {
		obs_log(LOG_INFO, "Relay Preview already open; raising existing window");
		g_relay_window->presentAndFocus();
		return;
	}

	auto *window = new RelayPreviewWindow(nullptr);

	QObject::connect(window, &QObject::destroyed, []() { g_relay_window = nullptr; });

	g_relay_window = window;
	window->presentAndFocus();

	obs_log(LOG_INFO, "Relay Preview window created");
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN || event == OBS_FRONTEND_EVENT_EXIT) {
		obs_log(LOG_INFO, "OBS is shutting down; closing Relay Preview");
		destroy_relay_window();
	}
}

} // namespace

// 給 relay-preview-window.cpp 使用
H264RingBuffer *get_global_ring_buffer()
{
	return g_ring_buffer;
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	obs_log(LOG_INFO, "FFmpeg version: %s", av_version_info());
	obs_log(LOG_INFO, "avcodec: %u.%u.%u", avcodec_version() >> 16, (avcodec_version() >> 8) & 0xFF,
		avcodec_version() & 0xFF);
	init_output_info();
	obs_register_output(&g_relay_output_info);

	start_relay_capture();

	const char *menu_text = obs_module_text("OpenRelayWindow");
	if (!menu_text) {
		menu_text = "Open Relay Window";
	}

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(menu_text));

	if (!action) {
		obs_log(LOG_ERROR, "failed to inject Tools menu item");
		return false;
	}

	QObject::connect(action, &QAction::triggered, open_or_focus_relay_window);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	destroy_relay_window();

	stop_relay_capture();

	if (g_ring_buffer) {
		delete g_ring_buffer;
		g_ring_buffer = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
