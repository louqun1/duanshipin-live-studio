#include "LiveStudioWindow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QVariant>

#include <QtConcurrent/QtConcurrentRun>

namespace {

constexpr int kRefreshIntervalMs = 200;

std::string toUtf8String(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

void ensureSizeOption(QComboBox* combo, const QSize& size) {
    if (combo == nullptr || !size.isValid()) {
        return;
    }

    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index).toSize() == size) {
            return;
        }
    }

    combo->addItem(QString("%1 x %2").arg(size.width()).arg(size.height()), size);
}

void ensureIntOption(QComboBox* combo, int value, const QString& suffix) {
    if (combo == nullptr || value <= 0) {
        return;
    }

    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index).toInt() == value) {
            return;
        }
    }

    combo->addItem(QString("%1%2").arg(value).arg(suffix), value);
}

void selectComboData(QComboBox* combo, const QVariant& desiredData) {
    if (combo == nullptr) {
        return;
    }

    for (int index = 0; index < combo->count(); ++index) {
        if (combo->itemData(index) == desiredData) {
            combo->setCurrentIndex(index);
            return;
        }
    }
}

int clampToByte(int value) {
    return std::clamp(value, 0, 255);
}

}  // namespace

LiveStudioWindow::LiveStudioWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    buildStatusBar();

    connect(&operationWatcher_, &QFutureWatcher<OperationResult>::finished, this, [this]() {
        handleOperationFinished();
    });
    connect(&refreshTimer_, &QTimer::timeout, this, [this]() {
        refreshPanel();
    });

    refreshTimer_.start(kRefreshIntervalMs);
    beginDeviceProbe();
}

LiveStudioWindow::~LiveStudioWindow() {
    refreshTimer_.stop();
    if (operationWatcher_.isRunning()) {
        operationWatcher_.waitForFinished();
    }
    controller_.close();
}

void LiveStudioWindow::closeEvent(QCloseEvent* event) {
    refreshTimer_.stop();
    if (operationWatcher_.isRunning()) {
        operationWatcher_.waitForFinished();
    }
    controller_.close();
    QMainWindow::closeEvent(event);
}

void LiveStudioWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    renderPreviewImage(lastPreviewImage_);
}

