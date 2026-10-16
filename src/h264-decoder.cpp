#include "h264-decoder.hpp"

#include <obs-module.h>
#include "plugin-support.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace {

int find_first_keyframe_index(const std::vector<CachedH264Packet> &packets)
{
	for (size_t i = 0; i < packets.size(); ++i) {
		if (packets[i].keyframe) return static_cast<int>(i);
	}
	return -1;
}

const char *pix_fmt_name_safe(int fmt)
{
	const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt));
	return name ? name : "unknown";
}

} // namespace

// ============================================================================
// Stateless (3.3b / 3.3c)
// ============================================================================

DecodedFrameInfo H264Decoder::decode_first_frame(const std::vector<CachedH264Packet> &packets,
						 int max_packets_to_feed) const
{
	DecodedFrameInfo result;

	if (packets.empty()) {
		obs_log(LOG_WARNING, "[T3.3b] decode_first_frame: empty snapshot");
		return result;
	}

	int kf_index = find_first_keyframe_index(packets);
	if (kf_index < 0) {
		obs_log(LOG_WARNING, "[T3.3b] decode_first_frame: no keyframe in snapshot");
		return result;
	}

	obs_log(LOG_INFO, "[T3.3b] decoder start: snapshot=%llu pkts, first keyframe at index=%d (pts=%lld)",
		(unsigned long long)packets.size(), kf_index, (long long)packets[kf_index].pts);

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		obs_log(LOG_ERROR, "[T3.3b] avcodec_find_decoder(H264) failed");
		return result;
	}

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		obs_log(LOG_ERROR, "[T3.3b] avcodec_alloc_context3 failed");
		return result;
	}

	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->thread_count = 1;

	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		obs_log(LOG_ERROR, "[T3.3b] avcodec_open2 failed");
		avcodec_free_context(&ctx);
		return result;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (!pkt || !frame) {
		obs_log(LOG_ERROR, "[T3.3b] av_packet_alloc / av_frame_alloc failed");
		if (pkt) av_packet_free(&pkt);
		if (frame) av_frame_free(&frame);
		avcodec_free_context(&ctx);
		return result;
	}

	int fed = 0;
	bool got_frame = false;

	for (size_t i = static_cast<size_t>(kf_index);
	     i < packets.size() && fed < max_packets_to_feed;
	     ++i) {

		const CachedH264Packet &cp = packets[i];

		if (av_new_packet(pkt, static_cast<int>(cp.size)) < 0) {
			obs_log(LOG_ERROR, "[T3.3b] av_new_packet failed at index=%llu", (unsigned long long)i);
			break;
		}
		memcpy(pkt->data, cp.data.data(), cp.size);
		pkt->pts = cp.pts;
		pkt->dts = cp.dts;

		int send_ret = avcodec_send_packet(ctx, pkt);
		av_packet_unref(pkt);

		if (send_ret < 0) {
			obs_log(LOG_WARNING, "[T3.3b] avcodec_send_packet failed at index=%llu ret=%d",
				(unsigned long long)i, send_ret);
		}

		fed++;

		int recv_ret = avcodec_receive_frame(ctx, frame);
		if (recv_ret == 0) {
			result.valid = true;
			result.width = frame->width;
			result.height = frame->height;
			result.format = frame->format;
			result.pts = frame->pts;
			result.decoded_index = static_cast<int>(i);
			got_frame = true;

			// 8 specifiers / 8 args (the old 3.3b bug had 9/7).
			obs_log(LOG_INFO,
				"[T3.3b] GOT FRAME: %dx%d fmt=%d (%s) pts=%lld "
				"fed=%d from_index=%llu keyframe=%d",
				frame->width, frame->height, frame->format,
				pix_fmt_name_safe(frame->format),
				(long long)frame->pts,
				fed, (unsigned long long)i, cp.keyframe ? 1 : 0);
			break;
		}

		if (recv_ret != AVERROR(EAGAIN)) {
			obs_log(LOG_WARNING,
				"[T3.3b] avcodec_receive_frame ret=%d at index=%llu (fed=%d)",
				recv_ret, (unsigned long long)i, fed);
		}
	}

	if (!got_frame) {
		obs_log(LOG_WARNING,
			"[T3.3b] no frame produced after feeding %d packets starting at keyframe index=%d",
			fed, kf_index);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	return result;
}

