#include "LiveStudioWindow.hpp"

#include "BusinessLiveClient.hpp"

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
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStringList>
#include <QUrl>
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

QString fallbackText(const QString& value, const QString& fallback = "--") {
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? fallback : trimmed;
}

QString roleSummary(const live_studio::linkmic::RoomMember& member) {
    QStringList parts;
    if (!member.role.trimmed().isEmpty()) {
        parts.push_back(member.role);
    }
    parts.push_back(member.online ? "online" : "offline");
    parts.push_back(member.inLinkMic ? "in-linkmic" : "idle");
    return parts.join(" | ");
}

QString businessMemberSummary(const live_studio::business::BusinessRoomMember& member) {
    QStringList parts;
    if (!member.role.trimmed().isEmpty()) {
        parts.push_back(member.role);
    }
    parts.push_back(member.online ? "online" : "offline");
    return parts.join(" | ");
}

bool isAudienceMember(const live_studio::business::BusinessLiveSnapshot& snapshot,
                      const live_studio::business::BusinessRoomMember& member) {
    return member.userId > 0 && (snapshot.userId <= 0 || member.userId != snapshot.userId);
}

int rtcLinkMicMemberCount(const live_studio::linkmic::RoomMemberDirectory& memberDirectory) {
    int count = 0;
    for (const auto& member : memberDirectory.members()) {
        if (member.inLinkMic) {
            ++count;
        }
    }
    return count;
}

QLabel* makeCardValueLabel(QWidget* parent, const QString& text = "--") {
    auto* label = new QLabel(text, parent);
    label->setObjectName("cardValue");
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

}  // namespace

LiveStudioWindow::LiveStudioWindow(QWidget* parent) : QMainWindow(parent) {
    businessClient_ = new live_studio::business::BusinessLiveClient(this);
    buildUi();
    buildStatusBar();
    businessSnapshot_ = businessClient_->snapshot();

    linkMicSession_.setUpdateHandler([this]() {
        refreshLinkMicPanel();
    });
    linkMicSession_.setExternalMessageSender([this](const QJsonObject& envelope) {
        return businessClient_ != nullptr && businessClient_->sendSignalingEnvelope(envelope);
    });
    linkMicSession_.setUseExternalTransport(true);

    live_studio::rtc::RtcPeerSession::Callbacks rtcCallbacks;
    rtcCallbacks.onLocalDescription = [this](const QString& sdp, const QString& descriptionType) {
        rtcSignalingBridge_.sendLocalDescription(sdp, descriptionType);
    };
    rtcCallbacks.onLocalIceCandidate = [this](const QString& candidate, const QString& mid) {
        rtcSignalingBridge_.sendLocalIceCandidate(candidate, mid);
    };
    rtcCallbacks.onPeerConnectionConnected = [this]() {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                linkMicSession_.noteRtcConnected();
                refreshLinkMicPanel();
            },
            Qt::QueuedConnection);
    };
    rtcPeerSession_.setCallbacks(std::move(rtcCallbacks));

    live_studio::rtc::RtcSignalingBridge::Callbacks bridgeCallbacks;
    bridgeCallbacks.onRemoteDescription = [this](const QString& sdp, const QString& descriptionType) {
        rtcPeerSession_.applyRemoteDescription(sdp, descriptionType);
    };
    bridgeCallbacks.onRemoteIceCandidate = [this](const QString& candidate, const QString& mid) {
        rtcPeerSession_.addRemoteCandidate(candidate, mid);
    };
    bridgeCallbacks.onWarning = [this](const QString& message) {
        if (!message.trimmed().isEmpty()) {
            showInfoPrompt(message, false);
        }
    };
    rtcSignalingBridge_.setCallbacks(std::move(bridgeCallbacks));

    connect(&operationWatcher_, &QFutureWatcher<OperationResult>::finished, this, [this]() {
        handleOperationFinished();
    });
    connect(&refreshTimer_, &QTimer::timeout, this, [this]() {
        refreshPanel();
    });
    connect(businessClient_, &live_studio::business::BusinessLiveClient::stateChanged, this, [this]() {
        handleBusinessStateChanged();
    });
    connect(businessClient_, &live_studio::business::BusinessLiveClient::readyToPublish, this, [this]() {
        handleBusinessReadyToPublish();
    });
    connect(businessClient_, &live_studio::business::BusinessLiveClient::warningRaised, this, [this](const QString& message) {
        showInfoPrompt(message, false);
    });
    connect(businessClient_, &live_studio::business::BusinessLiveClient::errorRaised, this, [this](const QString& message) {
        showErrorPrompt(message);
    });
    connect(businessClient_, &live_studio::business::BusinessLiveClient::signalingMessageReceived, this, [this](const QJsonObject& message) {
        handleBusinessSignalingMessage(message);
    });

    refreshTimer_.start(kRefreshIntervalMs);
    beginDeviceProbe();
    refreshLinkMicPanel();
    refreshBusinessStatus();
}

LiveStudioWindow::~LiveStudioWindow() {
    refreshTimer_.stop();
    if (operationWatcher_.isRunning()) {
        operationWatcher_.waitForFinished();
    }
    if (businessClient_ != nullptr) {
        businessClient_->shutdown();
    }
    linkMicSession_.disconnectFromRoom();
    stopRtcSession("window closing");
    controller_.close();
}

void LiveStudioWindow::closeEvent(QCloseEvent* event) {
    refreshTimer_.stop();
    if (operationWatcher_.isRunning()) {
        operationWatcher_.waitForFinished();
    }
    if (businessClient_ != nullptr) {
        businessClient_->shutdown();
    }
    linkMicSession_.disconnectFromRoom();
    stopRtcSession("window closing");
    controller_.close();
    QMainWindow::closeEvent(event);
}

void LiveStudioWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    renderPreviewImage(lastPreviewImage_);
}

void LiveStudioWindow::buildUi() {
    setWindowTitle("Live Studio Panel");
    resize(1600, 920);
    setMinimumSize(1380, 820);
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
        QLabel#cardValue {
            background: #fffdf8;
            border: 1px solid #d9cdbd;
            border-radius: 8px;
            padding: 8px 10px;
            font-weight: 500;
        }
        QLineEdit, QComboBox, QPlainTextEdit, QTableWidget {
            background: #fffdf8;
            border: 1px solid #cdbca7;
            border-radius: 8px;
            padding: 6px 8px;
            color: #2d241c;
        }
        QPlainTextEdit {
            padding: 10px;
        }
        QTableWidget {
            gridline-color: #e3d8c8;
            selection-background-color: #d8eadf;
            selection-color: #2d241c;
        }
        QHeaderView::section {
            background: #efe3d2;
            color: #43372b;
            border: 0;
            padding: 6px 8px;
            font-weight: 600;
        }
        QPushButton {
            border: 0;
            border-radius: 8px;
            padding: 9px 16px;
            font-weight: 600;
            color: #ffffff;
            background: #7c8f7b;
            min-height: 22px;
        }
        QPushButton#primaryButton {
            background: #2d6b60;
        }
        QPushButton#dangerButton {
            background: #ad4f37;
        }
        QPushButton#secondaryButton {
            background: #8c7255;
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

    pageStack_ = new QStackedWidget(central);
    setupPage_ = buildSetupPage(pageStack_);
    liveRoomPage_ = buildLiveRoomPage(pageStack_);
    pageStack_->addWidget(setupPage_);
    pageStack_->addWidget(liveRoomPage_);
    pageStack_->setCurrentWidget(setupPage_);

    rootLayout->addWidget(pageStack_, 1);
    setCentralWidget(central);

    connect(startButton_, &QPushButton::clicked, this, [this]() {
        if (operationWatcher_.isRunning() || !deviceProbeReady_ || businessClient_ == nullptr) {
            return;
        }

        if (shouldUseLocalCapturePipeline()) {
            const QString rtmpUrl = rtmpUrlEdit_->text().trimmed();
            if (rtmpUrl.isEmpty()) {
                setPanelState(PanelState::failed);
                showErrorPrompt("RTMP URL cannot be empty when local capture is enabled.");
                return;
            }
        }

        const auto businessConfig = buildBusinessStartConfig();
        if (!businessConfig.isValid()) {
            setPanelState(PanelState::failed);
            showErrorPrompt("API Base URL, username, and password are required.");
            return;
        }

        rtcOnlyPublishingActive_ = false;
        setPanelState(PanelState::starting);
        statusBar()->showMessage("Logging in and preparing the business live room...", 3000);
        businessClient_->beginStartSequence(businessConfig);
    });

    connect(stopButton_, &QPushButton::clicked, this, [this]() {
        if (operationWatcher_.isRunning()) {
            return;
        }

        if (rtcOnlyPublishingActive_) {
            rtcOnlyPublishingActive_ = false;
            publishElapsed_.invalidate();
            rateSnapshot_ = RateSnapshot{};
            lastPreviewImage_ = QImage{};
            lastPreviewFrameIndex_ = -1;
            if (previewLabel_ != nullptr) {
                previewLabel_->setPixmap(QPixmap());
                previewLabel_->setText("Preview is idle.\nStart publishing to begin local capture and preview.");
            }
            setPanelState(PanelState::stopping);
            statusBar()->showMessage("Stopping RTC-only live session...", 3000);
            if (businessClient_ != nullptr) {
                businessClient_->setPublishingActive(false);
                businessClient_->beginStopSequence();
            }
            showInfoPrompt("RTC-only session stopped. Business room cleanup continues in the background.");
            switchToSetupPage();
            refreshBusinessStatus();
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

    connect(linkMicInviteButton_, &QPushButton::clicked, this, [this]() {
        const auto target = buildInviteTarget();
        if (!linkMicSession_.invite(target.roomKey, target.userId)) {
            showInfoPrompt("Invite is unavailable in the current state.");
        }
    });

    connect(linkMicAcceptButton_, &QPushButton::clicked, this, [this]() {
        if (!linkMicSession_.accept()) {
            showInfoPrompt("Accept is unavailable in the current state.");
        }
    });

    connect(linkMicRejectButton_, &QPushButton::clicked, this, [this]() {
        if (!linkMicSession_.reject()) {
            showInfoPrompt("Reject is unavailable in the current state.");
        }
    });

    connect(linkMicHangupButton_, &QPushButton::clicked, this, [this]() {
        if (!linkMicSession_.hangup()) {
            showInfoPrompt("Hangup is unavailable in the current state.");
            return;
        }
        stopRtcSession("manual hangup");
    });

    connect(linkMicTargetRoomKeyEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateLinkMicActionButtons();
    });
    connect(linkMicTargetUserIdEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateLinkMicActionButtons();
    });

    refreshViewerInfo();
    refreshLiveRoomInfo(controller_.diagnostics());
    updateButtonStates();
}

