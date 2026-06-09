#include <QAbstractSocket>
#include <QDateTime>
#include <QMouseEvent>
#include <QMessageBox>
#include <QTextCursor>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProgressDialog>
#include <QTimer>
#include <memory>
#include <cstring>
#include <iostream>
#include "mainwindow.h"
#include "ui_mainwindow.h"



MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_socket(new QTcpSocket(this))
    , m_dragging(false)
{
    ui->setupUi(this);

    // 界面初始化和网络初始化分开写，后续扩展 SQL 登录时结构更清楚。
    InitUi();
    InitSocket();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::InitUi()
{
    // 使用无边框窗口，右上角的最小化/最大化/关闭按钮由主窗口公共标题栏统一管理。
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setWindowTitle("聊天室客户端");
    resize(980, 680);

    // 启动时先停留在登录页，注册页和聊天页通过按钮或连接状态切换。
    ui->stackedWidget->setCurrentWidget(ui->LoginPage);
    // 【新增/优化】✨ 动态时间欢迎语提示
    int hour = QTime::currentTime().hour();
    QString greeting = "✨ 欢迎回来";
    if (hour >= 5 && hour < 12)       greeting = "🌅 早上好，朋友";
    else if (hour >= 12 && hour < 18) greeting = "☕ 下午好，朋友";
    else                              greeting = "🌙 愿你有个美好的夜晚";
    ui->titleLogin->setText(greeting);
    // 本地调试默认值：如果服务端部署在虚拟机或云服务器，需要改成对应 IP。
    ui->lineEditHost->setText("192.168.56.100");
    ui->lineEditPort->setText("8888");
    ui->lineEditName->setText("默认用户");
    ui->lineEditHost->setPlaceholderText("例如：127.0.0.1");
    ui->lineEditPort->setPlaceholderText("例如：8888");
    ui->lineEditName->setPlaceholderText("请输入聊天昵称");
    ui->lineEditRegName->setPlaceholderText("设置用户名");
    ui->lineEditRegPassword->setPlaceholderText("设置密码");
    ui->lineEditRegPassword2->setPlaceholderText("再次输入密码");
    ui->textChatRecord->setReadOnly(true);
    ui->labelAvatar->setText("头像");
    ui->labelProfileName->setText("未登录");
    ui->labelSignature->setText("那朵花儿~");
    ui->statusbar->showMessage("未连接服务器");
    SetConnectedState(false);

    // 【新增/优化】🖼️ 工具栏按钮直接升级为 emoji 贴纸
    ui->btnToolImage->setText("🖼️");
    ui->btnToolEmoji->setText("😀");
    ui->btnToolFile->setText("📁");
    ui->btnToolShot->setText("✂️");
    ui->btnToolHistory->setText("📜 历史记录");
    // 自定义窗口按钮属于主窗口公共标题栏，所以登录页、注册页、聊天页都会显示。
    connect(ui->btnWindowMin, &QPushButton::clicked, this, &MainWindow::showMinimized);
    connect(ui->btnWindowMax, &QPushButton::clicked, this, [this]() {
        if (isMaximized())
        {
            showNormal();
        }
        else
        {
            showMaximized();
        }

        ui->btnWindowMax->setText(isMaximized() ? "❐" : "□");
    });
    connect(ui->btnWindowClose, &QPushButton::clicked, this, &MainWindow::close);

    // 登录页 -> 注册页。
    connect(ui->btnGoRegister, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->RegisterPage);
        ui->statusbar->showMessage("请填写注册信息");
    });

    // 注册页 -> 登录页。
    connect(ui->btnBackLogin, &QPushButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->LoginPage);
        ui->statusbar->showMessage("未连接服务器");
    });

    // 当前注册按钮只做前端校验。
    // 真正保存用户信息时，建议把账号密码发给服务器，由服务器写入 SQL 数据库。
    connect(ui->btnRegister, &QPushButton::clicked, this, [this]() {
        const QString name = ui->lineEditRegName->text().trimmed();
        const QString password = ui->lineEditRegPassword->text();
        const QString confirm = ui->lineEditRegPassword2->text();

        if (name.isEmpty() || password.isEmpty())
        {
            QMessageBox::warning(this, "注册失败", "用户名和密码不能为空。");
            return;
        }

        if (password != confirm)
        {
            QMessageBox::warning(this, "注册失败", "两次输入的密码不一致。");
            return;
        }

        QMessageBox::information(this, "提示", "注册页已准备好，后续建议把用户信息保存到服务器端 SQL 数据库。");
        ui->lineEditName->setText(name);
        ui->stackedWidget->setCurrentWidget(ui->LoginPage);
        ui->statusbar->showMessage("注册信息校验通过，请连接服务器");
    });

    // 点击连接按钮后，读取 IP/端口/用户名，然后发起 TCP 连接。
    connect(ui->btnConnect, &QPushButton::clicked, this, [this]() {
        const QString host = ui->lineEditHost->text().trimmed();
        const quint16 port = ui->lineEditPort->text().toUShort();
        m_userName = ui->lineEditName->text().trimmed();

        if (host.isEmpty() || port == 0 || m_userName.isEmpty())
        {
            QMessageBox::warning(this, "连接失败", "请填写服务器 IP、端口和用户名。");
            return;
        }

        ui->btnConnect->setEnabled(false);
        ui->btnConnect->setText("连接中...");
        ui->statusbar->showMessage(QString("正在连接 %1:%2 ...").arg(host).arg(port));

        // abort 会取消旧连接，避免用户重复点击后保留半连接状态。
        m_socket->abort();
        m_socket->connectToHost(host, port);
    });

    // 主动断开当前 TCP 连接。
    connect(ui->btnDisconnect, &QPushButton::clicked, this, [this]() {
        ui->statusbar->showMessage("正在断开连接...");
        m_socket->disconnectFromHost();
    });

    // 输入框上方的工具按钮先保留入口，后续可以分别接入图片、表情包、文件和截图功能。
    connect(ui->btnToolImage, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, "图片", "后续这里可以打开图片选择框并发送图片。");
    });

    connect(ui->btnToolEmoji, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, "表情包", "后续这里可以弹出表情包面板。");
    });

    // 【升级】点击文件按钮，选择并定向发送文件给选中的人
    connect(ui->btnToolFile, &QPushButton::clicked, this, [this]() {
        if (m_socket->state() != QAbstractSocket::ConnectedState) {
            QMessageBox::warning(this, "发送失败", "请先连接服务器！");
            return;
        }

        // 获取右侧列表当前选中的目标用户
        QListWidgetItem *selectedItem = ui->listOnlineUsers->currentItem();
        if (!selectedItem) {
            QMessageBox::warning(this, "发送失败", "请先在右侧列表中选中要发送的目标用户！");
            return;
        }
        QString targetUser = selectedItem->text();

        // 别给自己发
        if (targetUser == m_userName) {
            QMessageBox::warning(this, "提示", "不能给自己发送文件哦！");
            return;
        }

        // 弹出文件选择框
        QString filePath = QFileDialog::getOpenFileName(this, QString("发文件给 %1").arg(targetUser), "", "所有文件 (*.*)");
        if (filePath.isEmpty()) return;

        QFileInfo fileInfo(filePath);
        qint64 fileSize = fileInfo.size();

        if (fileSize > 1024 * 1024 * 1024) { // 1GB 限制
            QMessageBox::warning(this, "提示", "文件大小超过 1GB，无法发送。");
            return;
        }

        // ⭐【新增】使用二进制分块协议发送文件
        SendFileImpl(targetUser, filePath);
    });

    // 发送聊天消息。
    // 目前服务器还没有正式协议解析，所以这里先发送一段普通文本。
    connect(ui->btnSend, &QPushButton::clicked, this, [this]() {
        const QString text = ui->editMessage->text().trimmed();
        if (text.isEmpty())
        {
            return;
        }

        if (m_socket->state() != QAbstractSocket::ConnectedState)
        {
            QMessageBox::warning(this, "发送失败", "当前还没有连接服务器。");
            return;
        }

        // 加上 \n
        const QString packet = QString("[%1] %2\n").arg(m_userName, text);
        m_socket->write(packet.toUtf8());
        // 本地也立即显示一份，避免等待服务器广播给自己。
        AppendMessage("我", text);
        ui->editMessage->clear();
    });

    // 在输入框按 Enter 等同于点击发送。
    connect(ui->editMessage, &QLineEdit::returnPressed, ui->btnSend, &QPushButton::click);

    // 全局界面美化：保持课程项目风格简洁，重点突出登录卡片和聊天区域。
    this->setStyleSheet(R"(
QMainWindow{
    background:#d9f0fb;
}
QStackedWidget{
    background:transparent;
}
QWidget#ChatPage{
    background:#f7fbff;
}
QFrame#globalTitleBar{
    background:qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #56c1ee, stop:0.55 #3aa7df, stop:1 #1e84c8);
    border:none;
}
QLabel#labelWindowTitle{
    color:#ffffff;
    font-size:14px;
    font-weight:700;
}
QFrame#chatTopBar{
    background:qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #56c1ee, stop:0.55 #3aa7df, stop:1 #1e84c8);
    border:none;
}
QLabel#labelAvatar{
    background:#fff7c2;
    border:2px solid rgba(255,255,255,0.85);
    border-radius:6px;
    color:#7c4a03;
    font-weight:700;
}
QLabel#labelProfileName{
    font-size:20px;
    font-weight:700;
    color:#083b66;
}
QLabel#labelSignature{
    font-size:13px;
    color:#083b66;
}
QPushButton#btnWindowMin,QPushButton#btnWindowMax,QPushButton#btnWindowClose{
    background:transparent;
    color:white;
    border:none;
    border-radius:0;
    font-size:18px;
    font-weight:700;
    padding:0;
}
QPushButton#btnWindowMin:hover,QPushButton#btnWindowMax:hover{
    background:rgba(255,255,255,0.18);
}
QPushButton#btnWindowClose:hover{
    background:#ef4444;
}
QFrame#blueDivider{
    background:qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #dff5ff, stop:0.5 #8ed8f6, stop:1 #dff5ff);
    border:none;
}
QFrame#rightTransferPanel{
    background:#eaf7fd;
    border-left:1px solid #b9e3f5;
}
QLabel#labelTransferTitle{
    background:qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #1689c8, stop:1 #0b6ca8);
    color:white;
    font-size:20px;
    font-weight:700;
    padding:8px;
}
QFrame#inputPanel{
    background:#f1faff;
    border-top:1px solid #b9e3f5;
}
QPushButton#btnToolImage,QPushButton#btnToolEmoji,QPushButton#btnToolFile,QPushButton#btnToolShot{
    min-width:30px;
    min-height:28px;
    max-height:28px;
    padding:0;
    border:1px solid transparent;
    border-radius:4px;
    background:transparent;
    color:#1c7fb6;
    font-size:17px;
}
QPushButton#btnToolHistory{
    min-height:28px;
    max-height:28px;
    padding:0 10px;
    border:1px solid transparent;
    border-radius:4px;
    background:transparent;
    color:#1c7fb6;
}
QPushButton#btnToolImage:hover,QPushButton#btnToolEmoji:hover,QPushButton#btnToolFile:hover,QPushButton#btnToolShot:hover,QPushButton#btnToolHistory:hover{
    border-color:#8ed8f6;
    background:#dcf4ff;
}
QLabel#labelTransferHint{
    color:#075985;
    font-weight:700;
}
QLabel#labelTransferMock1,QLabel#labelTransferMock2{
    background:#f8fdff;
    border:1px solid #bee8f8;
    border-radius:6px;
    padding:8px;
    color:#1f4f6b;
    line-height:150%;
}
QFrame#loginCard,QFrame#registerCard{
    background:#ffffff;
    border:1px solid #d8e0ea;
    border-radius:8px;
}
QLabel{
    font-size:14px;
    color:#27364a;
}
QLabel#titleLogin,QLabel#titleRegister{
    font-size:26px;
    font-weight:700;
    color:#172033;
    padding-bottom:8px;
}
QLabel#labelChatTitle{
    font-size:22px;
    font-weight:700;
    color:#172033;
}
QLabel#labelOnlineUsers{
    font-size:16px;
    font-weight:700;
    color:#172033;
}
QLineEdit{
    border:1px solid #c7d2de;
    border-radius:6px;
    padding:9px 11px;
    font-size:14px;
    background:#ffffff;
    selection-background-color:#2563eb;
}
QLineEdit:hover{
    border-color:#9fb3c8;
}
QLineEdit:focus{
    border:1px solid #2684ff;
}
QTextEdit{
    background:#ffffff;
    border:none;
    border-radius:0;
    padding:10px;
    font-size:14px;
}
QListWidget{
    background:#ffffff;
    border:1px solid #d8e0ea;
    border-radius:6px;
    padding:8px;
    font-size:14px;
}
QListWidget::item{
    padding:8px;
    border-radius:5px;
}
QListWidget::item:selected{
    background:#dbeafe;
    color:#1d4ed8;
}
QPushButton{
    border:none;
    border-radius:6px;
    padding:9px 14px;
    font-size:14px;
    background:#d9e2ec;
    color:#102a43;
}
QPushButton:hover{
    background:#cbd5e1;
}
QPushButton#btnConnect,QPushButton#btnSend,QPushButton#btnRegister{
    background:#2563eb;
    color:white;
}
QPushButton#btnConnect:hover,QPushButton#btnSend:hover,QPushButton#btnRegister:hover{
    background:#1d4ed8;
}
QPushButton#btnDisconnect{
    background:#0ea5e9;
    color:white;
}
QPushButton#btnDisconnect:hover{
    background:#0284c7;
}
QPushButton:disabled{
    background:#93a4b7;
    color:#f8fafc;
}
QStatusBar{
    color:#526173;
    background:transparent;
}
)");
}