void LiveStudioWindow::buildUi() {
    setWindowTitle("Live Studio Panel");
    resize(1280, 780);
    setMinimumSize(1080, 700);
    setStyleSheet(R"(
        QMainWindow {
            background: #f2ece2;
        }
        QGroupBox {
            background: #fff9f1;
            border: 1px solid #d9cdbd;
            border-radius: 10px;
            font-weight: 600;
            margin-top: 14px;
            padding-top: 10px;
            color: #423327;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
        }
        QLabel {
            color: #43372b;
        }
        QLineEdit, QComboBox {
            background: #fffdf8;
            border: 1px solid #cdbca7;
            border-radius: 8px;
            padding: 6px 8px;
            color: #2d241c;
        }
        QPushButton {
            border: 0;
            border-radius: 8px;
            padding: 9px 16px;
            font-weight: 600;
            color: #ffffff;
            background: #7c8f7b;
        }
        QPushButton#primaryButton {
            background: #2d6b60;
        }
        QPushButton#dangerButton {
            background: #ad4f37;
        }
        QPushButton:disabled {
            background: #b7b0a7;
            color: #f6f1eb;
        }
        QFrame#previewSurface {
            background: #171412;
            border: 1px solid #3a3026;
            border-radius: 12px;
        }
        QStatusBar {
            background: #e7d9c5;
            color: #43372b;
        }
    )");

    QFont appFont("Microsoft YaHei UI", 10);
    setFont(appFont);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(16, 16, 16, 12);
    rootLayout->setSpacing(14);

    auto* controlsGroup = new QGroupBox("最小可用推流面板", central);
    auto* controlsLayout = new QGridLayout(controlsGroup);
    controlsLayout->setHorizontalSpacing(12);
    controlsLayout->setVerticalSpacing(10);

    auto* rtmpLabel = new QLabel("推流地址", controlsGroup);
    rtmpUrlEdit_ = new QLineEdit(controlsGroup);
    rtmpUrlEdit_->setPlaceholderText("rtmp://192.168.99.128/live/livestream");
    rtmpUrlEdit_->setText("rtmp://192.168.99.128/live/livestream");

    startButton_ = new QPushButton("开始推流", controlsGroup);
    startButton_->setObjectName("primaryButton");
    stopButton_ = new QPushButton("停止推流", controlsGroup);
    stopButton_->setObjectName("dangerButton");

    auto* videoLabel = new QLabel("视频设备", controlsGroup);
    videoDeviceCombo_ = new QComboBox(controlsGroup);
    auto* audioLabel = new QLabel("音频设备", controlsGroup);
    audioDeviceCombo_ = new QComboBox(controlsGroup);
    auto* resolutionLabel = new QLabel("分辨率", controlsGroup);
    resolutionCombo_ = new QComboBox(controlsGroup);
    auto* fpsLabel = new QLabel("FPS", controlsGroup);
    fpsCombo_ = new QComboBox(controlsGroup);
    auto* sampleRateLabel = new QLabel("采样率", controlsGroup);
    sampleRateCombo_ = new QComboBox(controlsGroup);

    controlsLayout->addWidget(rtmpLabel, 0, 0);
    controlsLayout->addWidget(rtmpUrlEdit_, 0, 1, 1, 5);
    controlsLayout->addWidget(startButton_, 0, 6);
    controlsLayout->addWidget(stopButton_, 0, 7);

    controlsLayout->addWidget(videoLabel, 1, 0);
    controlsLayout->addWidget(videoDeviceCombo_, 1, 1);
    controlsLayout->addWidget(audioLabel, 1, 2);
    controlsLayout->addWidget(audioDeviceCombo_, 1, 3);
    controlsLayout->addWidget(resolutionLabel, 1, 4);
    controlsLayout->addWidget(resolutionCombo_, 1, 5);
    controlsLayout->addWidget(fpsLabel, 1, 6);
    controlsLayout->addWidget(fpsCombo_, 1, 7);
    controlsLayout->addWidget(sampleRateLabel, 2, 0);
    controlsLayout->addWidget(sampleRateCombo_, 2, 1);

    auto* splitter = new QSplitter(Qt::Horizontal, central);

    auto* previewGroup = new QGroupBox("本地预览", splitter);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(12, 18, 12, 12);
    previewLabel_ = new QLabel(previewGroup);
    previewLabel_->setObjectName("previewSurface");
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumSize(640, 360);
    previewLabel_->setWordWrap(true);
    previewLabel_->setText("等待本地预览\n点击“开始推流”后开始采集并刷新画面");
    previewLayout->addWidget(previewLabel_);

    auto* logGroup = new QGroupBox("日志窗口", splitter);
    logGroup->hide();
    splitter->setStretchFactor(0, 1);

    rootLayout->addWidget(controlsGroup);
    rootLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    connect(startButton_, &QPushButton::clicked, this, [this]() {
        if (operationWatcher_.isRunning() || !deviceProbeReady_) {
            return;
        }

        const QString rtmpUrl = rtmpUrlEdit_->text().trimmed();
        if (rtmpUrl.isEmpty()) {
            setPanelState(PanelState::failed);
            showErrorPrompt("RTMP URL cannot be empty.");
            return;
        }

        setPanelState(PanelState::starting);
        statusBar()->showMessage("Starting preview and publish...", 3000);
        startOperation([this]() {
            OperationResult result;
            result.kind = OperationKind::start_publish;

            const live_studio::session::LiveSessionConfig config = buildSessionConfig();
            controller_.close();

            result.status = controller_.open(config);
            if (!result.status.ok()) {
                controller_.close();
                return result;
            }

            result.status = controller_.startPreview();
            if (!result.status.ok()) {
                controller_.close();
                return result;
            }

            result.status = controller_.startPublish();
            return result;
        });
    });

    connect(stopButton_, &QPushButton::clicked, this, [this]() {
        if (operationWatcher_.isRunning()) {
            return;
        }

        setPanelState(PanelState::stopping);
        statusBar()->showMessage("Stopping publish...", 3000);
        startOperation([this]() {
            OperationResult result;
            result.kind = OperationKind::stop_publish;
            result.status = controller_.stopPublish();
            return result;
        });
    });

    updateButtonStates();
}