QWidget* LiveStudioWindow::buildSetupPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    layout->addWidget(buildBusinessConfigGroup(page));
    layout->addWidget(buildPublishConfigGroup(page));
    layout->addWidget(buildSignalingConfigGroup(page));

    auto* liveRoomInfoGroup = new QGroupBox("Business Live State", page);
    auto* liveRoomInfoLayout = new QGridLayout(liveRoomInfoGroup);
    liveRoomInfoLayout->setHorizontalSpacing(10);
    liveRoomInfoLayout->setVerticalSpacing(10);
    liveRoomInfoLayout->setColumnStretch(1, 1);
    liveRoomInfoLayout->setColumnStretch(3, 1);

    liveRoomInfoLayout->addWidget(new QLabel("Business Status", liveRoomInfoGroup), 0, 0);
    setupRoomStatusValueLabel_ = makeCardValueLabel(liveRoomInfoGroup, "Not logged in");
    liveRoomInfoLayout->addWidget(setupRoomStatusValueLabel_, 0, 1);

    liveRoomInfoLayout->addWidget(new QLabel("Online Count", liveRoomInfoGroup), 0, 2);
    setupRoomOnlineCountValueLabel_ = makeCardValueLabel(liveRoomInfoGroup, "0");
    liveRoomInfoLayout->addWidget(setupRoomOnlineCountValueLabel_, 0, 3);

    setupRoomHintValueLabel_ = makeCardValueLabel(
        liveRoomInfoGroup,
        "Business Room Key must come from the live_room business response, not from an RTMP stream name or legacy signaling room_id.");
    liveRoomInfoLayout->addWidget(setupRoomHintValueLabel_, 1, 0, 1, 4);

    layout->addWidget(liveRoomInfoGroup);
    layout->addStretch(1);
    return page;
}

QWidget* LiveStudioWindow::buildLiveRoomPage(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, page);
    splitter->setChildrenCollapsible(false);

    auto* previewGroup = new QGroupBox("Local Preview", splitter);
    previewGroup->setMinimumWidth(640);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(12, 18, 12, 12);

    previewLabel_ = new QLabel(previewGroup);
    previewLabel_->setObjectName("previewSurface");
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumSize(640, 420);
    previewLabel_->setWordWrap(true);
    previewLabel_->setText("Preview is idle.\nStart publishing to begin local capture and preview.");
    previewLayout->addWidget(previewLabel_);

    auto* sidebarScrollArea = new QScrollArea(splitter);
    sidebarScrollArea->setWidgetResizable(true);
    sidebarScrollArea->setFrameShape(QFrame::NoFrame);
    sidebarScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarScrollArea->setMinimumWidth(520);
    sidebarScrollArea->setWidget(buildLiveRoomSidebar(sidebarScrollArea));

    splitter->addWidget(previewGroup);
    splitter->addWidget(sidebarScrollArea);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({940, 620});

    layout->addWidget(splitter, 1);
    return page;
}

QGroupBox* LiveStudioWindow::buildBusinessConfigGroup(QWidget* parent) {
    auto* group = new QGroupBox("Business Live Config", parent);
    auto* layout = new QGridLayout(group);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(10);
    layout->setColumnStretch(5, 1);

    apiBaseUrlEdit_ = new QLineEdit(group);
    apiBaseUrlEdit_->setText("http://192.168.3.28:8080");
    apiBaseUrlEdit_->setPlaceholderText("http://192.168.3.28:8080");
    apiBaseUrlEdit_->setMaximumWidth(360);

    usernameEdit_ = new QLineEdit(group);
    usernameEdit_->setPlaceholderText("username");
    usernameEdit_->setMaximumWidth(240);

    passwordEdit_ = new QLineEdit(group);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText("password");
    passwordEdit_->setMaximumWidth(240);

    businessRoomKeyEdit_ = new QLineEdit(group);
    businessRoomKeyEdit_->setPlaceholderText("optional, server-generated if empty");
    businessRoomKeyEdit_->setMaximumWidth(300);

    businessTitleEdit_ = new QLineEdit(group);
    businessTitleEdit_->setPlaceholderText("optional live title");
    businessTitleEdit_->setMaximumWidth(360);

    auto* hintLabel = new QLabel(
        "Business Room Key is not an RTMP stream name and is not a legacy signaling room_id. It must come from the live_room business response.",
        group);
    hintLabel->setWordWrap(true);

    layout->addWidget(new QLabel("API Base URL", group), 0, 0);
    layout->addWidget(apiBaseUrlEdit_, 0, 1);
    layout->addWidget(new QLabel("Username", group), 0, 2);
    layout->addWidget(usernameEdit_, 0, 3);

    layout->addWidget(new QLabel("Password", group), 1, 0);
    layout->addWidget(passwordEdit_, 1, 1);
    layout->addWidget(new QLabel("Business Room Key", group), 1, 2);
    layout->addWidget(businessRoomKeyEdit_, 1, 3);

    layout->addWidget(new QLabel("Title", group), 2, 0);
    layout->addWidget(businessTitleEdit_, 2, 1, 1, 3);
    layout->addWidget(hintLabel, 3, 0, 1, 4);
    return group;
}

