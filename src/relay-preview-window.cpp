/*
OBS Relay Buffer — MVP-2 playback window (Task 3.4b)
*/

#include "relay-preview-window.hpp"
#include "h264-ring-buffer.hpp"
#include "playback-session.hpp"

#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <obs-module.h>
#include "plugin-support.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern H264RingBuffer *get_global_ring_buffer();

namespace {

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 600;
constexpr int kPlayTimerMs = 16;   // ~60 Hz
constexpr int kTargetWidth = 1640;
constexpr int kTargetHeight = 920;

QImage yuvToQImage(const YuvFrame &yuv)
{
	if (yuv.width <= 0 || yuv.height <= 0 || yuv.planes.empty()) {
		return QImage();
	}

	SwsContext *sws = sws_getContext(yuv.width, yuv.height, AV_PIX_FMT_YUV420P,
					 yuv.width, yuv.height, AV_PIX_FMT_RGB24,
					 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws) {
		return QImage();
	}

	QImage img(yuv.width, yuv.height, QImage::Format_RGB888);

	uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
	int dst_linesize[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };

	const size_t y_size = static_cast<size_t>(yuv.width) * yuv.height;
	const size_t uv_size = static_cast<size_t>(yuv.width / 2) * (yuv.height / 2);

	const uint8_t *src[4] = {
		yuv.planes.data(),
		yuv.planes.data() + y_size,
		yuv.planes.data() + y_size + uv_size,
		nullptr,
	};
	int src_linesize[4] = { yuv.width, yuv.width / 2, yuv.width / 2, 0 };

	int out_h = sws_scale(sws, src, src_linesize, 0, yuv.height, dst, dst_linesize);
	sws_freeContext(sws);

	if (out_h <= 0) return QImage();
	return img;
}

} // namespace

// ============================================================================
// Canvas
// ============================================================================

class RelayPreviewCanvas : public QWidget {
public:
	explicit RelayPreviewCanvas(QWidget *parent = nullptr) : QWidget(parent)
	{
		setAttribute(Qt::WA_OpaquePaintEvent, true);
		setAutoFillBackground(false);
	}

	void setImage(const QImage &img)
	{
		m_image = img;
		m_has_image = !img.isNull();
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const QRect bounds = rect();

		if (m_has_image) {
			p.fillRect(bounds, QColor(0, 0, 0));
			QSize target = m_image.size();
			target.scale(bounds.size(), Qt::KeepAspectRatio);
			const int x = (bounds.width() - target.width()) / 2;
			const int y = (bounds.height() - target.height()) / 2;
			p.setRenderHint(QPainter::SmoothPixmapTransform, true);
			p.drawImage(QRect(x, y, target.width(), target.height()), m_image);
			return;
		}

		// Placeholder (unchanged from MVP-1)
		p.setRenderHint(QPainter::Antialiasing, true);
		p.fillRect(bounds, QColor(16, 18, 24));

		const qreal margin = 28.0;
		const QRectF stage = bounds.adjusted(margin, margin, -margin, -margin);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(28, 32, 44));
		p.drawRoundedRect(stage, 18, 18);

		p.setPen(QPen(QColor(122, 82, 214), 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(stage.adjusted(1, 1, -1, -1), 18, 18);

		const QRectF badge(stage.left() + 24, stage.top() + 22, 92, 28);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(122, 82, 214));
		p.drawRoundedRect(badge, 8, 8);
		p.setPen(Qt::white);
		p.setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold));
		p.drawText(badge, Qt::AlignCenter, QStringLiteral("MVP-2"));

		const QPointF center = stage.center();
		const qreal radius = 54.0;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(122, 82, 214));
		p.drawEllipse(center, radius, radius);

		QPolygonF play;
		play << QPointF(center.x() - 14, center.y() - 22)
		     << QPointF(center.x() - 14, center.y() + 22)
		     << QPointF(center.x() + 24, center.y());
		p.setBrush(Qt::white);
		p.drawPolygon(play);

		p.setPen(QColor(236, 239, 248));
		p.setFont(QFont(QStringLiteral("Segoe UI"), 20, QFont::Bold));
		p.drawText(QRectF(stage.left(), center.y() + 70, stage.width(), 36),
			   Qt::AlignHCenter | Qt::AlignTop,
			   QStringLiteral("Relay Preview"));

		p.setPen(QColor(164, 174, 194));
		p.setFont(QFont(QStringLiteral("Segoe UI"), 11));
		p.drawText(QRectF(stage.left(), center.y() + 108, stage.width(), 48),
			   Qt::AlignHCenter | Qt::AlignTop,
			   QStringLiteral("Waiting for first frame..."));
	}