DecodedRgbFrame H264Decoder::decode_first_frame_rgb(const std::vector<CachedH264Packet> &packets,
						    int max_packets_to_feed) const
{
	DecodedRgbFrame rgb;

	if (packets.empty()) return rgb;

	int kf_index = find_first_keyframe_index(packets);
	if (kf_index < 0) {
		obs_log(LOG_WARNING, "[T3.3c] decode_first_frame_rgb: no keyframe");
		return rgb;
	}

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) return rgb;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) return rgb;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->thread_count = 1;

	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return rgb;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (!pkt || !frame) {
		if (pkt) av_packet_free(&pkt);
		if (frame) av_frame_free(&frame);
		avcodec_free_context(&ctx);
		return rgb;
	}

	bool got_frame = false;
	int fed = 0;

	for (size_t i = static_cast<size_t>(kf_index);
	     i < packets.size() && fed < max_packets_to_feed && !got_frame;
	     ++i) {

		const CachedH264Packet &cp = packets[i];

		if (av_new_packet(pkt, static_cast<int>(cp.size)) < 0) break;
		memcpy(pkt->data, cp.data.data(), cp.size);
		pkt->pts = cp.pts;
		pkt->dts = cp.dts;

		avcodec_send_packet(ctx, pkt);
		av_packet_unref(pkt);
		fed++;

		int recv_ret = avcodec_receive_frame(ctx, frame);
		if (recv_ret == 0) {
			got_frame = true;

			obs_log(LOG_INFO, "[T3.3c] GOT FRAME: %dx%d fmt=%d (%s) pts=%lld",
				frame->width, frame->height, frame->format,
				pix_fmt_name_safe(frame->format), (long long)frame->pts);
		} else if (recv_ret != AVERROR(EAGAIN)) {
			obs_log(LOG_WARNING, "[T3.3c] avcodec_receive_frame ret=%d", recv_ret);
		}
	}

	if (!got_frame) {
		obs_log(LOG_WARNING, "[T3.3c] no frame produced");
		av_frame_free(&frame);
		av_packet_free(&pkt);
		avcodec_free_context(&ctx);
		return rgb;
	}

	// YUV -> RGB24
	SwsContext *sws = sws_getContext(frame->width, frame->height,
					 static_cast<AVPixelFormat>(frame->format),
					 frame->width, frame->height,
					 AV_PIX_FMT_RGB24,
					 SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws) {
		obs_log(LOG_ERROR, "[T3.3c] sws_getContext failed");
		av_frame_free(&frame);
		av_packet_free(&pkt);
		avcodec_free_context(&ctx);
		return rgb;
	}

	const int stride = frame->width * 3;
	rgb.rgb.resize(static_cast<size_t>(stride) * static_cast<size_t>(frame->height));

	uint8_t *dst_data[4] = { rgb.rgb.data(), nullptr, nullptr, nullptr };
	int dst_linesize[4] = { stride, 0, 0, 0 };

	int out_h = sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
			      dst_data, dst_linesize);
	sws_freeContext(sws);

	if (out_h <= 0) {
		obs_log(LOG_ERROR, "[T3.3c] sws_scale returned %d", out_h);
		rgb.rgb.clear();
		av_frame_free(&frame);
		av_packet_free(&pkt);
		avcodec_free_context(&ctx);
		return rgb;
	}

	rgb.valid = true;
	rgb.width = frame->width;
	rgb.height = frame->height;
	rgb.stride = stride;
	rgb.pts = frame->pts;

	obs_log(LOG_INFO, "[T3.3c] GOT RGB FRAME: %dx%d stride=%d bytes=%llu pts=%lld",
		rgb.width, rgb.height, rgb.stride,
		(unsigned long long)rgb.rgb.size(), (long long)rgb.pts);

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	return rgb;
}

// ============================================================================
// Stateful / streaming (3.4a)
// ============================================================================

H264Decoder::~H264Decoder()
{
	close();
}

bool H264Decoder::open(int target_w, int target_h)
{
	close();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		obs_log(LOG_ERROR, "[T3.4a] avcodec_find_decoder(H264) failed");
		return false;
	}

	m_ctx = avcodec_alloc_context3(codec);
	if (!m_ctx) {
		obs_log(LOG_ERROR, "[T3.4a] avcodec_alloc_context3 failed");
		return false;
	}

	m_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	m_ctx->thread_count = 1;

	if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
		obs_log(LOG_ERROR, "[T3.4a] avcodec_open2 failed");
		close();
		return false;
	}

	m_pkt = av_packet_alloc();
	m_frame = av_frame_alloc();
	if (!m_pkt || !m_frame) {
		obs_log(LOG_ERROR, "[T3.4a] alloc pkt/frame failed");
		close();
		return false;
	}

	m_target_w = target_w;
	m_target_h = target_h;

	obs_log(LOG_INFO, "[T3.4a] decoder opened (target=%dx%d)",
		target_w, target_h);
	return true;
}