QGroupBox* LiveStudioWindow::buildPublishConfigGroup(QWidget* parent) {
    auto* group = new QGroupBox("Live Publish Config", parent);
    auto* layout = new QGridLayout(group);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(10);
    layout->setColumnStretch(10, 1);

    rtmpUrlEdit_ = new QLineEdit(group);
    rtmpUrlEdit_->setPlaceholderText("rtmp://192.168.3.28/live/livestream");
    rtmpUrlEdit_->setText("rtmp://192.168.3.28/live/livestream");
    rtmpUrlEdit_->setMaximumWidth(480);

    videoDeviceCombo_ = new QComboBox(group);
    videoDeviceCombo_->setMaximumWidth(240);
    audioDeviceCombo_ = new QComboBox(group);
    audioDeviceCombo_->setMaximumWidth(240);
    resolutionCombo_ = new QComboBox(group);
    resolutionCombo_->setMaximumWidth(180);
    fpsCombo_ = new QComboBox(group);
    fpsCombo_->setMaximumWidth(100);
    sampleRateCombo_ = new QComboBox(group);
    sampleRateCombo_->setMaximumWidth(180);

    startButton_ = new QPushButton("Start Publish", group);
    startButton_->setObjectName("primaryButton");
    startButton_->setMinimumWidth(132);

    layout->addWidget(new QLabel("RTMP URL", group), 0, 0);
    layout->addWidget(rtmpUrlEdit_, 0, 1);
    layout->addWidget(startButton_, 0, 2);

    layout->addWidget(new QLabel("Video Device", group), 1, 0);
    layout->addWidget(videoDeviceCombo_, 1, 1);
    layout->addWidget(new QLabel("Audio Device", group), 1, 2);
    layout->addWidget(audioDeviceCombo_, 1, 3);
    layout->addWidget(new QLabel("Resolution", group), 1, 4);
    layout->addWidget(resolutionCombo_, 1, 5);
    layout->addWidget(new QLabel("FPS", group), 1, 6);
    layout->addWidget(fpsCombo_, 1, 7);
    layout->addWidget(new QLabel("Sample Rate", group), 1, 8);
    layout->addWidget(sampleRateCombo_, 1, 9);

    return group;
}

QGroupBox* LiveStudioWindow::buildSignalingConfigGroup(QWidget* parent) {
    auto* group = new QGroupBox("Cross-Room Anchor LinkMic", parent);
    auto* layout = new QGridLayout(group);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(10);
    layout->setColumnStretch(0, 1);

    auto* hintLabel = new QLabel(
        "Cross-room invites should be started after your anchor has entered the live room. "
        "Use the live-room sidebar to enter the remote anchor's roomKey and userId, then send Invite from there.",
        group);
    hintLabel->setWordWrap(true);
    layout->addWidget(hintLabel, 0, 0);

    return group;
}