private:
	QImage m_image;
	bool m_has_image = false;
};

// ============================================================================
// Window
// ============================================================================

RelayPreviewWindow::RelayPreviewWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("Relay Preview"));
	resize(kWindowWidth, kWindowHeight);
	setMinimumSize(480, 320);
	setAttribute(Qt::WA_DeleteOnClose, true);
	setWindowFlag(Qt::Window, true);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	auto *central = new QWidget(this);
	auto *outer = new QVBoxLayout(central);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	// --- status bar ---
	m_status_label = new QLabel(central);
	m_status_label->setStyleSheet(
		"background-color: #111; color: #00ff00; padding: 6px; "
		"font-family: monospace; font-size: 12px;");
	m_status_label->setText("Initializing...");
	m_status_label->setAlignment(Qt::AlignCenter);
	outer->addWidget(m_status_label);

	// --- canvas ---
	canvas = new RelayPreviewCanvas(central);
	outer->addWidget(canvas, 1);

	// --- controls row ---
	auto *controls = new QWidget(central);
	controls->setStyleSheet("background-color: #1a1d26;");
	auto *controls_layout = new QHBoxLayout(controls);
	controls_layout->setContentsMargins(10, 8, 10, 8);
	controls_layout->setSpacing(10);

	m_play_pause_btn = new QPushButton(QStringLiteral("Pause"), controls);
	m_play_pause_btn->setFixedWidth(80);
	controls_layout->addWidget(m_play_pause_btn);

	m_slider = new QSlider(Qt::Horizontal, controls);
	m_slider->setRange(-1000, 0);
	m_slider->setValue(-1000);
	controls_layout->addWidget(m_slider, 1);

	m_time_label = new QLabel(QStringLiteral("-0.0s"), controls);
	m_time_label->setStyleSheet("color: #cfd6e6; font-family: monospace; min-width: 70px;");
	m_time_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	controls_layout->addWidget(m_time_label);

	outer->addWidget(controls);

	setCentralWidget(central);

	// --- timers ---
	m_status_timer = new QTimer(this);
	QObject::connect(m_status_timer, &QTimer::timeout, this,
			 &RelayPreviewWindow::update_buffer_status);
	m_status_timer->start(250);

	m_play_timer = new QTimer(this);
	m_play_timer->setTimerType(Qt::PreciseTimer);
	QObject::connect(m_play_timer, &QTimer::timeout, this,
			 &RelayPreviewWindow::onPlayTick);

	// --- signals ---
	QObject::connect(m_play_pause_btn, &QPushButton::clicked,
			 this, &RelayPreviewWindow::onPlayPauseClicked);
	QObject::connect(m_slider, &QSlider::valueChanged,
			 this, &RelayPreviewWindow::onSliderValueChanged);
	QObject::connect(m_slider, &QSlider::sliderPressed,
			 this, &RelayPreviewWindow::onSliderPressed);
	QObject::connect(m_slider, &QSlider::sliderReleased,
			 this, &RelayPreviewWindow::onSliderReleased);
}

RelayPreviewWindow::~RelayPreviewWindow()
{
	if (m_session) {
		m_session->stop();
		delete m_session;
		m_session = nullptr;
	}
}

void RelayPreviewWindow::presentAndFocus()
{
	show();
	setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	raise();
	activateWindow();

#ifdef _WIN32
	HWND hwnd = reinterpret_cast<HWND>(winId());
	if (hwnd) {
		if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
		SetForegroundWindow(hwnd);
		BringWindowToTop(hwnd);
	}
#endif
}

