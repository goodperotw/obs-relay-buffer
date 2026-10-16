#include "d3d11-scaler.hpp"

#ifdef _WIN32

#include <obs-module.h>
#include "plugin-support.h"

namespace {

const char *hr_str(HRESULT hr, char *buf, size_t len)
{
	snprintf(buf, len, "0x%08lX", (unsigned long)hr);
	return buf;
}

} // namespace

D3D11Scaler::~D3D11Scaler()
{
	close();
}

void D3D11Scaler::close()
{
	m_ready = false;

	m_staging.Reset();
	m_dst_view.Reset();
	m_dst_tex.Reset();
	m_src_view.Reset();
	m_src_copy.Reset();
	m_vp.Reset();
	m_vp_enum.Reset();
	m_video_context.Reset();
	m_video_device.Reset();
	m_context.Reset();
	m_device.Reset();

	m_src_w = m_src_h = m_dst_w = m_dst_h = 0;
}

bool D3D11Scaler::init(ID3D11Device *device, int src_w, int src_h, int dst_w, int dst_h)
{
	close();

	if (!device) {
		obs_log(LOG_ERROR, "[T3.4d-1] D3D11Scaler::init: null device");
		return false;
	}
	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
		obs_log(LOG_ERROR, "[T3.4d-1] D3D11Scaler::init: bad sizes %dx%d -> %dx%d",
			src_w, src_h, dst_w, dst_h);
		return false;
	}

	char hr_buf[16];
	HRESULT hr;

	m_device = device;

	// --- device context -----------------------------------------------------
	device->GetImmediateContext(&m_context);
	if (!m_context) {
		obs_log(LOG_ERROR, "[T3.4d-1] GetImmediateContext returned null");
		close();
		return false;
	}

	// --- video device / context --------------------------------------------
	hr = device->QueryInterface(__uuidof(ID3D11VideoDevice),
				    reinterpret_cast<void **>(m_video_device.GetAddressOf()));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] QI ID3D11VideoDevice failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	hr = m_context->QueryInterface(__uuidof(ID3D11VideoContext),
				       reinterpret_cast<void **>(m_video_context.GetAddressOf()));
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] QI ID3D11VideoContext failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- video processor enumerator + processor ----------------------------
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputWidth = (UINT)src_w;
	content.InputHeight = (UINT)src_h;
	content.OutputWidth = (UINT)dst_w;
	content.OutputHeight = (UINT)dst_h;
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	hr = m_video_device->CreateVideoProcessorEnumerator(&content, &m_vp_enum);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateVideoProcessorEnumerator failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	hr = m_video_device->CreateVideoProcessor(m_vp_enum.Get(), 0, &m_vp);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateVideoProcessor failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- intermediate 4K NV12 texture (SRV bind, slice copy target) --------
	D3D11_TEXTURE2D_DESC src_desc{};
	src_desc.Width = (UINT)src_w;
	src_desc.Height = (UINT)src_h;
	src_desc.MipLevels = 1;
	src_desc.ArraySize = 1;
	src_desc.Format = DXGI_FORMAT_NV12;
	src_desc.SampleDesc.Count = 1;
	src_desc.SampleDesc.Quality = 0;
	src_desc.Usage = D3D11_USAGE_DEFAULT;
	src_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	src_desc.CPUAccessFlags = 0;
	src_desc.MiscFlags = 0;

	hr = device->CreateTexture2D(&src_desc, nullptr, &m_src_copy);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateTexture2D(src_copy) failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- input view for the processor --------------------------------------
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view_desc{};
	in_view_desc.FourCC = 0;
	in_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	in_view_desc.Texture2D.ArraySlice = 0; // m_src_copy is ArraySize=1
	in_view_desc.Texture2D.MipSlice = 0;

	hr = m_video_device->CreateVideoProcessorInputView(
		m_src_copy.Get(), m_vp_enum.Get(), &in_view_desc, &m_src_view);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateVideoProcessorInputView failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- output 1440x840 NV12 texture (render target) ----------------------
	D3D11_TEXTURE2D_DESC dst_desc{};
	dst_desc.Width = (UINT)dst_w;
	dst_desc.Height = (UINT)dst_h;
	dst_desc.MipLevels = 1;
	dst_desc.ArraySize = 1;
	dst_desc.Format = DXGI_FORMAT_NV12;
	dst_desc.SampleDesc.Count = 1;
	dst_desc.SampleDesc.Quality = 0;
	dst_desc.Usage = D3D11_USAGE_DEFAULT;
	dst_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	dst_desc.CPUAccessFlags = 0;
	dst_desc.MiscFlags = 0;

	hr = device->CreateTexture2D(&dst_desc, nullptr, &m_dst_tex);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateTexture2D(dst_tex) failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- output view -------------------------------------------------------
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
	out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	out_view_desc.Texture2D.MipSlice = 0;

	hr = m_video_device->CreateVideoProcessorOutputView(
		m_dst_tex.Get(), m_vp_enum.Get(), &out_view_desc, &m_dst_view);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateVideoProcessorOutputView failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	// --- staging texture for CPU readback ----------------------------------
	D3D11_TEXTURE2D_DESC stg_desc = dst_desc;
	stg_desc.Usage = D3D11_USAGE_STAGING;
	stg_desc.BindFlags = 0;
	stg_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stg_desc.MiscFlags = 0;

	hr = device->CreateTexture2D(&stg_desc, nullptr, &m_staging);
	if (FAILED(hr)) {
		obs_log(LOG_ERROR, "[T3.4d-1] CreateTexture2D(staging) failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		close();
		return false;
	}

	m_src_w = src_w;
	m_src_h = src_h;
	m_dst_w = dst_w;
	m_dst_h = dst_h;
	m_ready = true;

	obs_log(LOG_INFO,
		"[T3.4d-1] D3D11Scaler ready: %dx%d -> %dx%d (NV12)",
		src_w, src_h, dst_w, dst_h);
	return true;
}