QWidget* LiveStudioWindow::buildLiveRoomSidebar(QWidget* parent) {
    auto* sidebar = new QWidget(parent);
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* roomInfoGroup = new QGroupBox("Room Info", sidebar);
    auto* roomInfoLayout = new QGridLayout(roomInfoGroup);
    roomInfoLayout->setHorizontalSpacing(10);
    roomInfoLayout->setVerticalSpacing(10);
    roomInfoLayout->setColumnStretch(1, 1);
    roomInfoLayout->setColumnStretch(3, 1);

    roomInfoLayout->addWidget(new QLabel("Business Room Key", roomInfoGroup), 0, 0);
    liveRoomIdValueLabel_ = makeCardValueLabel(roomInfoGroup);
    roomInfoLayout->addWidget(liveRoomIdValueLabel_, 0, 1, 1, 3);

    roomInfoLayout->addWidget(new QLabel("Status", roomInfoGroup), 1, 0);
    liveRoomStatusValueLabel_ = makeCardValueLabel(roomInfoGroup, "Not logged in");
    roomInfoLayout->addWidget(liveRoomStatusValueLabel_, 1, 1);

    roomInfoLayout->addWidget(new QLabel("Online Count", roomInfoGroup), 1, 2);
    liveRoomOnlineCountValueLabel_ = makeCardValueLabel(roomInfoGroup, "0");
    roomInfoLayout->addWidget(liveRoomOnlineCountValueLabel_, 1, 3);

    roomInfoLayout->addWidget(new QLabel("Publish Duration", roomInfoGroup), 2, 0);
    liveRoomDurationValueLabel_ = makeCardValueLabel(roomInfoGroup);
    roomInfoLayout->addWidget(liveRoomDurationValueLabel_, 2, 1);

    roomInfoLayout->addWidget(new QLabel("Current User", roomInfoGroup), 2, 2);
    liveRoomOwnerValueLabel_ = makeCardValueLabel(roomInfoGroup);
    roomInfoLayout->addWidget(liveRoomOwnerValueLabel_, 2, 3);

    roomInfoLayout->addWidget(new QLabel("Business WS", roomInfoGroup), 3, 0);
    businessWsStateValueLabel_ = makeCardValueLabel(roomInfoGroup);
    roomInfoLayout->addWidget(businessWsStateValueLabel_, 3, 1);

    roomInfoLayout->addWidget(new QLabel("Signaling URL", roomInfoGroup), 3, 2);
    businessSignalingValueLabel_ = makeCardValueLabel(roomInfoGroup);
    roomInfoLayout->addWidget(businessSignalingValueLabel_, 3, 3);

    roomInfoLayout->addWidget(new QLabel("Last Issue", roomInfoGroup), 4, 0);
    businessIssueValueLabel_ = makeCardValueLabel(roomInfoGroup, "none");
    roomInfoLayout->addWidget(businessIssueValueLabel_, 4, 1, 1, 3);

    auto* linkMicGroup = new QGroupBox("Cross-Room Anchor LinkMic", sidebar);
    auto* linkMicLayout = new QGridLayout(linkMicGroup);
    linkMicLayout->setHorizontalSpacing(10);
    linkMicLayout->setVerticalSpacing(10);
    linkMicLayout->setColumnStretch(1, 1);
    linkMicLayout->setColumnStretch(3, 1);

    linkMicTargetRoomKeyEdit_ = new QLineEdit(linkMicGroup);
    linkMicTargetRoomKeyEdit_->setPlaceholderText("target anchor roomKey");
    linkMicTargetRoomKeyEdit_->setClearButtonEnabled(true);
    linkMicTargetRoomKeyEdit_->setFocusPolicy(Qt::StrongFocus);
    linkMicTargetRoomKeyEdit_->setMinimumWidth(220);
    linkMicTargetRoomKeyEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    linkMicTargetUserIdEdit_ = new QLineEdit(linkMicGroup);
    linkMicTargetUserIdEdit_->setPlaceholderText("target anchor userId");
    linkMicTargetUserIdEdit_->setClearButtonEnabled(true);
    linkMicTargetUserIdEdit_->setFocusPolicy(Qt::StrongFocus);
    linkMicTargetUserIdEdit_->setMinimumWidth(180);
    linkMicTargetUserIdEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    linkMicLayout->addWidget(new QLabel("Target Room Key", linkMicGroup), 0, 0);
    linkMicLayout->addWidget(linkMicTargetRoomKeyEdit_, 0, 1);
    linkMicLayout->addWidget(new QLabel("Target User ID", linkMicGroup), 0, 2);
    linkMicLayout->addWidget(linkMicTargetUserIdEdit_, 0, 3);

    auto* linkMicHintLabel = new QLabel(
        "Enter the remote anchor's roomKey and userId here after you are live. "
        "Invite and Hangup stay in this same cross-room session area.",
        linkMicGroup);
    linkMicHintLabel->setWordWrap(true);
    linkMicLayout->addWidget(linkMicHintLabel, 1, 0, 1, 4);

    auto* linkMicActionLayout = new QGridLayout();
    linkMicActionLayout->setHorizontalSpacing(8);
    linkMicActionLayout->setVerticalSpacing(8);
    linkMicActionLayout->setColumnStretch(0, 1);
    linkMicActionLayout->setColumnStretch(1, 1);
    linkMicActionLayout->setColumnStretch(2, 1);
    linkMicActionLayout->setColumnStretch(3, 1);

    linkMicInviteButton_ = new QPushButton("Invite", linkMicGroup);
    linkMicInviteButton_->setObjectName("primaryButton");
    linkMicAcceptButton_ = new QPushButton("Accept", linkMicGroup);
    linkMicAcceptButton_->setObjectName("primaryButton");
    linkMicRejectButton_ = new QPushButton("Reject", linkMicGroup);
    linkMicRejectButton_->setObjectName("secondaryButton");
    linkMicHangupButton_ = new QPushButton("Hangup", linkMicGroup);
    linkMicHangupButton_->setObjectName("dangerButton");

    const std::array<QPushButton*, 4> linkMicButtons = {
        linkMicInviteButton_,
        linkMicAcceptButton_,
        linkMicRejectButton_,
        linkMicHangupButton_,
    };
    for (QPushButton* button : linkMicButtons) {
        button->setMinimumWidth(120);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    linkMicActionLayout->addWidget(linkMicInviteButton_, 0, 0);
    linkMicActionLayout->addWidget(linkMicAcceptButton_, 0, 1);
    linkMicActionLayout->addWidget(linkMicRejectButton_, 0, 2);
    linkMicActionLayout->addWidget(linkMicHangupButton_, 0, 3);
    linkMicLayout->addLayout(linkMicActionLayout, 2, 0, 1, 4);

    auto* stateGroup = new QGroupBox("Stage1 State", sidebar);
    auto* stateLayout = new QGridLayout(stateGroup);
    stateLayout->setHorizontalSpacing(10);
    stateLayout->setVerticalSpacing(10);
    stateLayout->setColumnStretch(1, 1);
    stateLayout->setColumnStretch(3, 1);

    stateLayout->addWidget(new QLabel("Transport", stateGroup), 0, 0);
    linkMicTransportValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicTransportValueLabel_, 0, 1);
    stateLayout->addWidget(new QLabel("Signal State", stateGroup), 0, 2);
    linkMicStateValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicStateValueLabel_, 0, 3);
    stateLayout->addWidget(new QLabel("Session ID", stateGroup), 1, 0);
    linkMicSessionValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicSessionValueLabel_, 1, 1);
    stateLayout->addWidget(new QLabel("Request ID", stateGroup), 1, 2);
    linkMicRequestValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRequestValueLabel_, 1, 3);
    stateLayout->addWidget(new QLabel("Business Role", stateGroup), 2, 0);
    linkMicBusinessRoleValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicBusinessRoleValueLabel_, 2, 1);
    stateLayout->addWidget(new QLabel("RTC Role", stateGroup), 2, 2);
    linkMicRtcRoleValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRtcRoleValueLabel_, 2, 3);
    stateLayout->addWidget(new QLabel("Remote Anchor", stateGroup), 3, 0);
    linkMicRemoteValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRemoteValueLabel_, 3, 1, 1, 3);
    stateLayout->addWidget(new QLabel("Current Room Members", stateGroup), 4, 0);
    linkMicOnlineValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicOnlineValueLabel_, 4, 1);
    stateLayout->addWidget(new QLabel("RTC Count", stateGroup), 4, 2);
    linkMicRtcCountValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRtcCountValueLabel_, 4, 3);
    stateLayout->addWidget(new QLabel("Join Params", stateGroup), 5, 0);
    linkMicJoinParamsValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicJoinParamsValueLabel_, 5, 1, 1, 3);
    stateLayout->addWidget(new QLabel("RTC Peer", stateGroup), 6, 0);
    rtcPeerStateValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(rtcPeerStateValueLabel_, 6, 1);
    stateLayout->addWidget(new QLabel("DataChannel", stateGroup), 6, 2);
    rtcDataChannelValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(rtcDataChannelValueLabel_, 6, 3);
    stateLayout->addWidget(new QLabel("Ping / Pong", stateGroup), 7, 0);
    rtcPingValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(rtcPingValueLabel_, 7, 1, 1, 3);
    stateLayout->addWidget(new QLabel("Last Error", stateGroup), 8, 0);
    linkMicErrorValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicErrorValueLabel_, 8, 1, 1, 3);

    auto* viewersGroup = new QGroupBox("Live Room Audience", sidebar);
    auto* viewersLayout = new QGridLayout(viewersGroup);
    viewersLayout->setHorizontalSpacing(10);
    viewersLayout->setVerticalSpacing(10);
    viewersLayout->setColumnStretch(1, 1);

    viewersLayout->addWidget(new QLabel("Audience Count", viewersGroup), 0, 0);
    viewerOnlineCountValueLabel_ = makeCardValueLabel(viewersGroup, "0");
    viewersLayout->addWidget(viewerOnlineCountValueLabel_, 0, 1);

    audienceMemberTable_ = new QTableWidget(0, 4, viewersGroup);
    audienceMemberTable_->setHorizontalHeaderLabels({"User ID", "Username", "Role", "Status"});
    audienceMemberTable_->horizontalHeader()->setStretchLastSection(true);
    audienceMemberTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    audienceMemberTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    audienceMemberTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    audienceMemberTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    audienceMemberTable_->verticalHeader()->setVisible(false);
    audienceMemberTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    audienceMemberTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    audienceMemberTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    audienceMemberTable_->setAlternatingRowColors(true);
    audienceMemberTable_->setMinimumHeight(180);
    viewersLayout->addWidget(audienceMemberTable_, 1, 0, 1, 2);

    auto* membersGroup = new QGroupBox("RTC LinkMic Members", sidebar);
    auto* membersLayout = new QVBoxLayout(membersGroup);
    membersLayout->setContentsMargins(10, 18, 10, 10);
    membersLayout->setSpacing(10);

    linkMicMemberTable_ = new QTableWidget(0, 4, membersGroup);
    linkMicMemberTable_->setHorizontalHeaderLabels({"User ID", "Display", "Role", "Status"});
    linkMicMemberTable_->horizontalHeader()->setStretchLastSection(true);
    linkMicMemberTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    linkMicMemberTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    linkMicMemberTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    linkMicMemberTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    linkMicMemberTable_->verticalHeader()->setVisible(false);
    linkMicMemberTable_->setSelectionMode(QAbstractItemView::NoSelection);
    linkMicMemberTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    linkMicMemberTable_->setAlternatingRowColors(true);
    linkMicMemberTable_->setMinimumHeight(150);
    membersLayout->addWidget(linkMicMemberTable_);

    auto* eventGroup = new QGroupBox("Event Log", sidebar);
    auto* eventLayout = new QVBoxLayout(eventGroup);
    eventLayout->setContentsMargins(10, 18, 10, 10);

    linkMicEventLogEdit_ = new QPlainTextEdit(eventGroup);
    linkMicEventLogEdit_->setReadOnly(true);
    linkMicEventLogEdit_->setMinimumHeight(230);
    QFont monoFont("Consolas", 9);
    linkMicEventLogEdit_->setFont(monoFont);
    eventLayout->addWidget(linkMicEventLogEdit_);

    stopButton_ = new QPushButton("Stop Publish", sidebar);
    stopButton_->setObjectName("dangerButton");
    stopButton_->setMinimumWidth(132);

    auto* stopLayout = new QHBoxLayout();
    stopLayout->setContentsMargins(0, 0, 0, 0);
    stopLayout->addStretch(1);
    stopLayout->addWidget(stopButton_);

    layout->addWidget(roomInfoGroup);
    layout->addWidget(linkMicGroup);
    layout->addWidget(stateGroup);
    layout->addWidget(viewersGroup);
    layout->addWidget(membersGroup);
    layout->addWidget(eventGroup);
    layout->addLayout(stopLayout);
    layout->addStretch(1);

    return sidebar;
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

        live_studio::capture::LiveDeviceProbeResult probe;
        result.status = live_studio::capture::enumerateLiveDeviceCaptureDevices(&probe);
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
    const live_studio::session::SessionDiagnostics diagnostics = controller_.diagnostics();
    updateStatusSummary(diagnostics);
    refreshLiveRoomInfo(diagnostics);
    refreshBusinessStatus();
    refreshViewerInfo();
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
        rtcOnlyPublishingActive_ = false;
        publishElapsed_.invalidate();
        if (businessClient_ != nullptr) {
            businessClient_->setPublishingActive(false);
            businessClient_->beginStopSequence();
        }
        setPanelState(PanelState::failed);
        showErrorPrompt(QString("Start publish failed: %1").arg(QString::fromStdString(result.status.message)));
        return;
    }

    publishElapsed_.restart();
    if (businessClient_ != nullptr) {
        businessClient_->setPublishingActive(true);
    }
    setPanelState(PanelState::publishing);
    showInfoPrompt("Business ready and RTMP publishing is live.", true);
    switchToLiveRoomPage();
}

void LiveStudioWindow::handleStopPublishResult(const OperationResult& result) {
    if (!result.status.ok()) {
        setPanelState(PanelState::failed);
        showErrorPrompt(QString("Stop publish failed: %1").arg(QString::fromStdString(result.status.message)));
        return;
    }

    rtcOnlyPublishingActive_ = false;
    publishElapsed_.invalidate();
    if (businessClient_ != nullptr) {
        businessClient_->setPublishingActive(false);
        businessClient_->beginStopSequence();
    }
    setPanelState(PanelState::idle);
    showInfoPrompt("Publish stopped. Business room cleanup continues in the background.");
    switchToSetupPage();
}