void RelayPreviewWindow::showEvent(QShowEvent *event)
{
	QMainWindow::showEvent(event);
	detachFromObsMainWindow();

	if (!m_session) {
		setupSession();
	}
}

void RelayPreviewWindow::detachFromObsMainWindow()
{
#ifdef _WIN32
	HWND hwnd = reinterpret_cast<HWND>(winId());
	if (!hwnd) return;
	SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
#endif
}

void RelayPreviewWindow::setupSession()
{
	H264RingBuffer *buffer = get_global_ring_buffer();
	if (!buffer) {
		obs_log(LOG_WARNING, "[T3.4b] no ring buffer");
		m_status_label->setText("T3.4b: no ring buffer");
		return;
	}

	std::vector<CachedH264Packet> snapshot = buffer->get_snapshot();
	if (snapshot.empty()) {
		obs_log(LOG_WARNING, "[T3.4b] empty snapshot");
		m_status_label->setText("T3.4b: empty snapshot");
		return;
	}

	m_session = new PlaybackSession();
	if (!m_session->startFromSnapshot(std::move(snapshot), kTargetWidth, kTargetHeight)) {
		obs_log(LOG_ERROR, "[T3.4b] startFromSnapshot failed");
		delete m_session;
		m_session = nullptr;
		m_status_label->setText("T3.4b: session start failed");
		return;
	}

	m_duration_ms = static_cast<int64_t>(m_session->durationSec() * 1000.0);
	m_expected_frames = m_session->expectedFrameCount();

	if (m_duration_ms <= 0) m_duration_ms = 1;
	if (m_expected_frames <= 1) m_expected_frames = 1;

	// Slider: [-duration_ms, 0]
	m_slider->blockSignals(true);
	m_slider->setRange(static_cast<int>(-m_duration_ms), 0);
	m_slider->setValue(static_cast<int>(-m_duration_ms));
	m_slider->blockSignals(false);

	m_playhead_ms = -m_duration_ms;
	m_displayed_frame = SIZE_MAX;

	// Auto-play
	m_playing = true;
	m_elapsed.start();
	m_play_timer->start(kPlayTimerMs);
	updatePlayPauseButton();
	updateTimeLabel();

	obs_log(LOG_INFO,
		"[T3.4b] session ready: duration=%lld ms, expected_frames=%lld, autoplay=1",
		(long long)m_duration_ms, (long long)m_expected_frames);
}

// ---------------------------------------------------------------------------
// Playback tick
// ---------------------------------------------------------------------------

void RelayPreviewWindow::onPlayTick()
{
	if (!m_playing || !m_session) return;

	int64_t elapsed = m_elapsed.restart();
	if (elapsed < 0) elapsed = 0;

	int64_t new_playhead = m_playhead_ms + elapsed;

	const size_t total = m_session->frameCount();
	if (total == 0) {
		// Nothing decoded yet — hold the playhead at the start.
		return;
	}

	const size_t max_avail = total - 1;
	size_t wanted = playheadToFrameIndex(new_playhead);

	if (wanted > max_avail) {
		// Can't run ahead of decode. Clamp playhead to last decoded frame.
		wanted = max_avail;
		m_playhead_ms = frameIndexToPlayhead(max_avail);
	} else {
		m_playhead_ms = new_playhead;
	}

	if (m_playhead_ms >= 0) {
		m_playhead_ms = 0;
		m_playing = false;
		updatePlayPauseButton();
	}

	updateDisplay();
	updateSliderUI();
}

// ---------------------------------------------------------------------------
// Slider / seek
// ---------------------------------------------------------------------------

void RelayPreviewWindow::onSliderPressed()
{
	if (m_playing) {
		m_playing = false;
		m_resume_after_seek = true;
		updatePlayPauseButton();
	}
}

void RelayPreviewWindow::onSliderReleased()
{
	if (m_resume_after_seek) {
		m_resume_after_seek = false;
		// Don't resume if we're parked at the end.
		if (m_playhead_ms < 0) {
			m_playing = true;
			m_elapsed.restart();
			updatePlayPauseButton();
		}
	}
}

