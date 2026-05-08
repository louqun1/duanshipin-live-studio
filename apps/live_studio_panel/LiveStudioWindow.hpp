#pragma once

#include <functional>
#include <optional>

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include "BusinessLiveTypes.hpp"
#include "capture/LiveDeviceCaptureSource.hpp"
#include "linkmic/LinkMicSession.hpp"
#include "rtc/RtcPeerSession.hpp"
#include "rtc/RtcSignalingBridge.hpp"
#include "session/LiveSessionController.hpp"

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QWidget;

namespace live_studio::business {
class BusinessLiveClient;
}

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
    QWidget* buildSetupPage(QWidget* parent);
    QWidget* buildLiveRoomPage(QWidget* parent);
    QGroupBox* buildBusinessConfigGroup(QWidget* parent);
    QGroupBox* buildPublishConfigGroup(QWidget* parent);
    QGroupBox* buildSignalingConfigGroup(QWidget* parent);
    QWidget* buildLiveRoomSidebar(QWidget* parent);
    void buildStatusBar();
    void beginDeviceProbe();
    void startOperation(std::function<OperationResult()> task);
    void handleOperationFinished();
    void handleProbeResult(const OperationResult& result);
    void handleStartPublishResult(const OperationResult& result);
    void handleStopPublishResult(const OperationResult& result);
    void handleBusinessStateChanged();
    void handleBusinessReadyToPublish();
    void updateButtonStates();
    void refreshPanel();
    void refreshLiveRoomInfo(const live_studio::session::SessionDiagnostics& diagnostics);
    void refreshViewerInfo();
    void updatePreview();
    void renderPreviewImage(const QImage& image);
    void updateStatusSummary(const live_studio::session::SessionDiagnostics& diagnostics);
    void refreshBusinessStatus();
    void populateCaptureControls(const live_studio::capture::LiveDeviceProbeResult& probe);
    void setPanelState(PanelState state);
    void switchToSetupPage();
    void switchToLiveRoomPage();
    void syncLinkMicTransportFromBusiness();
    void handleBusinessSignalingMessage(const QJsonObject& message);
    void ensureRtcSessionForJoinParams();
    void stopRtcSession(const QString& reason);

    void refreshLinkMicPanel();
    void populateAudienceMemberTable();
    void populateLinkMicMemberTable();
    void renderLinkMicEventLog();
    void updateLinkMicConnectionControls();
    void updateLinkMicActionButtons();

    [[nodiscard]] live_studio::business::BusinessStartConfig buildBusinessStartConfig() const;
    [[nodiscard]] live_studio::session::LiveSessionConfig buildSessionConfig() const;
    [[nodiscard]] live_studio::linkmic::LinkMicSessionConfig buildBusinessLinkMicSessionConfig() const;
    [[nodiscard]] live_studio::linkmic::CrossRoomInviteTarget buildInviteTarget() const;
    [[nodiscard]] static QImage videoFrameToImage(const live_studio::media::VideoFrame& frame);
    [[nodiscard]] static QString panelStateText(PanelState state);
    [[nodiscard]] static QString liveRoomStatusText(PanelState state, bool publishing);
    [[nodiscard]] static QString businessDisplayStateText(
        const live_studio::business::BusinessLiveSnapshot& snapshot,
        const live_studio::session::SessionDiagnostics& diagnostics,
        PanelState panelState);
    [[nodiscard]] static QString formatDuration(qint64 elapsedMs);
    [[nodiscard]] static QString elideError(const std::string& errorText);
    [[nodiscard]] static QString formatLinkMicEvent(const live_studio::linkmic::LinkMicEventLogEntry& entry);
    [[nodiscard]] static int chooseVideoBitrate(int width, int height, int fps);

    void showInfoPrompt(const QString& text, bool modal = false) const;
    void showErrorPrompt(const QString& text) const;

    live_studio::session::LiveSessionController controller_;
    live_studio::linkmic::LinkMicSession linkMicSession_;
    live_studio::rtc::RtcSignalingBridge rtcSignalingBridge_;
    live_studio::rtc::RtcPeerSession rtcPeerSession_;
    live_studio::business::BusinessLiveClient* businessClient_ = nullptr;
    QFutureWatcher<OperationResult> operationWatcher_;
    QTimer refreshTimer_;
    QElapsedTimer publishElapsed_;
    PanelState panelState_ = PanelState::idle;
    RateSnapshot rateSnapshot_;
    QString lastErrorText_;
    QImage lastPreviewImage_;
    int lastPreviewFrameIndex_ = -1;
    bool deviceProbeReady_ = false;
    live_studio::business::BusinessLiveSnapshot businessSnapshot_;

    QStackedWidget* pageStack_ = nullptr;
    QWidget* setupPage_ = nullptr;
    QWidget* liveRoomPage_ = nullptr;

    QLineEdit* apiBaseUrlEdit_ = nullptr;
    QLineEdit* usernameEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLineEdit* businessRoomKeyEdit_ = nullptr;
    QLineEdit* businessTitleEdit_ = nullptr;
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
    QLabel* setupRoomStatusValueLabel_ = nullptr;
    QLabel* setupRoomOnlineCountValueLabel_ = nullptr;
    QLabel* setupRoomHintValueLabel_ = nullptr;
    QLabel* liveRoomIdValueLabel_ = nullptr;
    QLabel* liveRoomStatusValueLabel_ = nullptr;
    QLabel* liveRoomOnlineCountValueLabel_ = nullptr;
    QLabel* liveRoomDurationValueLabel_ = nullptr;
    QLabel* liveRoomOwnerValueLabel_ = nullptr;
    QLabel* businessWsStateValueLabel_ = nullptr;
    QLabel* businessSignalingValueLabel_ = nullptr;
    QLabel* businessIssueValueLabel_ = nullptr;
    QLabel* viewerOnlineCountValueLabel_ = nullptr;
    QTableWidget* audienceMemberTable_ = nullptr;

    QLineEdit* linkMicTargetRoomKeyEdit_ = nullptr;
    QLineEdit* linkMicTargetUserIdEdit_ = nullptr;
    QLabel* linkMicTransportValueLabel_ = nullptr;
    QLabel* linkMicStateValueLabel_ = nullptr;
    QLabel* linkMicSessionValueLabel_ = nullptr;
    QLabel* linkMicRequestValueLabel_ = nullptr;
    QLabel* linkMicBusinessRoleValueLabel_ = nullptr;
    QLabel* linkMicRtcRoleValueLabel_ = nullptr;
    QLabel* linkMicRemoteValueLabel_ = nullptr;
    QLabel* linkMicOnlineValueLabel_ = nullptr;
    QLabel* linkMicRtcCountValueLabel_ = nullptr;
    QLabel* linkMicJoinParamsValueLabel_ = nullptr;
    QLabel* linkMicErrorValueLabel_ = nullptr;
    QLabel* rtcPeerStateValueLabel_ = nullptr;
    QLabel* rtcDataChannelValueLabel_ = nullptr;
    QLabel* rtcPingValueLabel_ = nullptr;
    QTableWidget* linkMicMemberTable_ = nullptr;
    QPushButton* linkMicInviteButton_ = nullptr;
    QPushButton* linkMicAcceptButton_ = nullptr;
    QPushButton* linkMicRejectButton_ = nullptr;
    QPushButton* linkMicHangupButton_ = nullptr;
    QPlainTextEdit* linkMicEventLogEdit_ = nullptr;
};
