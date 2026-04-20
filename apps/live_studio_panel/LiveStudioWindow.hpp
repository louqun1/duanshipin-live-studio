#pragma once

#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include "capture/LiveDeviceCaptureSource.hpp"
#include "session/LiveSessionController.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStatusBar;

class LiveStudioWindow final : public QMainWindow {
public:
    explicit LiveStudioWindow(QWidget* parent = nullptr);
    ~LiveStudioWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class PanelState {
        idle,
        starting,
        publishing,
        stopping,
        failed,
    };

    enum class OperationKind {
        probe_devices,
        start_publish,
        stop_publish,
    };

    struct OperationResult {
        OperationKind kind = OperationKind::probe_devices;
        live_studio::Status status = live_studio::Status::Ok();
        std::optional<live_studio::capture::LiveDeviceProbeResult> probe;
    };

    struct RateSnapshot {
        qint64 sampleTimeMs = 0;
        int previewedVideoFrames = 0;
        int publishedAudioUnits = 0;
        double videoFps = 0.0;
        double audioPacketsPerSecond = 0.0;
    };

    void buildUi();
    void buildStatusBar();
    void beginDeviceProbe();
    void startOperation(std::function<OperationResult()> task);
    void handleOperationFinished();
    void handleProbeResult(const OperationResult& result);
    void handleStartPublishResult(const OperationResult& result);
    void handleStopPublishResult(const OperationResult& result);
    void updateButtonStates();
    void refreshPanel();
    void updatePreview();
    void renderPreviewImage(const QImage& image);
    void updateStatusSummary(const live_studio::session::SessionDiagnostics& diagnostics);
    void populateCaptureControls(const live_studio::capture::LiveDeviceProbeResult& probe);
    void setPanelState(PanelState state);

    [[nodiscard]] live_studio::session::LiveSessionConfig buildSessionConfig() const;
    [[nodiscard]] static QImage videoFrameToImage(const live_studio::media::VideoFrame& frame);
    [[nodiscard]] static QString panelStateText(PanelState state);
    [[nodiscard]] static QString formatDuration(qint64 elapsedMs);
    [[nodiscard]] static QString elideError(const std::string& errorText);
    [[nodiscard]] static int chooseVideoBitrate(int width, int height, int fps);

    void showInfoPrompt(const QString& text, bool modal = false) const;
    void showErrorPrompt(const QString& text) const;

    live_studio::session::LiveSessionController controller_;
    QFutureWatcher<OperationResult> operationWatcher_;
    QTimer refreshTimer_;
    QElapsedTimer publishElapsed_;
    PanelState panelState_ = PanelState::idle;
    RateSnapshot rateSnapshot_;
    QString lastErrorText_;
    QImage lastPreviewImage_;
    int lastPreviewFrameIndex_ = -1;
    bool deviceProbeReady_ = false;

    QLineEdit* rtmpUrlEdit_ = nullptr;
    QComboBox* videoDeviceCombo_ = nullptr;
    QComboBox* audioDeviceCombo_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* fpsCombo_ = nullptr;
    QComboBox* sampleRateCombo_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QLabel* statusSummaryLabel_ = nullptr;
};