void MainWindow::InitSocket()
{
    // 连接成功：切到聊天页，并向服务器发送一条临时 login 文本。
    // 后续做正式协议时，可以改成 JSON 或自定义消息头。
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        SetConnectedState(true);
        ui->stackedWidget->setCurrentWidget(ui->ChatPage);
        ui->labelProfileName->setText(m_userName);
        ui->listOnlineUsers->clear();
        ui->listOnlineUsers->addItem(m_userName);  // ✅ 恢复：先显示自己
        AppendMessage("系统", "已连接服务器。");

        // 加上 \n
        const QString loginPacket = QString("[login] %1\n").arg(m_userName);
        m_socket->write(loginPacket.toUtf8());
        // ⭐【修复】删除重复发送登录包
        // m_socket->write(loginPacket.toUtf8());  // ❌ 删除这行
    });

    // 服务器发来数据时触发 readyRead。
    // 当前服务端是“收到什么广播什么”，所以客户端先按 UTF-8 文本显示。


    // 服务器发来数据时触发 readyRead
    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        m_readBuffer.append(m_socket->readAll());

        while (true) {
            // ⭐【检查是否为二进制文件帧】
            if (m_readBuffer.size() >= static_cast<int>(sizeof(FileTransferHeader))) {
                FileTransferHeader header;
                std::memcpy(&header, m_readBuffer.data(), sizeof(FileTransferHeader));

                // 💡【修复】：直接读取结构体字段为局部变量（不加 ntohl），确保后续代码中的变量正常定义
                const uint32_t magic      = header.magic;
                const uint32_t type       = header.type;
                const uint32_t totalSize  = header.totalSize;
                const uint32_t chunkSize  = header.chunkSize;
                const uint32_t chunkIndex = header.chunkIndex;
                const uint32_t crc32      = header.crc32;

                if (magic == 0xDEADBEEF) {
                    // 用 chunkSize 计算总帧长
                    const int totalFrameSize = sizeof(FileTransferHeader) + chunkSize;
                    if (m_readBuffer.size() < totalFrameSize) {
                        break; // 数据不完整，等待下次 readyRead
                    }
                    QString targetUserStr = QString::fromUtf8(header.targetUser).trimmed();
                    if (targetUserStr != m_userName.trimmed()) {
                        // 如果接收人不是我，说明是服务器广播出来的他人文件流，或者是自己发出去被反弹回来的数据
                        // 直接从缓冲区中剥离这个包，不做任何业务处理，继续解析下一个包
                        m_readBuffer.remove(0, totalFrameSize);
                        continue;
                    }

                    const char* frameData = m_readBuffer.data() + sizeof(FileTransferHeader);
                    const QString sender = QString::fromUtf8(header.senderUser);

                    // 1. 收到文件请求 (作为接收方)
                    if (type == 0) { // 统一使用局部变量 type
                        const QString fileName = QString::fromUtf8(header.fileName);

                        // 弹窗询问保存路径
                        const QString savePath = QFileDialog::getSaveFileName(
                            this,
                            QString("收到来自 %1 的文件").arg(sender),
                            fileName
                            );

                        if (!savePath.isEmpty()) {
                            m_receivingFile = std::make_shared<ReceivingFile>();
                            m_receivingFile->filePath = savePath;
                            m_receivingFile->file = new QFile(savePath);

                            // ✅ 修复2：增加文件打开失败处理
                            if (!m_receivingFile->file->open(QIODevice::WriteOnly)) {
                                QMessageBox::critical(
                                    this,
                                    "保存失败",
                                    "无法创建文件：" + m_receivingFile->file->errorString()
                                    );

                                // 发送错误帧给对方（💡 修复：去掉了 htonl）
                                FileTransferHeader err{};
                                err.magic = 0xDEADBEEF;
                                err.type = 4;
                                std::strncpy(err.targetUser, header.senderUser, sizeof(err.targetUser) - 1);
                                err.targetUser[sizeof(err.targetUser) - 1] = '\0';
                                std::strncpy(err.fileName, "无法保存文件", sizeof(err.fileName) - 1);
                                err.fileName[sizeof(err.fileName) - 1] = '\0';
                                std::strncpy(err.senderUser, m_userName.toUtf8().constData(), sizeof(err.senderUser) - 1);
                                err.senderUser[sizeof(err.senderUser) - 1] = '\0';

                                m_socket->write(reinterpret_cast<const char*>(&err), sizeof(err));

                                // 清理资源
                                delete m_receivingFile->file;
                                m_receivingFile = nullptr;
                                m_readBuffer.remove(0, totalFrameSize);
                                continue;
                            }

                            m_receivingFile->totalSize = totalSize;
                            m_receivingFile->receivedBytes = 0;
                            m_receivingFile->senderUser = sender;

                            // ✅ 修复3：ACK帧所有数字字段不转网络序，手动加字符串结束符（💡 修复：去掉了 htonl）
                            FileTransferHeader ack{};
                            ack.magic = 0xDEADBEEF;
                            ack.type = 2; // 进度确认
                            std::strncpy(ack.targetUser, header.senderUser, sizeof(ack.targetUser) - 1);
                            ack.targetUser[sizeof(ack.targetUser) - 1] = '\0';
                            std::strncpy(ack.senderUser, m_userName.toUtf8().constData(), sizeof(ack.senderUser) - 1);
                            ack.senderUser[sizeof(ack.senderUser) - 1] = '\0';

                            m_socket->write(reinterpret_cast<const char*>(&ack), sizeof(ack));
                            AppendMessage("系统", QString("正在接收 %1 发来的文件...").arg(sender));
                            ui->statusbar->showMessage("准备接收文件");
                        }
                    }
                    // 2. 收到文件数据 (作为接收方)
                    else if (type == 1 && m_receivingFile && m_receivingFile->file) {
                        // ✅ 修复4：增加端到端CRC32校验（保证数据完整性）
                        const uint32_t calculatedCRC = CalculateCRC32(frameData, chunkSize);
                        if (calculatedCRC != crc32) {
                            AppendMessage("系统", QString("❌ 文件块%1校验失败，传输中断").arg(chunkIndex));

                            // 发送错误帧（💡 修复：去掉了 htonl）
                            FileTransferHeader err{};
                            err.magic = 0xDEADBEEF;
                            err.type = 4;
                            std::strncpy(err.targetUser, header.senderUser, sizeof(err.targetUser) - 1);
                            err.targetUser[sizeof(err.targetUser) - 1] = '\0';
                            std::strncpy(err.fileName, "数据校验失败", sizeof(err.fileName) - 1);
                            err.fileName[sizeof(err.fileName) - 1] = '\0';
                            std::strncpy(err.senderUser, m_userName.toUtf8().constData(), sizeof(err.senderUser) - 1);
                            err.senderUser[sizeof(err.senderUser) - 1] = '\0';

                            m_socket->write(reinterpret_cast<const char*>(&err), sizeof(err));

                            // 清理资源
                            m_receivingFile->file->close();
                            delete m_receivingFile->file;
                            m_receivingFile = nullptr;
                            m_readBuffer.remove(0, totalFrameSize);
                            continue;
                        }

                        // 校验通过再写入文件
                        m_receivingFile->file->write(frameData, chunkSize);
                        m_receivingFile->receivedBytes += chunkSize;

                        // ✅ 新增：更新接收进度
                        const float progress = (m_receivingFile->receivedBytes * 100.0f) / m_receivingFile->totalSize;
                        ui->statusbar->showMessage(QString("正在接收文件: %1%").arg(static_cast<int>(progress)));
                        FileTransferHeader ack{};
                        ack.magic = 0xDEADBEEF;
                        ack.type = 2; // 进度确认
                        // 把发送者的名字作为目标填入，让服务器精准投递回去
                        std::strncpy(ack.targetUser, header.senderUser, sizeof(ack.targetUser) - 1);
                        ack.targetUser[sizeof(ack.targetUser) - 1] = '\0';
                        std::strncpy(ack.senderUser, m_userName.toUtf8().constData(), sizeof(ack.senderUser) - 1);
                        ack.senderUser[sizeof(ack.senderUser) - 1] = '\0';
                        m_socket->write(reinterpret_cast<const char*>(&ack), sizeof(ack));
                        // ==================================================================
                    }
                    // 3. 收到发送确认 (作为发送方)
                    else if (type == 2) {
                        OnFileAcknowledged();
                    }
                    // 4. 收到文件完成指令 (作为接收方)
                    else if (type == 3 && m_receivingFile) {
                        m_receivingFile->file->close();
                        delete m_receivingFile->file;
                        AppendMessage(
                            "系统",
                            QString("✅ 成功接收来自 %1 的文件\n已保存至: %2")
                                .arg(sender)
                                .arg(m_receivingFile->filePath)
                            );
                        ui->statusbar->showMessage("文件接收完成", 3000);
                        m_receivingFile = nullptr;
                    }
                    // ✅ 新增：处理错误帧
                    else if (type == 4) {
                        const QString errorMsg = QString::fromUtf8(header.fileName);
                        AppendMessage("系统", QString("❌ 传输失败：%1").arg(errorMsg));

                        // 清理所有传输资源
                        if (m_sendingFile) {
                            if (m_sendingFile->file) {
                                m_sendingFile->file->close();
                                delete m_sendingFile->file;
                            }
                            if (m_sendingFile->progressDialog) {
                                m_sendingFile->progressDialog->close();
                            }
                            m_sendingFile = nullptr;
                        }
                        if (m_receivingFile) {
                            if (m_receivingFile->file) {
                                m_receivingFile->file->close();
                                delete m_receivingFile->file;
                            }
                            m_receivingFile = nullptr;
                        }
                        ui->statusbar->showMessage("传输失败", 3000);
                    }

                    // 移除已处理的帧数据
                    m_readBuffer.remove(0, totalFrameSize);
                    continue;
                }
            }

            // 不是文件帧，按文本协议处理
            const int nlIndex = m_readBuffer.indexOf('\n');
            if (nlIndex < 0) break;

            const QByteArray packetData = m_readBuffer.left(nlIndex);
            m_readBuffer.remove(0, nlIndex + 1);

            // 处理JSON协议
            if (packetData.startsWith('{')) {
                QJsonParseError jsonError;
                const QJsonDocument doc = QJsonDocument::fromJson(packetData, &jsonError);

                if (jsonError.error == QJsonParseError::NoError && doc.isObject()) {
                    const QJsonObject obj = doc.object();
                    if (obj.value("type").toString() == "file") {
                        const QString targetUser = obj.value("receiver").toString().trimmed();
                        if (targetUser == m_userName.trimmed()) {
                            // 这里保留你原来的JSON文件处理逻辑
                        }
                        continue;
                    }
                }
            }

            // 处理普通文本消息和用户列表
            const QString message = QString::fromUtf8(packetData);
            if (message.startsWith("[USER_LIST]:")) {
                const QString userListStr = message.mid(12);
                const QStringList users = userListStr.split(",");
                ui->listOnlineUsers->clear();
                ui->listOnlineUsers->addItems(users);
            } else {
                AppendMessage("", message);
            }
        }
    });
    // 连接断开后回到登录页，方便用户修改 IP/端口后重新连接。
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        AppendMessage("系统", "服务器连接已断开。");
        // 清理发送资源（之前已经加了）
        if (m_sendingFile) {
            if (m_sendingFile->file) {
                m_sendingFile->file->close();
                delete m_sendingFile->file;
            }
            if (m_sendingFile->progressDialog) {
                m_sendingFile->progressDialog->close();
            }
            m_sendingFile = nullptr;
        }
        // 【新增】清理接收资源
        if (m_receivingFile) {
            if (m_receivingFile->file) {
                m_receivingFile->file->close();
                delete m_receivingFile->file;
            }
            m_receivingFile = nullptr;
        }
        SetConnectedState(false);
        ui->stackedWidget->setCurrentWidget(ui->LoginPage);
    });

    // Qt 5.15 以后信号名是 errorOccurred；旧版本仍然叫 error。
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        QMessageBox::warning(this, "网络错误", m_socket->errorString());
        SetConnectedState(false);
    });