void LiveStudioWindow::buildStatusBar() {
    statusSummaryLabel_ = new QLabel(this);
    statusSummaryLabel_->setMargin(2);
    statusBar()->addPermanentWidget(statusSummaryLabel_, 1);
    updateStatusSummary(controller_.diagnostics());
}

void LiveStudioWindow::beginDeviceProbe() {
    statusBar()->showMessage("Probing live devices...", 3000);
    startOperation([]() {
        OperationResult result;
        result.kind = OperationKind::probe_devices;

        live_studio::media::LiveDeviceCaptureConfig config;
        config.sourceLabel = "live-studio-panel-probe";
        config.videoDeviceId = "default";
        config.width = 1280;
        config.height = 720;
        config.fps = 30;
        config.sampleRate = 48000;
        config.channels = 2;
        config.frameCount = 1;

        live_studio::capture::LiveDeviceProbeResult probe;
        result.status = live_studio::capture::probeLiveDeviceCaptureConfig(config, &probe);
        if (result.status.ok()) {
            result.probe = std::move(probe);
        }
        return result;
    });
}

void LiveStudioWindow::startOperation(std::function<OperationResult()> task) {
    if (operationWatcher_.isRunning()) {
        return;
    }

    updateButtonStates();
    operationWatcher_.setFuture(QtConcurrent::run([task = std::move(task)]() mutable {
        return task();
    }));
}

void LiveStudioWindow::handleOperationFinished() {
    const OperationResult result = operationWatcher_.result();
    switch (result.kind) {
    case OperationKind::probe_devices:
        handleProbeResult(result);
        break;
    case OperationKind::start_publish:
        handleStartPublishResult(result);
        break;
    case OperationKind::stop_publish:
        handleStopPublishResult(result);
        break;
    }

    updateButtonStates();
    updateStatusSummary(controller_.diagnostics());
}

void LiveStudioWindow::handleProbeResult(const OperationResult& result) {
    if (!result.status.ok() || !result.probe.has_value()) {
        deviceProbeReady_ = false;
        setPanelState(PanelState::failed);
        showErrorPrompt(QString("Device probe failed: %1").arg(QString::fromStdString(result.status.message)));
        return;
    }

    deviceProbeReady_ = true;
    populateCaptureControls(*result.probe);
    setPanelState(PanelState::idle);
    showInfoPrompt(QString("Device probe ready: %1 camera(s), %2 microphone(s).")
                       .arg(result.probe->availableVideoDeviceNames.size())
                       .arg(result.probe->availableAudioDeviceNames.size()));
}

void LiveStudioWindow::handleStartPublishResult(const OperationResult& result) {
    if (!result.status.ok()) {
        publishElapsed_.invalidate();
        setPanelState(PanelState::failed);
        showErrorPrompt(QString("Start publish failed: %1").arg(QString::fromStdString(result.status.message)));
        return;
    }

    publishElapsed_.restart();
    setPanelState(PanelState::publishing);
    showInfoPrompt("Connected successfully. Publishing is live.", true);
}

