#pragma once

#ifdef _WIN32

#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

// Task 3.4d 階段 1: D3D11 VideoProcessor based scaler.
//
// Takes a D3D11 NV12 texture array (from FFmpeg hw decode) and produces a
// downscaled NV12 frame in system memory. All GPU work, one GPU->CPU copy.
//
// Any failure at init() or scale() returns false; caller must fall back.
class D3D11Scaler {
public:
	D3D11Scaler() = default;
	~D3D11Scaler();

	// device: the ID3D11Device that owns the decoder textures.
	// src_w/h: decoder output size (e.g. 3840x2160)
	// dst_w/h: target size (e.g. 1440x840), must be even
	bool init(ID3D11Device *device, int src_w, int src_h, int dst_w, int dst_h);
	void close();
	bool isReady() const { return m_ready; }

	// src_texture: the NV12 texture array from FFmpeg
	// src_slice  : which array slice (from frame->data[1])
	// out        : filled with dst_w*dst_h*1.5 bytes of NV12 (Y then UV)
	bool scale(ID3D11Texture2D *src_texture, int src_slice, std::vector<uint8_t> &out);

	int dstWidth() const { return m_dst_w; }
	int dstHeight() const { return m_dst_h; }

private:
	bool m_ready = false;

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

	Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_video_device;
	Microsoft::WRL::ComPtr<ID3D11VideoContext> m_video_context;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vp_enum;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;

	// Intermediate 4K NV12 texture (slice copy target with SRV bind).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_src_copy;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_src_view;

	// 1440x840 NV12 output texture (VideoProcessor output).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_dst_tex;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_dst_view;

	// CPU-readable staging texture (same size/format as m_dst_tex).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;

	int m_src_w = 0;
	int m_src_h = 0;
	int m_dst_w = 0;
	int m_dst_h = 0;
};

#endif // _WIN32