void H264Decoder::close()
{
	if (m_sws) {
		sws_freeContext(m_sws);
		m_sws = nullptr;
	}
	if (m_frame) {
		av_frame_free(&m_frame);
		m_frame = nullptr;
	}
	if (m_pkt) {
		av_packet_free(&m_pkt);
		m_pkt = nullptr;
	}
	if (m_ctx) {
		avcodec_free_context(&m_ctx);
		m_ctx = nullptr;
	}

	m_src_w = 0;
	m_src_h = 0;
	m_src_fmt = -1;
	m_sws_src_fmt = -1;
	m_sws_dst_w = 0;
	m_sws_dst_h = 0;
}

void H264Decoder::emitFrame(std::vector<YuvFrame> &out)
{
	// On first frame, lock in source dimensions.
	if (m_src_w == 0) {
		m_src_w = m_frame->width;
		m_src_h = m_frame->height;
		m_src_fmt = m_frame->format;
	}

	// Compute destination size (even numbers required for YUV420P).
	int dst_w = m_target_w > 0 ? m_target_w : m_src_w;
	int dst_h = m_target_h > 0 ? m_target_h : m_src_h;
	dst_w &= ~1;
	dst_h &= ~1;

	// (Re)build sws if anything relevant changed.
	if (!m_sws || m_sws_src_fmt != m_src_fmt || m_sws_dst_w != dst_w || m_sws_dst_h != dst_h) {
		if (m_sws) sws_freeContext(m_sws);
		m_sws = sws_getContext(m_src_w, m_src_h,
				       static_cast<AVPixelFormat>(m_src_fmt),
				       dst_w, dst_h,
				       AV_PIX_FMT_YUV420P,
				       SWS_BILINEAR, nullptr, nullptr, nullptr);
		m_sws_src_fmt = m_src_fmt;
		m_sws_dst_w = dst_w;
		m_sws_dst_h = dst_h;

		if (!m_sws) {
			obs_log(LOG_ERROR, "[T3.4a] sws_getContext failed (src=%dx%d fmt=%d, dst=%dx%d)",
				m_src_w, m_src_h, m_src_fmt, dst_w, dst_h);
			return;
		}
	}

	const size_t y_size = static_cast<size_t>(dst_w) * dst_h;
	const size_t uv_size = static_cast<size_t>(dst_w / 2) * (dst_h / 2);

	YuvFrame yf;
	yf.pts = m_frame->pts;
	yf.width = dst_w;
	yf.height = dst_h;
	yf.planes.resize(y_size + uv_size * 2);

	uint8_t *dst_data[4] = {
		yf.planes.data(),
		yf.planes.data() + y_size,
		yf.planes.data() + y_size + uv_size,
		nullptr,
	};
	int dst_linesize[4] = { dst_w, dst_w / 2, dst_w / 2, 0 };

	int scaled = sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_src_h,
			       dst_data, dst_linesize);
	if (scaled <= 0) {
		obs_log(LOG_WARNING, "[T3.4a] sws_scale returned %d", scaled);
		return;
	}

	out.push_back(std::move(yf));
}

bool H264Decoder::decode_packet(const CachedH264Packet &pkt, std::vector<YuvFrame> &out)
{
	if (!m_ctx || !m_pkt || !m_frame) return false;

	if (av_new_packet(m_pkt, static_cast<int>(pkt.size)) < 0) return false;
	memcpy(m_pkt->data, pkt.data.data(), pkt.size);
	m_pkt->pts = pkt.pts;
	m_pkt->dts = pkt.dts;

	int send_ret = avcodec_send_packet(m_ctx, m_pkt);
	av_packet_unref(m_pkt);

	if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
		// Not fatal — some streams tolerate this.
		// We still attempt to drain.
	}

	while (true) {
		int recv_ret = avcodec_receive_frame(m_ctx, m_frame);
		if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) break;
		if (recv_ret < 0) {
			obs_log(LOG_WARNING, "[T3.4a] avcodec_receive_frame ret=%d", recv_ret);
			return false;
		}
		emitFrame(out);
		av_frame_unref(m_frame);
	}
	return true;
}

void H264Decoder::flush(std::vector<YuvFrame> &out)
{
	if (!m_ctx || !m_frame) return;

	avcodec_send_packet(m_ctx, nullptr);

	while (true) {
		int recv_ret = avcodec_receive_frame(m_ctx, m_frame);
		if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) break;
		if (recv_ret < 0) break;
		emitFrame(out);
		av_frame_unref(m_frame);
	}
}