void LiveStudioWindow::handleStopPublishResult(const OperationResult& result) {
    if (!result.status.ok()) {
        setPanelState(PanelState::failed);
        showErrorPrompt(QString("Stop publish failed: %1").arg(QString::fromStdString(result.status.message)));
        return;
    }

    publishElapsed_.invalidate();
    setPanelState(PanelState::idle);
    showInfoPrompt("Publish stopped. Local preview is still running.");
}

void LiveStudioWindow::updateButtonStates() {
    const bool busy = operationWatcher_.isRunning();
    const live_studio::session::SessionDiagnostics diagnostics = controller_.diagnostics();
    const bool publishing = diagnostics.publishing;
    const bool hasVideoSelection =
        videoDeviceCombo_ != nullptr && !videoDeviceCombo_->currentData().toString().trimmed().isEmpty();
    const bool hasRtmpUrl = rtmpUrlEdit_ != nullptr && !rtmpUrlEdit_->text().trimmed().isEmpty();

    if (rtmpUrlEdit_ != nullptr) {
        rtmpUrlEdit_->setEnabled(!busy && !publishing);
    }
    if (videoDeviceCombo_ != nullptr) {
        videoDeviceCombo_->setEnabled(!busy && !publishing && deviceProbeReady_);
    }
    if (audioDeviceCombo_ != nullptr) {
        audioDeviceCombo_->setEnabled(!busy && !publishing && deviceProbeReady_);
    }
    if (resolutionCombo_ != nullptr) {
        resolutionCombo_->setEnabled(!busy && !publishing && deviceProbeReady_);
    }
    if (fpsCombo_ != nullptr) {
        fpsCombo_->setEnabled(!busy && !publishing && deviceProbeReady_);
    }
    if (sampleRateCombo_ != nullptr) {
        sampleRateCombo_->setEnabled(!busy && !publishing && deviceProbeReady_);
    }
    if (startButton_ != nullptr) {
        startButton_->setEnabled(deviceProbeReady_ && !busy && !publishing && hasVideoSelection && hasRtmpUrl);
    }
    if (stopButton_ != nullptr) {
        stopButton_->setEnabled(!busy && publishing);
    }
}

void LiveStudioWindow::refreshPanel() {
    updatePreview();

    const live_studio::session::SessionDiagnostics diagnostics = controller_.diagnostics();
    const QString currentError = QString::fromStdString(diagnostics.lastError);
    if (!currentError.isEmpty() && currentError != lastErrorText_) {
        showErrorPrompt(QString("Session error: %1").arg(currentError));
    }
    lastErrorText_ = currentError;

    if (!operationWatcher_.isRunning()) {
        if (diagnostics.publishing) {
            if (panelState_ != PanelState::publishing) {
                setPanelState(PanelState::publishing);
            }
            if (!publishElapsed_.isValid()) {
                publishElapsed_.start();
            }
        } else if (!diagnostics.lastError.empty()) {
            publishElapsed_.invalidate();
            setPanelState(PanelState::failed);
        } else if (panelState_ == PanelState::publishing || panelState_ == PanelState::stopping) {
            publishElapsed_.invalidate();
            setPanelState(PanelState::idle);
        }
    }

    updateStatusSummary(diagnostics);
    updateButtonStates();
}

void LiveStudioWindow::updatePreview() {
    const live_studio::session::PreviewFrameSnapshot snapshot = controller_.latestPreviewFrame();
    if (!snapshot.available || !snapshot.frame.hasPixelPayload || snapshot.frame.frameIndex == lastPreviewFrameIndex_) {
        return;
    }

    QImage converted = videoFrameToImage(snapshot.frame);
    if (converted.isNull()) {
        return;
    }

    lastPreviewFrameIndex_ = snapshot.frame.frameIndex;
    lastPreviewImage_ = std::move(converted);
    renderPreviewImage(lastPreviewImage_);
}