bool D3D11Scaler::scale(ID3D11Texture2D *src_texture, int src_slice, std::vector<uint8_t> &out)
{
	if (!m_ready || !src_texture) {
		return false;
	}

	char hr_buf[16];
	HRESULT hr;

	// --- 1. copy the decoder array slice into our SRV-capable texture ------
	// The decoder texture only has D3D11_BIND_DECODER, so we cannot create
	// a shader resource view on it directly. Blit one slice into m_src_copy.
	m_context->CopySubresourceRegion(
		m_src_copy.Get(), D3D11CalcSubresource(0, 0, 1), // dst: mip 0, slice 0
		0, 0, 0,                                          // dst x/y/z
		src_texture,
		D3D11CalcSubresource(0, (UINT)src_slice, 1),      // src: mip 0, slice N
		nullptr);                                         // copy whole subresource

	// --- 2. run the video processor (GPU scale) ---------------------------
	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.OutputIndex = 0;
	stream.InputFrameOrField = 0;
	stream.PastFrames = 0;
	stream.FutureFrames = 0;
	stream.pInputSurface = m_src_view.Get();

	hr = m_video_context->VideoProcessorBlt(m_vp.Get(), m_dst_view.Get(), 0, 1, &stream);
	if (FAILED(hr)) {
		obs_log(LOG_WARNING, "[T3.4d-1] VideoProcessorBlt failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		return false;
	}

	// --- 3. copy to staging ------------------------------------------------
	m_context->CopyResource(m_staging.Get(), m_dst_tex.Get());

	// --- 4. map + memcpy to out -------------------------------------------
	D3D11_MAPPED_SUBRESOURCE mapped{};
	hr = m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		obs_log(LOG_WARNING, "[T3.4d-1] Map(staging) failed (%s)",
			hr_str(hr, hr_buf, sizeof(hr_buf)));
		return false;
	}

	// NV12 layout: Y plane (full size) then interleaved UV plane (half height).
	// The mapped row pitch may be larger than the logical width.
	const int y_rows = m_dst_h;
	const int uv_rows = m_dst_h / 2;
	const int y_row_bytes = m_dst_w;
	const int uv_row_bytes = m_dst_w; // interleaved U+V, 2 bytes per 2 pixels

	const size_t total = (size_t)m_dst_w * m_dst_h * 3 / 2;
	out.resize(total);

	const uint8_t *src_base = static_cast<const uint8_t *>(mapped.pData);
	uint8_t *dst_base = out.data();

	// Y plane
	for (int r = 0; r < y_rows; ++r) {
		memcpy(dst_base + (size_t)r * y_row_bytes,
		       src_base + (size_t)r * mapped.RowPitch,
		       (size_t)y_row_bytes);
	}

	// UV plane (comes right after Y plane in NV12 layout)
	uint8_t *dst_uv = dst_base + (size_t)y_row_bytes * y_rows;
	const uint8_t *src_uv = src_base + (size_t)mapped.RowPitch * y_rows;
	for (int r = 0; r < uv_rows; ++r) {
		memcpy(dst_uv + (size_t)r * uv_row_bytes,
		       src_uv + (size_t)r * mapped.RowPitch,
		       (size_t)uv_row_bytes);
	}

	m_context->Unmap(m_staging.Get(), 0);
	return true;
}

#endif // _WIN32