void LiveStudioWindow::handleBusinessStateChanged() {
    const auto previousState = businessSnapshot_.state;
    if (businessClient_ != nullptr) {
        businessSnapshot_ = businessClient_->snapshot();
    }
    syncLinkMicTransportFromBusiness();
    const auto diagnostics = controller_.diagnostics();

    if (!diagnostics.publishing) {
        switch (businessSnapshot_.state) {
        case live_studio::business::BusinessLiveState::logging_in:
        case live_studio::business::BusinessLiveState::creating_room:
        case live_studio::business::BusinessLiveState::ws_connecting:
        case live_studio::business::BusinessLiveState::ws_connected:
            if (!rtcOnlyPublishingActive_) {
                setPanelState(PanelState::starting);
            }
            break;
        case live_studio::business::BusinessLiveState::stopping:
            rtcOnlyPublishingActive_ = false;
            setPanelState(PanelState::stopping);
            break;
        case live_studio::business::BusinessLiveState::error:
            rtcOnlyPublishingActive_ = false;
            publishElapsed_.invalidate();
            setPanelState(PanelState::failed);
            break;
        case live_studio::business::BusinessLiveState::idle:
        case live_studio::business::BusinessLiveState::logged_in:
        case live_studio::business::BusinessLiveState::room_created:
            if (rtcOnlyPublishingActive_) {
                rtcOnlyPublishingActive_ = false;
                publishElapsed_.invalidate();
                setPanelState(PanelState::idle);
            } else if (panelState_ != PanelState::publishing) {
                setPanelState(PanelState::idle);
            }
            break;
        case live_studio::business::BusinessLiveState::anchor_online:
        case live_studio::business::BusinessLiveState::reconnecting:
            if (!rtcOnlyPublishingActive_ && panelState_ != PanelState::publishing) {
                setPanelState(PanelState::idle);
            }
            break;
        }
    }

    if (previousState == live_studio::business::BusinessLiveState::reconnecting &&
        businessSnapshot_.state == live_studio::business::BusinessLiveState::anchor_online && diagnostics.publishing) {
        showInfoPrompt("Business presence recovered.", false);
    }

    refreshBusinessStatus();
    refreshLinkMicPanel();
}

void LiveStudioWindow::handleBusinessReadyToPublish() {
    if (operationWatcher_.isRunning()) {
        return;
    }

    if (!shouldUseLocalCapturePipeline()) {
        controller_.close();
        rtcOnlyPublishingActive_ = true;
        publishElapsed_.restart();
        rateSnapshot_ = RateSnapshot{};
        lastPreviewImage_ = QImage{};
        lastPreviewFrameIndex_ = -1;
        if (businessClient_ != nullptr) {
            businessClient_->setPublishingActive(true);
        }
        setPanelState(PanelState::publishing);
        if (previewLabel_ != nullptr) {
            previewLabel_->setPixmap(QPixmap());
            previewLabel_->setText(
                "Local preview is unavailable.\nRTC-only mode is active because no camera is selected.");
        }
        statusBar()->showMessage("Business ready. RTC-only mode is active.", 3000);
        showInfoPrompt("Business ready. RTC-only mode is active without local capture or RTMP publish.", true);
        switchToLiveRoomPage();
        return;
    }

    rtcOnlyPublishingActive_ = false;
    setPanelState(PanelState::starting);
    statusBar()->showMessage("Business ready. Starting local capture and RTMP publish...", 3000);
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
        if (!result.status.ok()) {
            controller_.close();
        }
        return result;
    });
}

void LiveStudioWindow::updateButtonStates() {
    const bool operationBusy = operationWatcher_.isRunning();
    const bool businessBusy = live_studio::business::businessStateIsBusy(businessSnapshot_.state);
    const bool busy = operationBusy || businessBusy;
    const live_studio::session::SessionDiagnostics diagnostics = controller_.diagnostics();
    const bool publishing = isPublishingSessionActive(diagnostics);
    const bool localCaptureEnabled = shouldUseLocalCapturePipeline();
    const bool hasRtmpUrl = rtmpUrlEdit_ != nullptr && !rtmpUrlEdit_->text().trimmed().isEmpty();
    const bool hasBusinessConfig =
        apiBaseUrlEdit_ != nullptr && usernameEdit_ != nullptr && passwordEdit_ != nullptr &&
        !apiBaseUrlEdit_->text().trimmed().isEmpty() && !usernameEdit_->text().trimmed().isEmpty() &&
        !passwordEdit_->text().isEmpty();

    if (apiBaseUrlEdit_ != nullptr) {
        apiBaseUrlEdit_->setEnabled(!busy && !publishing);
    }
    if (usernameEdit_ != nullptr) {
        usernameEdit_->setEnabled(!busy && !publishing);
    }
    if (passwordEdit_ != nullptr) {
        passwordEdit_->setEnabled(!busy && !publishing);
    }
    if (businessRoomKeyEdit_ != nullptr) {
        businessRoomKeyEdit_->setEnabled(!busy && !publishing);
    }
    if (businessTitleEdit_ != nullptr) {
        businessTitleEdit_->setEnabled(!busy && !publishing);
    }

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
        startButton_->setEnabled(
            deviceProbeReady_ && !busy && !publishing && hasBusinessConfig && (!localCaptureEnabled || hasRtmpUrl));
    }
    if (stopButton_ != nullptr) {
        stopButton_->setEnabled(!operationBusy && publishing);
    }
}

void LiveStudioWindow::refreshLiveRoomInfo(const live_studio::session::SessionDiagnostics& diagnostics) {
    const QString roomKeyText = fallbackText(businessSnapshot_.roomKey);
    const QString liveStatusText = businessDisplayStateText(businessSnapshot_, diagnostics, panelState_);
    const QString durationText = publishElapsed_.isValid() ? formatDuration(publishElapsed_.elapsed()) : "--";
    const QString onlineCountText = QString::number(businessSnapshot_.members.size());
    const QString issueText = !businessSnapshot_.lastError.trimmed().isEmpty()
                                  ? businessSnapshot_.lastError.trimmed()
                                  : fallbackText(businessSnapshot_.lastWarning, "none");
    const QString roomHintText = !businessSnapshot_.roomKey.trimmed().isEmpty()
                                     ? QString("Business Room Key=%1 | signaling=%2")
                                           .arg(businessSnapshot_.roomKey, fallbackText(businessSnapshot_.signalingUrl))
                                     : QString("Business Room Key will come from the business live_room response, never from RTMP URL or legacy signaling room_id.");
    const QString wsStateText = businessSnapshot_.anchorJoined
                                    ? "anchor joined"
                                    : (businessSnapshot_.businessWsConnected
                                           ? "connected"
                                           : (businessSnapshot_.state ==
                                                      live_studio::business::BusinessLiveState::reconnecting
                                                  ? "reconnecting"
                                                  : "disconnected"));
    const QString userText = businessSnapshot_.userId > 0
                                 ? QString("%1 (#%2)").arg(fallbackText(businessSnapshot_.username),
                                                           QString::number(businessSnapshot_.userId))
                                 : "--";

    if (setupRoomStatusValueLabel_ != nullptr) {
        setupRoomStatusValueLabel_->setText(liveStatusText);
    }
    if (setupRoomOnlineCountValueLabel_ != nullptr) {
        setupRoomOnlineCountValueLabel_->setText(onlineCountText);
    }
    if (setupRoomHintValueLabel_ != nullptr) {
        setupRoomHintValueLabel_->setText(roomHintText);
    }
    if (liveRoomIdValueLabel_ != nullptr) {
        liveRoomIdValueLabel_->setText(roomKeyText);
    }
    if (liveRoomStatusValueLabel_ != nullptr) {
        liveRoomStatusValueLabel_->setText(liveStatusText);
    }
    if (liveRoomOnlineCountValueLabel_ != nullptr) {
        liveRoomOnlineCountValueLabel_->setText(onlineCountText);
    }
    if (liveRoomDurationValueLabel_ != nullptr) {
        liveRoomDurationValueLabel_->setText(durationText);
    }
    if (liveRoomOwnerValueLabel_ != nullptr) {
        liveRoomOwnerValueLabel_->setText(userText);
    }
    if (businessWsStateValueLabel_ != nullptr) {
        businessWsStateValueLabel_->setText(wsStateText);
    }
    if (businessSignalingValueLabel_ != nullptr) {
        businessSignalingValueLabel_->setText(fallbackText(businessSnapshot_.signalingUrl));
    }
    if (businessIssueValueLabel_ != nullptr) {
        businessIssueValueLabel_->setText(issueText);
    }
}

