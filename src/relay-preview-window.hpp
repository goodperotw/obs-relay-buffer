/*
OBS Relay Buffer — MVP-2 playback window (Task 3.4b)
*/

#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>

#include <cstdint>

class RelayPreviewCanvas;
class PlaybackSession;
class QSlider;
class QPushButton;

class RelayPreviewWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit RelayPreviewWindow(QWidget *parent = nullptr);
	~RelayPreviewWindow() override;

	void presentAndFocus();

protected:
	void showEvent(QShowEvent *event) override;

private slots:
	void onPlayTick();
	void onSliderValueChanged(int value);
	void onSliderPressed();
	void onSliderReleased();
	void onPlayPauseClicked();
	void update_buffer_status();

private:
	void detachFromObsMainWindow();
	void setupSession();
	void updatePlayPauseButton();
	void updateTimeLabel();
	void updateSliderUI();
	void updateDisplay();

	// playhead (ms from end, range [-duration_ms, 0]) <-> frame index
	size_t playheadToFrameIndex(int64_t playhead_ms) const;
	int64_t frameIndexToPlayhead(size_t idx) const;

	RelayPreviewCanvas *canvas = nullptr;
	QLabel *m_status_label = nullptr;
	QSlider *m_slider = nullptr;
	QPushButton *m_play_pause_btn = nullptr;
	QLabel *m_time_label = nullptr;
	QTimer *m_status_timer = nullptr;
	QTimer *m_play_timer = nullptr;
	QElapsedTimer m_elapsed;

	PlaybackSession *m_session = nullptr;

	// Playback state
	bool m_playing = false;
	bool m_resume_after_seek = false;
	int64_t m_playhead_ms = 0;         // [-m_duration_ms, 0]
	size_t m_displayed_frame = SIZE_MAX;

	// UI mapping
	int64_t m_duration_ms = 0;
	int64_t m_expected_frames = 0;
};