void LiveStudioWindow::renderPreviewImage(const QImage& image) {
    if (previewLabel_ == nullptr || image.isNull()) {
        return;
    }

    const QSize targetSize = previewLabel_->contentsRect().size();
    const QPixmap sourcePixmap = QPixmap::fromImage(image);
    if (!targetSize.isValid()) {
        previewLabel_->setPixmap(sourcePixmap);
        previewLabel_->setText(QString());
        return;
    }

    previewLabel_->setPixmap(
        sourcePixmap.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    previewLabel_->setText(QString());
}

void LiveStudioWindow::updateStatusSummary(const live_studio::session::SessionDiagnostics& diagnostics) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (rateSnapshot_.sampleTimeMs == 0) {
        rateSnapshot_.sampleTimeMs = nowMs;
        rateSnapshot_.previewedVideoFrames = diagnostics.previewedVideoFrames;
        rateSnapshot_.publishedAudioUnits = diagnostics.publishedAudioUnits;
    } else {
        const qint64 elapsedMs = nowMs - rateSnapshot_.sampleTimeMs;
        if (elapsedMs >= 500) {
            const int videoDelta = diagnostics.previewedVideoFrames - rateSnapshot_.previewedVideoFrames;
            const int audioDelta = diagnostics.publishedAudioUnits - rateSnapshot_.publishedAudioUnits;

            rateSnapshot_.videoFps = std::max(0.0, static_cast<double>(videoDelta) * 1000.0 /
                                                       static_cast<double>(elapsedMs));
            rateSnapshot_.audioPacketsPerSecond =
                diagnostics.publishing ? std::max(0.0, static_cast<double>(audioDelta) * 1000.0 /
                                                           static_cast<double>(elapsedMs))
                                       : 0.0;
            rateSnapshot_.sampleTimeMs = nowMs;
            rateSnapshot_.previewedVideoFrames = diagnostics.previewedVideoFrames;
            rateSnapshot_.publishedAudioUnits = diagnostics.publishedAudioUnits;
        }
    }

    if (diagnostics.publishing && !publishElapsed_.isValid()) {
        publishElapsed_.start();
    }
    if (!diagnostics.publishing && panelState_ != PanelState::starting) {
        publishElapsed_.invalidate();
    }

    const QString durationText =
        diagnostics.publishing && publishElapsed_.isValid() ? formatDuration(publishElapsed_.elapsed()) : "--";
    const QString firstPacketText = diagnostics.firstMediaPacketSent
                                        ? QString("%1 ms").arg(diagnostics.firstMediaLatencyMs, 0, 'f', 1)
                                        : "--";
    const QString errorText = diagnostics.lastError.empty() ? "none" : elideError(diagnostics.lastError);

    statusSummaryLabel_->setText(
        QString("当前状态：%1 | 视频帧率：%2 fps | 音频包率：%3/s | 推流时长：%4 | 首包延迟：%5 | 最近错误：%6")
            .arg(panelStateText(panelState_))
            .arg(rateSnapshot_.videoFps, 0, 'f', 1)
            .arg(rateSnapshot_.audioPacketsPerSecond, 0, 'f', 1)
            .arg(durationText)
            .arg(firstPacketText)
            .arg(errorText));
    statusSummaryLabel_->setToolTip(QString::fromStdString(diagnostics.lastError));
}