void RelayPreviewWindow::onSliderValueChanged(int value)
{
	m_playhead_ms = value;
	updateTimeLabel();
	updateDisplay();
}

void RelayPreviewWindow::onPlayPauseClicked()
{
	if (!m_session) return;

	if (m_playing) {
		m_playing = false;
	} else {
		// If parked at the end, rewind to start.
		if (m_playhead_ms >= 0) {
			m_playhead_ms = -m_duration_ms;
			m_displayed_frame = SIZE_MAX;
			updateDisplay();
			updateSliderUI();
		}
		m_playing = true;
		m_elapsed.restart();
	}
	updatePlayPauseButton();
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

void RelayPreviewWindow::updatePlayPauseButton()
{
	if (!m_play_pause_btn) return;
	m_play_pause_btn->setText(m_playing ? QStringLiteral("Pause")
					    : QStringLiteral("Play"));
}

void RelayPreviewWindow::updateTimeLabel()
{
	if (!m_time_label) return;
	double sec = m_playhead_ms / 1000.0;
	m_time_label->setText(QStringLiteral("%1s").arg(sec, 0, 'f', 1));
}

void RelayPreviewWindow::updateSliderUI()
{
	if (m_slider) {
		m_slider->blockSignals(true);
		m_slider->setValue(static_cast<int>(m_playhead_ms));
		m_slider->blockSignals(false);
	}
	updateTimeLabel();
}

void RelayPreviewWindow::updateDisplay()
{
	if (!m_session) return;

	const size_t total = m_session->frameCount();
	if (total == 0) return;

	size_t wanted = playheadToFrameIndex(m_playhead_ms);
	if (wanted >= total) wanted = total - 1;
	if (wanted == m_displayed_frame) return;

	YuvFrame frame;
	if (!m_session->getFrame(wanted, frame)) return;

	QImage img = yuvToQImage(frame);
	if (img.isNull()) return;

	canvas->setImage(img);
	m_displayed_frame = wanted;
}

size_t RelayPreviewWindow::playheadToFrameIndex(int64_t playhead_ms) const
{
	if (m_duration_ms <= 0 || m_expected_frames <= 1) return 0;

	double frac = static_cast<double>(playhead_ms + m_duration_ms) /
		      static_cast<double>(m_duration_ms);
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;

	return static_cast<size_t>(frac * static_cast<double>(m_expected_frames - 1));
}

int64_t RelayPreviewWindow::frameIndexToPlayhead(size_t idx) const
{
	if (m_duration_ms <= 0 || m_expected_frames <= 1) return 0;

	double frac = static_cast<double>(idx) /
		      static_cast<double>(m_expected_frames - 1);
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;

	return static_cast<int64_t>(frac * static_cast<double>(m_duration_ms)) -
	       m_duration_ms;
}

// ---------------------------------------------------------------------------
// Status label
// ---------------------------------------------------------------------------

void RelayPreviewWindow::update_buffer_status()
{
	auto *buffer = get_global_ring_buffer();
	if (!buffer) {
		m_status_label->setText("Buffer: NULL");
		return;
	}

	BufferStats stats = buffer->get_stats();

	if (!m_session) {
		if (stats.packet_count == 0) {
			m_status_label->setText("Buffer: 0 packets (waiting)");
		} else {
			m_status_label->setText(
				QString("Buffer: %1 pkts / %2 KB — session not started")
					.arg((qulonglong)stats.packet_count)
					.arg((qulonglong)(stats.total_bytes / 1024)));
		}
		return;
	}

	PlaybackProgress p = m_session->progress();

	const QString state = p.decode_failed
				      ? QStringLiteral("FAILED")
				      : (p.decoding_done ? QStringLiteral("ready")
							 : QStringLiteral("decoding…"));

	m_status_label->setText(
		QString("Buffer %1 pkts | Decoded %2 / %3 | %4")
			.arg((qulonglong)stats.packet_count)
			.arg((qulonglong)p.frames_decoded)
			.arg((qulonglong)p.total_packets)
			.arg(state));
}