#else
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [this](QAbstractSocket::SocketError) {
        QMessageBox::warning(this, "网络错误", m_socket->errorString());
        SetConnectedState(false);
    });
#endif
}

void MainWindow::AppendMessage(const QString &sender, const QString &message)
{
    // 统一聊天记录格式，方便后续替换成气泡消息或富文本。
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    ui->textChatRecord->append(QString("[%1] %2：%3").arg(time, sender, message));
    ui->textChatRecord->moveCursor(QTextCursor::End);
}

void MainWindow::SetConnectedState(bool connected)
{
    // 所有“连接状态相关”的控件都集中在这里更新，避免不同信号槽里重复写。
    ui->btnConnect->setEnabled(!connected);
    ui->btnConnect->setText(connected ? "已连接" : "连接服务器");
    ui->btnDisconnect->setEnabled(connected);
    ui->btnSend->setEnabled(connected);
    ui->statusbar->showMessage(connected ? "已连接服务器" : "未连接服务器");
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // 登录页、注册页、聊天页都允许按住鼠标左键拖拽整个无边框窗口。
    if (event->button() == Qt::LeftButton)
    {
        m_dragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
#else
        m_dragPosition = event->globalPos() - frameGeometry().topLeft();
#endif
        event->accept();
        return;
    }

    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton) && !isMaximized())
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        move(event->globalPosition().toPoint() - m_dragPosition);
#else
        move(event->globalPos() - m_dragPosition);