void LiveStudioWindow::populateCaptureControls(const live_studio::capture::LiveDeviceProbeResult& probe) {
    const QSignalBlocker blockVideo(videoDeviceCombo_);
    const QSignalBlocker blockAudio(audioDeviceCombo_);
    const QSignalBlocker blockResolution(resolutionCombo_);
    const QSignalBlocker blockFps(fpsCombo_);
    const QSignalBlocker blockSampleRate(sampleRateCombo_);

    videoDeviceCombo_->clear();
    videoDeviceCombo_->addItem("default (首个摄像头)", "default");
    for (const std::string& deviceName : probe.availableVideoDeviceNames) {
        const QString name = QString::fromStdString(deviceName);
        videoDeviceCombo_->addItem(name, name);
    }
    selectComboData(videoDeviceCombo_, "default");

    audioDeviceCombo_->clear();
    audioDeviceCombo_->addItem("none (静音推流)", "");
    audioDeviceCombo_->addItem("default (系统默认)", "default");
    for (const std::string& deviceName : probe.availableAudioDeviceNames) {
        const QString name = QString::fromStdString(deviceName);
        audioDeviceCombo_->addItem(name, name);
    }
    if (!probe.availableAudioDeviceNames.empty()) {
        selectComboData(audioDeviceCombo_, "default");
    } else {
        selectComboData(audioDeviceCombo_, "");
    }

    resolutionCombo_->clear();
    const std::array<QSize, 4> resolutions = {
        QSize(640, 480),
        QSize(960, 540),
        QSize(1280, 720),
        QSize(1920, 1080),
    };
    for (const QSize& size : resolutions) {
        resolutionCombo_->addItem(QString("%1 x %2").arg(size.width()).arg(size.height()), size);
    }
    const QSize negotiatedSize(probe.negotiatedVideoWidth, probe.negotiatedVideoHeight);
    ensureSizeOption(resolutionCombo_, negotiatedSize);
    selectComboData(resolutionCombo_, negotiatedSize.isValid() ? QVariant(negotiatedSize) : QVariant(QSize(1280, 720)));

    fpsCombo_->clear();
    const std::array<int, 4> fpsOptions = {15, 24, 30, 60};
    for (int fps : fpsOptions) {
        fpsCombo_->addItem(QString("%1 fps").arg(fps), fps);
    }
    ensureIntOption(fpsCombo_, probe.negotiatedVideoFps, " fps");
    selectComboData(fpsCombo_, probe.negotiatedVideoFps > 0 ? QVariant(probe.negotiatedVideoFps) : QVariant(30));

    sampleRateCombo_->clear();
    const std::array<int, 3> sampleRates = {32000, 44100, 48000};
    for (int sampleRate : sampleRates) {
        sampleRateCombo_->addItem(QString("%1 Hz").arg(sampleRate), sampleRate);
    }
    ensureIntOption(sampleRateCombo_, probe.negotiatedSampleRate, " Hz");
    selectComboData(sampleRateCombo_,
                    probe.negotiatedSampleRate > 0 ? QVariant(probe.negotiatedSampleRate) : QVariant(48000));
}

void LiveStudioWindow::setPanelState(PanelState state) {
    panelState_ = state;
}

live_studio::session::LiveSessionConfig LiveStudioWindow::buildSessionConfig() const {
    live_studio::session::LiveSessionConfig config;
    const QSize resolution = resolutionCombo_->currentData().toSize();
    const int fps = fpsCombo_->currentData().toInt();
    const int sampleRate = sampleRateCombo_->currentData().toInt();
    const std::string audioDeviceId = toUtf8String(audioDeviceCombo_->currentData().toString());

    config.rtmpUrl = toUtf8String(rtmpUrlEdit_->text().trimmed());
    config.capture.sourceLabel = "live-studio-panel";
    config.capture.videoDeviceId = toUtf8String(videoDeviceCombo_->currentData().toString());
    config.capture.audioDeviceId = audioDeviceId;
    config.capture.width = resolution.width();
    config.capture.height = resolution.height();
    config.capture.fps = fps > 0 ? fps : 30;
    config.capture.sampleRate = sampleRate > 0 ? sampleRate : 48000;
    config.capture.channels = audioDeviceId.empty() ? 0 : 2;
    config.capture.frameCount = 1;
    config.videoBitrate = chooseVideoBitrate(config.capture.width, config.capture.height, config.capture.fps);
    config.audioBitrate = audioDeviceId.empty() ? 0 : 128000;
    return config;
}

