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

        const QString rtmpUrl = rtmpUrlEdit_->text().trimmed();
        if (rtmpUrl.isEmpty()) {
            setPanelState(PanelState::failed);
            showErrorPrompt("RTMP URL cannot be empty.");
            return;
        }

        const auto businessConfig = buildBusinessStartConfig();
        if (!businessConfig.isValid()) {
            setPanelState(PanelState::failed);
            showErrorPrompt("API Base URL, username, and password are required.");
            return;
        }

        setPanelState(PanelState::starting);
        statusBar()->showMessage("Logging in and preparing the business live room...", 3000);
        businessClient_->beginStartSequence(businessConfig);
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

    connect(linkMicConnectButton_, &QPushButton::clicked, this, [this]() {
        const auto config = buildLinkMicSessionConfig();
        if (!config.isValid()) {
            showErrorPrompt("LinkMic signaling config is incomplete.");
            return;
        }
        if (!linkMicSession_.connectToRoom(config)) {
            showErrorPrompt("Failed to initialize the stage1 signaling session.");
            return;
        }
        statusBar()->showMessage("Stage1 signaling connect requested.", 3000);
    });

    connect(linkMicDisconnectButton_, &QPushButton::clicked, this, [this]() {
        linkMicSession_.disconnectFromRoom();
        statusBar()->showMessage("Stage1 signaling disconnected.", 3000);
    });

    connect(linkMicInviteButton_, &QPushButton::clicked, this, [this]() {
        const QString targetUserId = selectedAudienceMemberId();
        if (!linkMicSession_.invite(targetUserId)) {
            showInfoPrompt("Invite is unavailable in the current state.");
        }
    });

    connect(linkMicApplyButton_, &QPushButton::clicked, this, [this]() {
        const QString targetUserId = selectedAudienceMemberId();
        if (!linkMicSession_.apply(targetUserId)) {
            showInfoPrompt("Apply is unavailable in the current state.");
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
        }
    });

    connect(linkMicRoleCombo_, &QComboBox::currentTextChanged, this, [this](const QString& roleText) {
        if (linkMicUserIdEdit_ != nullptr &&
            (linkMicUserIdEdit_->text().trimmed().isEmpty() ||
             linkMicUserIdEdit_->text() == "controller-panel" ||
             linkMicUserIdEdit_->text() == "guest-panel")) {
            linkMicUserIdEdit_->setText(roleText == "guest" ? "guest-panel" : "controller-panel");
        }

        if (linkMicDisplayNameEdit_ != nullptr &&
            (linkMicDisplayNameEdit_->text().trimmed().isEmpty() ||
             linkMicDisplayNameEdit_->text() == "Controller Panel" ||
             linkMicDisplayNameEdit_->text() == "Guest Panel")) {
            linkMicDisplayNameEdit_->setText(roleText == "guest" ? "Guest Panel" : "Controller Panel");
        }

        refreshLinkMicPanel();
    });

    connect(audienceMemberTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
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
        "Business Room Key must come from the live_room business response, not from an RTMP stream name or probe room_id.");
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
        "Business Room Key is not an RTMP stream name and is not a probe room_id. It must come from the live_room business response.",
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
    auto* group = new QGroupBox("RTC Probe Signaling Config", parent);
    auto* layout = new QGridLayout(group);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(10);
    layout->setColumnStretch(6, 1);

    linkMicSignalingUrlEdit_ = new QLineEdit(group);
    linkMicSignalingUrlEdit_->setText("ws://192.168.3.28:8080/ws");
    linkMicSignalingUrlEdit_->setMaximumWidth(480);

    linkMicRoomIdEdit_ = new QLineEdit(group);
    linkMicRoomIdEdit_->setText("stage1");
    linkMicRoomIdEdit_->setMaximumWidth(240);

    linkMicUserIdEdit_ = new QLineEdit(group);
    linkMicUserIdEdit_->setText("controller-panel");
    linkMicUserIdEdit_->setMaximumWidth(240);

    linkMicDisplayNameEdit_ = new QLineEdit(group);
    linkMicDisplayNameEdit_->setText("Controller Panel");
    linkMicDisplayNameEdit_->setMaximumWidth(240);

    linkMicRoleCombo_ = new QComboBox(group);
    linkMicRoleCombo_->addItem("controller");
    linkMicRoleCombo_->addItem("guest");
    linkMicRoleCombo_->setMaximumWidth(180);

    linkMicConnectButton_ = new QPushButton("Connect", group);
    linkMicConnectButton_->setObjectName("primaryButton");
    linkMicConnectButton_->setMinimumWidth(120);

    linkMicDisconnectButton_ = new QPushButton("Disconnect", group);
    linkMicDisconnectButton_->setObjectName("dangerButton");
    linkMicDisconnectButton_->setMinimumWidth(120);

    layout->addWidget(new QLabel("Signaling URL", group), 0, 0);
    layout->addWidget(linkMicSignalingUrlEdit_, 0, 1, 1, 3);

    layout->addWidget(new QLabel("Probe Room ID", group), 1, 0);
    layout->addWidget(linkMicRoomIdEdit_, 1, 1);
    layout->addWidget(new QLabel("User ID", group), 1, 2);
    layout->addWidget(linkMicUserIdEdit_, 1, 3);

    layout->addWidget(new QLabel("Display Name", group), 2, 0);
    layout->addWidget(linkMicDisplayNameEdit_, 2, 1);
    layout->addWidget(new QLabel("Role", group), 2, 2);
    layout->addWidget(linkMicRoleCombo_, 2, 3);

    layout->addWidget(linkMicConnectButton_, 3, 1);
    layout->addWidget(linkMicDisconnectButton_, 3, 2);

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
    stateLayout->addWidget(new QLabel("Request ID", stateGroup), 1, 0);
    linkMicRequestValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRequestValueLabel_, 1, 1);
    stateLayout->addWidget(new QLabel("Remote User", stateGroup), 1, 2);
    linkMicRemoteValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRemoteValueLabel_, 1, 3);
    stateLayout->addWidget(new QLabel("Audience Count", stateGroup), 2, 0);
    linkMicOnlineValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicOnlineValueLabel_, 2, 1);
    stateLayout->addWidget(new QLabel("RTC Count", stateGroup), 2, 2);
    linkMicRtcCountValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicRtcCountValueLabel_, 2, 3);
    stateLayout->addWidget(new QLabel("Join Params", stateGroup), 3, 0);
    linkMicJoinParamsValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicJoinParamsValueLabel_, 3, 1, 1, 3);
    stateLayout->addWidget(new QLabel("Last Error", stateGroup), 4, 0);
    linkMicErrorValueLabel_ = makeCardValueLabel(stateGroup);
    stateLayout->addWidget(linkMicErrorValueLabel_, 4, 1, 1, 3);

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

    auto* audienceActionLayout = new QGridLayout();
    audienceActionLayout->setHorizontalSpacing(8);
    audienceActionLayout->setVerticalSpacing(8);

    linkMicInviteButton_ = new QPushButton("Invite", viewersGroup);
    linkMicInviteButton_->setObjectName("primaryButton");
    linkMicApplyButton_ = new QPushButton("Apply", viewersGroup);
    linkMicApplyButton_->setObjectName("secondaryButton");

    const std::array<QPushButton*, 2> audienceButtons = {
        linkMicInviteButton_,
        linkMicApplyButton_,
    };
    for (QPushButton* button : audienceButtons) {
        button->setMinimumWidth(120);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    audienceActionLayout->setColumnStretch(0, 1);
    audienceActionLayout->setColumnStretch(1, 1);
    audienceActionLayout->addWidget(linkMicInviteButton_, 0, 0);
    audienceActionLayout->addWidget(linkMicApplyButton_, 0, 1);
    viewersLayout->addLayout(audienceActionLayout, 2, 0, 1, 2);

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

    auto* rtcActionLayout = new QGridLayout();
    rtcActionLayout->setHorizontalSpacing(8);
    rtcActionLayout->setVerticalSpacing(8);

    linkMicAcceptButton_ = new QPushButton("Accept", membersGroup);
    linkMicAcceptButton_->setObjectName("primaryButton");
    linkMicRejectButton_ = new QPushButton("Reject", membersGroup);
    linkMicRejectButton_->setObjectName("secondaryButton");
    linkMicHangupButton_ = new QPushButton("Hangup", membersGroup);
    linkMicHangupButton_->setObjectName("dangerButton");

    const std::array<QPushButton*, 3> rtcButtons = {
        linkMicAcceptButton_,
        linkMicRejectButton_,
        linkMicHangupButton_,
    };
    for (QPushButton* button : rtcButtons) {
        button->setMinimumWidth(120);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    rtcActionLayout->setColumnStretch(0, 1);
    rtcActionLayout->setColumnStretch(1, 1);
    rtcActionLayout->setColumnStretch(2, 1);
    rtcActionLayout->addWidget(linkMicAcceptButton_, 0, 0);
    rtcActionLayout->addWidget(linkMicRejectButton_, 0, 1);
    rtcActionLayout->addWidget(linkMicHangupButton_, 0, 2);
    membersLayout->addLayout(rtcActionLayout);

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
    const auto diagnostics = controller_.diagnostics();

    if (!diagnostics.publishing) {
        switch (businessSnapshot_.state) {
        case live_studio::business::BusinessLiveState::logging_in:
        case live_studio::business::BusinessLiveState::creating_room:
        case live_studio::business::BusinessLiveState::ws_connecting:
        case live_studio::business::BusinessLiveState::ws_connected:
            setPanelState(PanelState::starting);
            break;
        case live_studio::business::BusinessLiveState::stopping:
            setPanelState(PanelState::stopping);
            break;
        case live_studio::business::BusinessLiveState::error:
            setPanelState(PanelState::failed);
            break;
        case live_studio::business::BusinessLiveState::idle:
        case live_studio::business::BusinessLiveState::logged_in:
        case live_studio::business::BusinessLiveState::room_created:
        case live_studio::business::BusinessLiveState::anchor_online:
        case live_studio::business::BusinessLiveState::reconnecting:
            if (panelState_ != PanelState::publishing) {
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
    const bool publishing = diagnostics.publishing;
    const bool hasVideoSelection =
        videoDeviceCombo_ != nullptr && !videoDeviceCombo_->currentData().toString().trimmed().isEmpty();
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
            deviceProbeReady_ && !busy && !publishing && hasVideoSelection && hasRtmpUrl && hasBusinessConfig);
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
                                     : QString("Business Room Key will come from the business live_room response, never from RTMP URL or probe room_id.");
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

    if (diagnostics.publishing) {
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
    videoDeviceCombo_->addItem("default", "default");
    for (const std::string& deviceName : probe.availableVideoDeviceNames) {
        const QString name = QString::fromStdString(deviceName);
        videoDeviceCombo_->addItem(name, name);
    }
    selectComboData(videoDeviceCombo_, "default");

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
    const auto& memberDirectory = linkMicSession_.memberDirectory();
    int audienceCount = 0;
    for (const auto& member : businessSnapshot_.members) {
        if (isAudienceMember(businessSnapshot_, member)) {
            ++audienceCount;
        }
    }

    if (linkMicTransportValueLabel_ != nullptr) {
        QString transportText = live_studio::linkmic::connectionStateText(roomState.connectionState());
        if (roomState.isRegistered() && roomState.connectionState() == live_studio::linkmic::SignalingConnectionState::connected) {
            transportText += " | registered";
        }
        linkMicTransportValueLabel_->setText(transportText);
    }
    if (linkMicStateValueLabel_ != nullptr) {
        linkMicStateValueLabel_->setText(fallbackText(roomState.signalState()));
    }
    if (linkMicRequestValueLabel_ != nullptr) {
        linkMicRequestValueLabel_->setText(fallbackText(roomState.activeRequestId()));
    }
    if (linkMicRemoteValueLabel_ != nullptr) {
        linkMicRemoteValueLabel_->setText(fallbackText(roomState.remoteUserId()));
    }
    if (linkMicOnlineValueLabel_ != nullptr) {
        linkMicOnlineValueLabel_->setText(QString::number(audienceCount));
    }
    if (linkMicRtcCountValueLabel_ != nullptr) {
        linkMicRtcCountValueLabel_->setText(QString::number(rtcLinkMicMemberCount(memberDirectory)));
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
        linkMicErrorValueLabel_->setText(fallbackText(roomState.lastError()));
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

    const QString selectedUserId = selectedAudienceMemberId();
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

    if (!selectedUserId.isEmpty()) {
        for (int currentRow = 0; currentRow < audienceMemberTable_->rowCount(); ++currentRow) {
            if (audienceMemberTable_->item(currentRow, 0) != nullptr &&
                audienceMemberTable_->item(currentRow, 0)->data(Qt::UserRole).toString() == selectedUserId) {
                audienceMemberTable_->selectRow(currentRow);
                break;
            }
        }
    }
}

void LiveStudioWindow::populateLinkMicMemberTable() {
    if (linkMicMemberTable_ == nullptr) {
        return;
    }

    const auto& members = linkMicSession_.memberDirectory().members();
    const QSignalBlocker blocker(linkMicMemberTable_);
    linkMicMemberTable_->setRowCount(rtcLinkMicMemberCount(linkMicSession_.memberDirectory()));

    int row = 0;
    for (const auto& member : members) {
        // Stage1 currently exposes RTC presence through inLinkMic flags.
        // When we later support multi-user RTC, replace this with the real RTC participant roster.
        if (!member.inLinkMic) {
            continue;
        }

        auto* userIdItem = new QTableWidgetItem(member.userId);
        if (member.userId == linkMicSession_.config().userId) {
            userIdItem->setText(member.userId + " (you)");
        }
        userIdItem->setData(Qt::UserRole, member.userId);
        auto* displayItem = new QTableWidgetItem(live_studio::linkmic::displayNameForMember(member));
        auto* roleItem = new QTableWidgetItem(fallbackText(member.role, member.userId == linkMicSession_.config().userId ? "local" : "viewer"));
        auto* statusItem = new QTableWidgetItem(roleSummary(member));

        linkMicMemberTable_->setItem(row, 0, userIdItem);
        linkMicMemberTable_->setItem(row, 1, displayItem);
        linkMicMemberTable_->setItem(row, 2, roleItem);
        linkMicMemberTable_->setItem(row, 3, statusItem);
        ++row;
    }
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
    const auto connectionState = linkMicSession_.roomState().connectionState();
    const bool busy = connectionState == live_studio::linkmic::SignalingConnectionState::connecting ||
        connectionState == live_studio::linkmic::SignalingConnectionState::connected;

    const auto config = buildLinkMicSessionConfig();
    if (linkMicSignalingUrlEdit_ != nullptr) {
        linkMicSignalingUrlEdit_->setEnabled(!busy);
    }
    if (linkMicRoomIdEdit_ != nullptr) {
        linkMicRoomIdEdit_->setEnabled(!busy);
    }
    if (linkMicUserIdEdit_ != nullptr) {
        linkMicUserIdEdit_->setEnabled(!busy);
    }
    if (linkMicDisplayNameEdit_ != nullptr) {
        linkMicDisplayNameEdit_->setEnabled(!busy);
    }
    if (linkMicRoleCombo_ != nullptr) {
        linkMicRoleCombo_->setEnabled(!busy);
    }
    if (linkMicConnectButton_ != nullptr) {
        linkMicConnectButton_->setEnabled(!busy && config.isValid());
    }
    if (linkMicDisconnectButton_ != nullptr) {
        linkMicDisconnectButton_->setEnabled(busy);
    }
}

void LiveStudioWindow::updateLinkMicActionButtons() {
    const QString selectedUserId = selectedAudienceMemberId();
    const auto selectedMember = linkMicSession_.memberDirectory().memberById(selectedUserId);
    const bool selectedInLinkMic = selectedMember.has_value() && selectedMember->inLinkMic;
    const QString localBusinessUserId =
        businessSnapshot_.userId > 0 ? QString::number(businessSnapshot_.userId) : QString();
    const bool selectedIsLocal = !selectedUserId.isEmpty() &&
        (selectedUserId == linkMicSession_.config().userId || selectedUserId == localBusinessUserId);
    const auto& roomState = linkMicSession_.roomState();

    if (linkMicInviteButton_ != nullptr) {
        linkMicInviteButton_->setEnabled(!selectedIsLocal &&
            roomState.canInvite(selectedUserId, selectedInLinkMic));
    }
    if (linkMicApplyButton_ != nullptr) {
        linkMicApplyButton_->setEnabled(!selectedIsLocal &&
            roomState.canApply(selectedUserId, selectedInLinkMic));
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

live_studio::linkmic::LinkMicSessionConfig LiveStudioWindow::buildLinkMicSessionConfig() const {
    live_studio::linkmic::LinkMicSessionConfig config;
    config.signalingUrl = QUrl::fromUserInput(linkMicSignalingUrlEdit_ != nullptr
                                                  ? linkMicSignalingUrlEdit_->text().trimmed()
                                                  : QString());
    config.roomId = linkMicRoomIdEdit_ != nullptr ? linkMicRoomIdEdit_->text().trimmed() : QString();
    config.userId = linkMicUserIdEdit_ != nullptr ? linkMicUserIdEdit_->text().trimmed() : QString();
    config.displayName = linkMicDisplayNameEdit_ != nullptr
        ? linkMicDisplayNameEdit_->text().trimmed()
        : QString();
    if (config.displayName.isEmpty()) {
        config.displayName = config.userId;
    }
    config.role = linkMicRoleCombo_ != nullptr ? linkMicRoleCombo_->currentText().trimmed() : "controller";
    config.requestTimeoutMs = 10000;
    return config;
}

QString LiveStudioWindow::selectedAudienceMemberId() const {
    if (audienceMemberTable_ == nullptr || audienceMemberTable_->currentRow() < 0) {
        return {};
    }

    const auto* item = audienceMemberTable_->item(audienceMemberTable_->currentRow(), 0);
    if (item == nullptr) {
        return {};
    }
    return item->data(Qt::UserRole).toString().trimmed();
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
    if (!entry.requestId.trimmed().isEmpty()) {
        parts.push_back("request=" + entry.requestId);
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