#endif
        event->accept();
        return;
    }

    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QMainWindow::mouseReleaseEvent(event);
}

// ⭐【新增】发送文件实现
void MainWindow::SendFileImpl(const QString& targetUser, const QString& filePath)
{
    QFileInfo fileInfo(filePath);
    uint32_t fileSize = fileInfo.size();

    // 创建发送文件记录
    m_sendingFile = std::make_shared<SendingFile>();
    m_sendingFile->filePath = filePath;
    m_sendingFile->totalSize = fileSize;
    m_sendingFile->sentBytes = 0;
    m_sendingFile->chunkIndex = 0;
    m_sendingFile->targetUser = targetUser;
    m_sendingFile->initialized = false;  // ⭐【新增】等待服务器确认

    // 打开文件
    m_sendingFile->file = new QFile(filePath);
    if (!m_sendingFile->file->open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "错误", "无法读取文件！");
        delete m_sendingFile->file;
        m_sendingFile = nullptr;
        return;
    }

    // 创建进度对话框
    m_sendingFile->progressDialog = new QProgressDialog(this);
    m_sendingFile->progressDialog->setAttribute(Qt::WA_DeleteOnClose); // 【新增】关闭时自动销毁
    m_sendingFile->progressDialog->setWindowTitle(QString("发送文件给 %1").arg(targetUser));
    m_sendingFile->progressDialog->setMaximum(fileSize);
    m_sendingFile->progressDialog->setValue(0);
    m_sendingFile->progressDialog->setAutoClose(false);
    m_sendingFile->progressDialog->show();

    // 1. 发送文件请求帧（type=0）
    FileTransferHeader reqHeader{};
    reqHeader.magic = 0xDEADBEEF;
    reqHeader.type = 0; // 文件请求
    reqHeader.totalSize = fileSize;
    reqHeader.chunkSize = 0;
    reqHeader.chunkIndex = 0;
    reqHeader.crc32 = 0;

    std::strncpy(reqHeader.fileName, fileInfo.fileName().toUtf8().constData(), sizeof(reqHeader.fileName) - 1);
    std::strncpy(reqHeader.targetUser, targetUser.toUtf8().constData(), sizeof(reqHeader.targetUser) - 1);
    std::strncpy(reqHeader.senderUser, m_userName.toUtf8().constData(), sizeof(reqHeader.senderUser) - 1);
    reqHeader.senderUser[sizeof(reqHeader.senderUser) - 1] = '\0'; // 安全截断
    // ====================================================================

    QByteArray frame(reinterpret_cast<const char*>(&reqHeader), sizeof(FileTransferHeader));
    m_socket->write(frame);

    std::cout << "【开始发送文件】" << std::endl;
    std::cout << "  目标用户: " << targetUser.toStdString() << std::endl;
    std::cout << "  文件名: " << fileInfo.fileName().toStdString() << std::endl;
    std::cout << "  大小: " << (fileSize / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "  等待服务器确认..." << std::endl;
}