void LiveStudioWindow::refreshViewerInfo() {
    int viewerCount = 0;
    for (const auto& member : businessSnapshot_.members) {
        if (!isAudienceMember(businessSnapshot_, member)) {
            continue;
        }
        ++viewerCount;
    }

    if (viewerOnlineCountValueLabel_ != nullptr) {
        viewerOnlineCountValueLabel_->setText(QString::number(viewerCount));
    }
    populateAudienceMemberTable();
    updateLinkMicActionButtons();
}

void LiveStudioWindow::refreshBusinessStatus() {
    const auto diagnostics = controller_.diagnostics();
    refreshLiveRoomInfo(diagnostics);
    updateStatusSummary(diagnostics);
    refreshViewerInfo();
    updateButtonStates();
}

void LiveStudioWindow::syncLinkMicTransportFromBusiness() {
    const bool hasBusinessIdentity = businessSnapshot_.userId > 0 &&
        !businessSnapshot_.roomKey.trimmed().isEmpty() &&
        !businessSnapshot_.signalingUrl.trimmed().isEmpty();

    if (!hasBusinessIdentity) {
        if (linkMicSession_.usesExternalTransport()) {
            linkMicSession_.disconnectFromRoom();
        }
        stopRtcSession("business identity unavailable");
        return;
    }

    const live_studio::linkmic::LinkMicSessionConfig businessConfig = buildBusinessLinkMicSessionConfig();
    const bool configChanged =
        linkMicSession_.config().signalingUrl != businessConfig.signalingUrl ||
        linkMicSession_.config().roomKey != businessConfig.roomKey ||
        linkMicSession_.config().userId != businessConfig.userId ||
        linkMicSession_.config().displayName != businessConfig.displayName;

    if (configChanged) {
        linkMicSession_.connectToRoom(businessConfig);
    }

    if (businessSnapshot_.anchorJoined) {
        linkMicSession_.setExternalTransportState(
            live_studio::linkmic::SignalingConnectionState::connected,
            "business websocket joined the live room");
        return;
    }

    if (businessSnapshot_.businessWsConnected) {
        linkMicSession_.setExternalTransportState(
            live_studio::linkmic::SignalingConnectionState::connecting,
            "business websocket connected, waiting for live.anchor.joined");
        return;
    }

    const live_studio::linkmic::SignalingConnectionState state =
        live_studio::linkmic::SignalingConnectionState::disconnected;
    const QString detail =
        businessSnapshot_.state == live_studio::business::BusinessLiveState::reconnecting
        ? QString("business websocket reconnecting")
        : QString("business websocket is offline");
    linkMicSession_.setExternalTransportState(state, detail);
    if (state == live_studio::linkmic::SignalingConnectionState::disconnected) {
        stopRtcSession(detail);
    }
}

void LiveStudioWindow::handleBusinessSignalingMessage(const QJsonObject& message) {
    if (linkMicSession_.usesExternalTransport()) {
        linkMicSession_.processExternalMessage(message);
    }

    if (rtcSignalingBridge_.isActive()) {
        rtcSignalingBridge_.handleIncomingMessage(message);
    }

    const QString type = message.value("type").toString().trimmed();
    if (type == "rtc.join-params") {
        ensureRtcSessionForJoinParams();
        return;
    }

    if (type == "linkmic.reject" || type == "linkmic.cancel" ||
        type == "linkmic.hangup" || type == "linkmic.kick") {
        stopRtcSession(type);
    }
}

void LiveStudioWindow::ensureRtcSessionForJoinParams() {
    const auto& roomState = linkMicSession_.roomState();
    if (!roomState.hasJoinParams()) {
        return;
    }

    const auto& joinParams = roomState.joinParams();
    if (rtcSignalingBridge_.isActive() && rtcSignalingBridge_.sessionId() == joinParams.sessionId) {
        return;
    }

    stopRtcSession("starting new rtc session");

    live_studio::rtc::RtcSessionConfig config;
    config.sessionId = joinParams.sessionId;
    config.requestId = joinParams.requestId;
    config.roomAKey = joinParams.roomAKey;
    config.roomBKey = joinParams.roomBKey;
    config.selfRoomKey = joinParams.selfRoomKey.trimmed().isEmpty() ? businessSnapshot_.roomKey : joinParams.selfRoomKey;
    config.peerRoomKey = joinParams.peerRoomKey;
    config.selfUserId = joinParams.selfUserId.trimmed().isEmpty()
        ? QString::number(businessSnapshot_.userId)
        : joinParams.selfUserId;
    config.peerUserId = joinParams.peerUserId;
    config.businessRole = joinParams.businessRole;
    config.rtcRole = joinParams.rtcRole;
    config.signalingUrl = QUrl::fromUserInput(joinParams.signalingUrl.trimmed().isEmpty()
                                                  ? businessSnapshot_.signalingUrl
                                                  : joinParams.signalingUrl);
    config.iceServers = joinParams.iceServers;
    config.rtcBackend = joinParams.rtcBackend;
    config.mediaMode = joinParams.mediaMode;

    const bool bridgeStarted = rtcSignalingBridge_.start(
        config,
        [this](const QJsonObject& envelope) {
            return businessClient_ != nullptr && businessClient_->sendSignalingEnvelope(envelope);
        });
    if (!bridgeStarted || !rtcPeerSession_.start(config)) {
        stopRtcSession("failed to start libdatachannel peer session");
        showErrorPrompt("Failed to start the cross-room libdatachannel session.");
        return;
    }

    refreshLinkMicPanel();
}

void LiveStudioWindow::stopRtcSession(const QString& reason) {
    rtcSignalingBridge_.stop();
    rtcPeerSession_.stop(reason);
}

void LiveStudioWindow::switchToSetupPage() {
    if (pageStack_ == nullptr || setupPage_ == nullptr || pageStack_->currentWidget() == setupPage_) {
        return;
    }

    pageStack_->setCurrentWidget(setupPage_);
}

void LiveStudioWindow::switchToLiveRoomPage() {
    if (pageStack_ == nullptr || liveRoomPage_ == nullptr) {
        return;
    }

    if (pageStack_->currentWidget() != liveRoomPage_) {
        pageStack_->setCurrentWidget(liveRoomPage_);
    }
    renderPreviewImage(lastPreviewImage_);
}

bool LiveStudioWindow::shouldUseLocalCapturePipeline() const {
    return videoDeviceCombo_ != nullptr && !videoDeviceCombo_->currentData().toString().trimmed().isEmpty();
}

bool LiveStudioWindow::isPublishingSessionActive(
    const live_studio::session::SessionDiagnostics& diagnostics) const {
    return diagnostics.publishing || rtcOnlyPublishingActive_;
}