QImage LiveStudioWindow::videoFrameToImage(const live_studio::media::VideoFrame& frame) {
    if (!frame.hasPixelPayload || frame.pixelFormat != live_studio::media::VideoPixelFormat::yuv420p ||
        frame.width <= 0 || frame.height <= 0 || frame.lineStrideY <= 0 || frame.lineStrideU <= 0 ||
        frame.lineStrideV <= 0) {
        return {};
    }

    const std::size_t yPlaneSize = static_cast<std::size_t>(frame.lineStrideY) * static_cast<std::size_t>(frame.height);
    const std::size_t uvHeight = static_cast<std::size_t>((frame.height + 1) / 2);
    const std::size_t uPlaneSize = static_cast<std::size_t>(frame.lineStrideU) * uvHeight;
    const std::size_t vPlaneSize = static_cast<std::size_t>(frame.lineStrideV) * uvHeight;
    if (frame.pixelPayload.size() < yPlaneSize + uPlaneSize + vPlaneSize) {
        return {};
    }

    const auto* yPlane = frame.pixelPayload.data();
    const auto* uPlane = yPlane + yPlaneSize;
    const auto* vPlane = uPlane + uPlaneSize;

    QImage image(frame.width, frame.height, QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }

    for (int row = 0; row < frame.height; ++row) {
        auto* destination = reinterpret_cast<QRgb*>(image.scanLine(row));
        const int uvRow = row / 2;
        for (int column = 0; column < frame.width; ++column) {
            const int uvColumn = column / 2;

            const int yValue = static_cast<int>(yPlane[row * frame.lineStrideY + column]);
            const int uValue = static_cast<int>(uPlane[uvRow * frame.lineStrideU + uvColumn]);
            const int vValue = static_cast<int>(vPlane[uvRow * frame.lineStrideV + uvColumn]);

            const int c = std::max(0, yValue - 16);
            const int d = uValue - 128;
            const int e = vValue - 128;
            const int red = clampToByte((298 * c + 409 * e + 128) >> 8);
            const int green = clampToByte((298 * c - 100 * d - 208 * e + 128) >> 8);
            const int blue = clampToByte((298 * c + 516 * d + 128) >> 8);
            destination[column] = qRgb(red, green, blue);
        }
    }

    return image;
}

QString LiveStudioWindow::panelStateText(PanelState state) {
    switch (state) {
    case PanelState::idle:
        return "idle";
    case PanelState::starting:
        return "starting";
    case PanelState::publishing:
        return "publishing";
    case PanelState::stopping:
        return "stopping";
    case PanelState::failed:
        return "failed";
    }
    return "idle";
}

QString LiveStudioWindow::formatDuration(qint64 elapsedMs) {
    if (elapsedMs < 0) {
        return "--";
    }

    const qint64 totalSeconds = elapsedMs / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString LiveStudioWindow::elideError(const std::string& errorText) {
    const QString text = QString::fromStdString(errorText);
    constexpr int kMaxCharacters = 80;
    if (text.size() <= kMaxCharacters) {
        return text;
    }
    return text.left(kMaxCharacters - 1) + "…";
}

int LiveStudioWindow::chooseVideoBitrate(int width, int height, int fps) {
    const int pixelRate = width * height * std::max(fps, 1);
    if (pixelRate >= 1920 * 1080 * 50) {
        return 6000000;
    }
    if (pixelRate >= 1280 * 720 * 30) {
        return 3500000;
    }
    return 2000000;
}

void LiveStudioWindow::showInfoPrompt(const QString& text, bool modal) const {
    if (statusBar() != nullptr) {
        statusBar()->showMessage(text, 5000);
    }
    if (modal) {
        QMessageBox::information(const_cast<LiveStudioWindow*>(this), "Publish Message", text);
    }
}

void LiveStudioWindow::showErrorPrompt(const QString& text) const {
    if (statusBar() != nullptr) {
        statusBar()->showMessage(text, 7000);
    }
    QMessageBox::warning(const_cast<LiveStudioWindow*>(this), "Publish Error", text);
}