// ⭐【新增】发送单个数据块
void MainWindow::SendFileChunk()
{
    if (!m_sendingFile || !m_sendingFile->file) {
        return;
    }

    // 读取数据块
    const int CHUNK_SIZE = 262144; // 256KB
    char buffer[CHUNK_SIZE];
    int toRead = std::min(CHUNK_SIZE, (int)(m_sendingFile->totalSize - m_sendingFile->sentBytes));

    if (toRead <= 0) {
        return; // 已读完
    }

    int read = m_sendingFile->file->read(buffer, toRead);
    if (read <= 0) {
        QMessageBox::critical(this, "错误", "读文件失败！");
        m_sendingFile->file->close();
        delete m_sendingFile->file;
        m_sendingFile = nullptr;
        return;
    }

    // 计算CRC32
    uint32_t crc = CalculateCRC32(buffer, read);

    // 构造数据帧
    FileTransferHeader dataHeader{};
    dataHeader.magic = 0xDEADBEEF;
    dataHeader.type = 1; // 文件数据
    dataHeader.totalSize = m_sendingFile->totalSize;
    dataHeader.chunkSize = read;
    dataHeader.chunkIndex = m_sendingFile->chunkIndex;
    dataHeader.crc32 = crc;
    // ⭐【就在这里加上这两行】
    std::strncpy(dataHeader.targetUser, m_sendingFile->targetUser.toUtf8().constData(), sizeof(dataHeader.targetUser) - 1);
    std::strncpy(dataHeader.senderUser, m_userName.toUtf8().constData(), sizeof(dataHeader.senderUser) - 1);
    std::strncpy(dataHeader.fileName, 
                 QFileInfo(m_sendingFile->filePath).fileName().toStdString().c_str(),
                 sizeof(dataHeader.fileName) - 1);

    // 发送帧头 + 数据
    QByteArray frame(reinterpret_cast<const char*>(&dataHeader), sizeof(FileTransferHeader));
    frame.append(buffer, read);
    m_socket->write(frame);

    m_sendingFile->sentBytes += read;
    m_sendingFile->chunkIndex++;

    // 更新进度条
    if (m_sendingFile->progressDialog) {
        m_sendingFile->progressDialog->setValue(m_sendingFile->sentBytes);
        float progress = (m_sendingFile->sentBytes * 100.0f) / m_sendingFile->totalSize;
        ui->statusbar->showMessage(QString("正在发送文件: %1% (%2 / %3 MB)")
            .arg((int)progress)
            .arg(m_sendingFile->sentBytes / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(m_sendingFile->totalSize / 1024.0 / 1024.0, 0, 'f', 1));
    }

    // 如果还有数据，继续发送
    if (m_sendingFile->sentBytes >= m_sendingFile->totalSize) {

        // 发送完成帧
        FileTransferHeader completeHeader{};
        completeHeader.magic = 0xDEADBEEF;
        completeHeader.type = 3; // 完成
        completeHeader.totalSize = m_sendingFile->totalSize;
        completeHeader.chunkSize = 0;
        completeHeader.chunkIndex = m_sendingFile->chunkIndex;
        completeHeader.crc32 = 0;
        std::strncpy(completeHeader.targetUser, m_sendingFile->targetUser.toUtf8().constData(), sizeof(completeHeader.targetUser) - 1);
        std::strncpy(completeHeader.senderUser, m_userName.toUtf8().constData(), sizeof(completeHeader.senderUser) - 1);
        std::strncpy(completeHeader.fileName,
                     QFileInfo(m_sendingFile->filePath).fileName().toStdString().c_str(),
                     sizeof(completeHeader.fileName) - 1);

        QByteArray completeFrame(reinterpret_cast<const char*>(&completeHeader), sizeof(FileTransferHeader));
        m_socket->write(completeFrame);

        m_sendingFile->file->close();
        delete m_sendingFile->file;
        m_sendingFile->file = nullptr;

        if (m_sendingFile->progressDialog) {
            m_sendingFile->progressDialog->setValue(m_sendingFile->totalSize);
            m_sendingFile->progressDialog->setLabelText("文件发送完成！");
            // ⭐【改进】3 秒后自动关闭对话框
            QTimer::singleShot(3000, m_sendingFile->progressDialog, &QProgressDialog::close);
        }

        std::cout << "【文件发送完成】" << std::endl;
        AppendMessage("我", QString("📁 已完成发送文件给 [%1]").arg(m_sendingFile->targetUser));

        // ⭐【改进】清理资源（注意：progressDialog 会自动 deleteLater）
        m_sendingFile = nullptr;
    }
}

// ⭐【新增】处理服务器的进度确认
void MainWindow::OnFileAcknowledged()
{
    // ⭐【改进】验证发送状态，安全地继续
    if (!m_sendingFile) {
        return; // 没有正在进行的文件传输
    }

    if (!m_sendingFile->file || !m_sendingFile->file->isOpen()) {
        return; // 文件已关闭
    }

    // ⭐【新增】如果还未初始化，标记为已初始化，然后立即开始发送
    if (!m_sendingFile->initialized) {
        m_sendingFile->initialized = true;
        std::cout << "  服务器已确认接收" << std::endl;
        // 立即开始发送第一块
        SendFileChunk();
        return;
    }

    // 检查是否已完成
    if (m_sendingFile->sentBytes >= m_sendingFile->totalSize) {
        return; // 已全部发送完毕
    }

    // 继续发送下一块
    SendFileChunk();
}

// ⭐【新增】CRC32 计算
uint32_t MainWindow::CalculateCRC32(const char* data, uint32_t size)
{
    static uint32_t crc32Table[256] = {0};
    static bool initialized = false;

    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
            crc32Table[i] = crc;
        }
        initialized = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ crc32Table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}