void LiveStudioWindow::refreshPanel() {
    updatePreview();

    const live_studio::session::SessionDiagnostics diagnostics = controller_.diagnostics();
    const bool publishingActive = isPublishingSessionActive(diagnostics);
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
        } else if (rtcOnlyPublishingActive_) {
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

    if (publishingActive) {
        switchToLiveRoomPage();
    } else if (pageStack_ != nullptr && pageStack_->currentWidget() == liveRoomPage_ &&
               panelState_ != PanelState::starting && panelState_ != PanelState::stopping) {
        switchToSetupPage();
    }

    updateStatusSummary(diagnostics);
    refreshLiveRoomInfo(diagnostics);
    refreshViewerInfo();
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
    const bool publishingActive = isPublishingSessionActive(diagnostics);
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

    if (publishingActive && !publishElapsed_.isValid()) {
        publishElapsed_.start();
    }
    if (!publishingActive && panelState_ != PanelState::starting) {
        publishElapsed_.invalidate();
    }

    const QString durationText =
        publishingActive && publishElapsed_.isValid() ? formatDuration(publishElapsed_.elapsed()) : "--";
    const QString firstPacketText = diagnostics.firstMediaPacketSent
                                        ? QString("%1 ms").arg(diagnostics.firstMediaLatencyMs, 0, 'f', 1)
                                        : "--";
    const QString errorText = diagnostics.lastError.empty() ? "none" : elideError(diagnostics.lastError);
    const QString businessText = businessDisplayStateText(businessSnapshot_, diagnostics, panelState_);
    const QString wsText = businessSnapshot_.anchorJoined
                               ? "anchor-online"
                               : (businessSnapshot_.businessWsConnected ? "ws-connected" : "ws-offline");

    statusSummaryLabel_->setText(
        QString("business=%1 | room=%2 | ws=%3 | video=%4 fps | audio=%5/s | publish=%6 | first_packet=%7 | last_error=%8")
            .arg(businessText)
            .arg(fallbackText(businessSnapshot_.roomKey))
            .arg(wsText)
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
    videoDeviceCombo_->addItem("none", "");
    if (!probe.availableVideoDeviceNames.empty()) {
        videoDeviceCombo_->addItem("default", "default");
    }
    for (const std::string& deviceName : probe.availableVideoDeviceNames) {
        const QString name = QString::fromStdString(deviceName);
        videoDeviceCombo_->addItem(name, name);
    }
    if (!probe.availableVideoDeviceNames.empty()) {
        selectComboData(videoDeviceCombo_, "default");
    } else {
        selectComboData(videoDeviceCombo_, "");
    }

    audioDeviceCombo_->clear();
    audioDeviceCombo_->addItem("none", "");
    audioDeviceCombo_->addItem("default", "default");
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

void LiveStudioWindow::refreshLinkMicPanel() {
    const auto& roomState = linkMicSession_.roomState();
    const auto rtcSnapshot = rtcPeerSession_.snapshot();
    int audienceCount = 0;
    for (const auto& member : businessSnapshot_.members) {
        if (isAudienceMember(businessSnapshot_, member)) {
            ++audienceCount;
        }
    }

    if (linkMicTransportValueLabel_ != nullptr) {
        QString transportText = live_studio::linkmic::connectionStateText(roomState.connectionState());
        if (linkMicSession_.usesExternalTransport()) {
            transportText = "business-ws | " + transportText;
        } else {
            transportText = "standalone-ws | " + transportText;
        }
        if (roomState.isRegistered() && roomState.connectionState() == live_studio::linkmic::SignalingConnectionState::connected) {
            transportText += " | registered";
        }
        linkMicTransportValueLabel_->setText(transportText);
    }
    if (linkMicStateValueLabel_ != nullptr) {
        linkMicStateValueLabel_->setText(fallbackText(roomState.signalState()));
    }
    if (linkMicSessionValueLabel_ != nullptr) {
        linkMicSessionValueLabel_->setText(fallbackText(roomState.activeSessionId()));
    }
    if (linkMicRequestValueLabel_ != nullptr) {
        linkMicRequestValueLabel_->setText(fallbackText(roomState.activeRequestId()));
    }
    if (linkMicBusinessRoleValueLabel_ != nullptr) {
        linkMicBusinessRoleValueLabel_->setText(fallbackText(roomState.businessRole()));
    }
    if (linkMicRtcRoleValueLabel_ != nullptr) {
        linkMicRtcRoleValueLabel_->setText(fallbackText(roomState.rtcRole()));
    }
    if (linkMicRemoteValueLabel_ != nullptr) {
        const QString remoteText = roomState.remoteRoomKey().trimmed().isEmpty()
            ? roomState.remoteUserId()
            : QString("%1 @ %2").arg(fallbackText(roomState.remoteUserId()), roomState.remoteRoomKey());
        linkMicRemoteValueLabel_->setText(fallbackText(remoteText));
    }
    if (linkMicOnlineValueLabel_ != nullptr) {
        linkMicOnlineValueLabel_->setText(QString::number(audienceCount));
    }
    if (linkMicRtcCountValueLabel_ != nullptr) {
        const int rtcCount = roomState.activeSessionId().trimmed().isEmpty() ? 0 :
            (roomState.remoteUserId().trimmed().isEmpty() ? 1 : 2);
        linkMicRtcCountValueLabel_->setText(QString::number(rtcCount));
    }
    if (linkMicJoinParamsValueLabel_ != nullptr) {
        QString joinText = "--";
        if (roomState.hasJoinParams()) {
            joinText = live_studio::linkmic::joinParamsSummary(roomState.joinParams());
        } else if (roomState.isWaitingJoinParams()) {
            joinText = "waiting for rtc.join-params";
        }
        linkMicJoinParamsValueLabel_->setText(joinText);
    }
    if (linkMicErrorValueLabel_ != nullptr) {
        const QString errorText = roomState.lastError().trimmed().isEmpty()
            ? rtcSnapshot.lastError
            : roomState.lastError();
        linkMicErrorValueLabel_->setText(fallbackText(errorText));
    }
    if (rtcPeerStateValueLabel_ != nullptr) {
        rtcPeerStateValueLabel_->setText(live_studio::rtc::rtcTransportStateText(rtcSnapshot.transportState));
    }
    if (rtcDataChannelValueLabel_ != nullptr) {
        rtcDataChannelValueLabel_->setText(live_studio::rtc::rtcDataChannelStateText(rtcSnapshot.dataChannelState));
    }
    if (rtcPingValueLabel_ != nullptr) {
        const QString pingText = QString("ping sent=%1 recv=%2 | pong sent=%3 recv=%4")
                                     .arg(rtcSnapshot.pingSentCount)
                                     .arg(rtcSnapshot.pingReceivedCount)
                                     .arg(rtcSnapshot.pongSentCount)
                                     .arg(rtcSnapshot.pongReceivedCount);
        rtcPingValueLabel_->setText(pingText);
    }

    populateLinkMicMemberTable();
    renderLinkMicEventLog();
    updateLinkMicConnectionControls();
    updateLinkMicActionButtons();
}

void LiveStudioWindow::populateAudienceMemberTable() {
    if (audienceMemberTable_ == nullptr) {
        return;
    }

    int rowCount = 0;
    for (const auto& member : businessSnapshot_.members) {
        if (isAudienceMember(businessSnapshot_, member)) {
            ++rowCount;
        }
    }

    const QSignalBlocker blocker(audienceMemberTable_);
    audienceMemberTable_->setRowCount(rowCount);

    int row = 0;
    for (const auto& member : businessSnapshot_.members) {
        if (!isAudienceMember(businessSnapshot_, member)) {
            continue;
        }

        const QString userIdText = QString::number(member.userId);
        auto* userIdItem = new QTableWidgetItem(userIdText);
        userIdItem->setData(Qt::UserRole, userIdText);
        auto* usernameItem = new QTableWidgetItem(fallbackText(member.username, userIdText));
        auto* roleItem = new QTableWidgetItem(fallbackText(member.role, "viewer"));
        auto* statusItem = new QTableWidgetItem(businessMemberSummary(member));

        audienceMemberTable_->setItem(row, 0, userIdItem);
        audienceMemberTable_->setItem(row, 1, usernameItem);
        audienceMemberTable_->setItem(row, 2, roleItem);
        audienceMemberTable_->setItem(row, 3, statusItem);
        ++row;
    }
}

void LiveStudioWindow::populateLinkMicMemberTable() {
    if (linkMicMemberTable_ == nullptr) {
        return;
    }

    const QSignalBlocker blocker(linkMicMemberTable_);
    const auto& roomState = linkMicSession_.roomState();
    const bool hasSession = !roomState.activeSessionId().trimmed().isEmpty();
    const int rowCount = hasSession ? (roomState.remoteUserId().trimmed().isEmpty() ? 1 : 2) : 0;
    linkMicMemberTable_->setRowCount(rowCount);

    if (!hasSession) {
        return;
    }

    auto* localUserIdItem = new QTableWidgetItem(linkMicSession_.config().userId + " (you)");
    auto* localDisplayItem = new QTableWidgetItem(fallbackText(linkMicSession_.config().displayName, linkMicSession_.config().userId));
    auto* localRoleItem = new QTableWidgetItem(fallbackText(roomState.businessRole(), "local-anchor"));
    auto* localStatusItem = new QTableWidgetItem("local-room=" + fallbackText(linkMicSession_.config().roomKey));
    linkMicMemberTable_->setItem(0, 0, localUserIdItem);
    linkMicMemberTable_->setItem(0, 1, localDisplayItem);
    linkMicMemberTable_->setItem(0, 2, localRoleItem);
    linkMicMemberTable_->setItem(0, 3, localStatusItem);

    if (rowCount < 2) {
        return;
    }

    auto* remoteUserIdItem = new QTableWidgetItem(roomState.remoteUserId());
    auto* remoteDisplayItem = new QTableWidgetItem(roomState.remoteUserId());
    auto* remoteRoleItem = new QTableWidgetItem(
        roomState.businessRole() == "inviter" ? "invitee" : "inviter");
    auto* remoteStatusItem = new QTableWidgetItem("remote-room=" + fallbackText(roomState.remoteRoomKey()));
    linkMicMemberTable_->setItem(1, 0, remoteUserIdItem);
    linkMicMemberTable_->setItem(1, 1, remoteDisplayItem);
    linkMicMemberTable_->setItem(1, 2, remoteRoleItem);
    linkMicMemberTable_->setItem(1, 3, remoteStatusItem);
}

void LiveStudioWindow::renderLinkMicEventLog() {
    if (linkMicEventLogEdit_ == nullptr) {
        return;
    }

    QStringList lines;
    const auto& events = linkMicSession_.eventLog();
    lines.reserve(events.size());
    for (const auto& entry : events) {
        lines.push_back(formatLinkMicEvent(entry));
    }

    const QString nextText = lines.join('\n');
    if (linkMicEventLogEdit_->toPlainText() != nextText) {
        linkMicEventLogEdit_->setPlainText(nextText);
        auto* scrollBar = linkMicEventLogEdit_->verticalScrollBar();
        if (scrollBar != nullptr) {
            scrollBar->setValue(scrollBar->maximum());
        }
    }
}

void LiveStudioWindow::updateLinkMicConnectionControls() {
    if (linkMicTargetRoomKeyEdit_ != nullptr) {
        linkMicTargetRoomKeyEdit_->setEnabled(true);
        linkMicTargetRoomKeyEdit_->setReadOnly(false);
    }
    if (linkMicTargetUserIdEdit_ != nullptr) {
        linkMicTargetUserIdEdit_->setEnabled(true);
        linkMicTargetUserIdEdit_->setReadOnly(false);
    }
}

void LiveStudioWindow::updateLinkMicActionButtons() {
    const auto target = buildInviteTarget();
    const auto& roomState = linkMicSession_.roomState();

    if (linkMicInviteButton_ != nullptr) {
        linkMicInviteButton_->setEnabled(roomState.canInvite(linkMicSession_.config().roomKey,
                                                             linkMicSession_.config().userId,
                                                             target.roomKey,
                                                             target.userId));
    }
    if (linkMicAcceptButton_ != nullptr) {
        linkMicAcceptButton_->setEnabled(roomState.canAccept());
    }
    if (linkMicRejectButton_ != nullptr) {
        linkMicRejectButton_->setEnabled(roomState.canReject());
    }
    if (linkMicHangupButton_ != nullptr) {
        linkMicHangupButton_->setEnabled(roomState.canTerminate());
    }
}

live_studio::business::BusinessStartConfig LiveStudioWindow::buildBusinessStartConfig() const {
    live_studio::business::BusinessStartConfig config;
    config.apiBaseUrl = apiBaseUrlEdit_ != nullptr ? apiBaseUrlEdit_->text().trimmed() : QString();
    config.username = usernameEdit_ != nullptr ? usernameEdit_->text().trimmed() : QString();
    config.password = passwordEdit_ != nullptr ? passwordEdit_->text() : QString();
    config.roomKey = businessRoomKeyEdit_ != nullptr ? businessRoomKeyEdit_->text().trimmed() : QString();
    config.title = businessTitleEdit_ != nullptr ? businessTitleEdit_->text().trimmed() : QString();
    return config;
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

live_studio::linkmic::LinkMicSessionConfig LiveStudioWindow::buildBusinessLinkMicSessionConfig() const {
    live_studio::linkmic::LinkMicSessionConfig config;
    config.signalingUrl = QUrl::fromUserInput(businessSnapshot_.signalingUrl.trimmed());
    config.roomKey = businessSnapshot_.roomKey.trimmed();
    config.userId = businessSnapshot_.userId > 0 ? QString::number(businessSnapshot_.userId) : QString();
    config.displayName = fallbackText(businessSnapshot_.username, config.userId);
    config.requestTimeoutMs = 30000;
    return config;
}

live_studio::linkmic::CrossRoomInviteTarget LiveStudioWindow::buildInviteTarget() const {
    live_studio::linkmic::CrossRoomInviteTarget target;
    target.roomKey = linkMicTargetRoomKeyEdit_ != nullptr ? linkMicTargetRoomKeyEdit_->text().trimmed() : QString();
    target.userId = linkMicTargetUserIdEdit_ != nullptr ? linkMicTargetUserIdEdit_->text().trimmed() : QString();
    return target;
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

QString LiveStudioWindow::liveRoomStatusText(PanelState state, bool publishing) {
    if (publishing || state == PanelState::publishing) {
        return "live";
    }

    switch (state) {
    case PanelState::idle:
        return "idle";
    case PanelState::starting:
        return "starting";
    case PanelState::publishing:
        return "live";
    case PanelState::stopping:
    case PanelState::failed:
        return "ended";
    }
    return "idle";
}

QString LiveStudioWindow::businessDisplayStateText(
    const live_studio::business::BusinessLiveSnapshot& snapshot,
    const live_studio::session::SessionDiagnostics& diagnostics,
    PanelState panelState) {
    if (panelState == PanelState::stopping || snapshot.state == live_studio::business::BusinessLiveState::stopping) {
        return "Stopping";
    }
    if (diagnostics.publishing || panelState == PanelState::publishing) {
        return "Publishing";
    }
    if (snapshot.state == live_studio::business::BusinessLiveState::error || panelState == PanelState::failed) {
        return "Error";
    }
    if (snapshot.state == live_studio::business::BusinessLiveState::reconnecting) {
        return "Business presence disconnected";
    }
    if (snapshot.state == live_studio::business::BusinessLiveState::logging_in) {
        return "Logging in";
    }
    if (snapshot.state == live_studio::business::BusinessLiveState::creating_room) {
        return "Creating room";
    }
    if (snapshot.state == live_studio::business::BusinessLiveState::ws_connecting) {
        return "Connecting WebSocket";
    }
    if (snapshot.anchorJoined) {
        return "Anchor online";
    }
    if (snapshot.businessWsConnected) {
        return "WebSocket connected";
    }
    if (!snapshot.roomKey.trimmed().isEmpty()) {
        return "Room created";
    }
    if (!snapshot.accessToken.trimmed().isEmpty() && snapshot.userId > 0) {
        return "Logged in";
    }
    return "Not logged in";
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
    return text.left(kMaxCharacters - 3) + "...";
}

QString LiveStudioWindow::formatLinkMicEvent(const live_studio::linkmic::LinkMicEventLogEntry& entry) {
    QStringList parts;
    parts.push_back(entry.timestampUtc.toLocalTime().toString("HH:mm:ss.zzz"));
    parts.push_back(entry.direction);
    parts.push_back(entry.type);
    if (!entry.state.trimmed().isEmpty()) {
        parts.push_back("state=" + entry.state);
    }
    if (!entry.sessionId.trimmed().isEmpty()) {
        parts.push_back("session=" + entry.sessionId);
    }
    if (!entry.requestId.trimmed().isEmpty()) {
        parts.push_back("request=" + entry.requestId);
    }
    if (!entry.remoteRoomKey.trimmed().isEmpty()) {
        parts.push_back("remoteRoom=" + entry.remoteRoomKey);
    }
    if (!entry.remoteUserId.trimmed().isEmpty()) {
        parts.push_back("remote=" + entry.remoteUserId);
    }

    QString line = parts.join(" | ");
    if (!entry.detail.trimmed().isEmpty()) {
        line += " | " + entry.detail;
    }
    return line;
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
        QMessageBox::information(const_cast<LiveStudioWindow*>(this), "Live Studio", text);
    }
}

void LiveStudioWindow::showErrorPrompt(const QString& text) const {
    if (statusBar() != nullptr) {
        statusBar()->showMessage(text, 7000);
    }
    QMessageBox::warning(const_cast<LiveStudioWindow*>(this), "Live Studio", text);
}
