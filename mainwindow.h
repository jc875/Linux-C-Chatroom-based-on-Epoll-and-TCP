#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPoint>
#include <QTcpSocket>
#include <QProgressDialog>
#include<QFile>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// ⭐【新增】二进制文件传输协议帧头
#pragma pack(1)
struct FileTransferHeader {
    uint32_t magic;        // 0xDEADBEEF
    uint32_t type;         // 0=请求, 1=数据, 2=确认, 3=完成, 4=错误
    uint32_t totalSize;    // 文件总大小
    uint32_t chunkSize;    // 本块数据大小
    uint32_t chunkIndex;   // 块索引
    uint32_t crc32;        // CRC32校验
    char fileName[256];    // 文件名
    char targetUser[64];  // 目标用户名
    char senderUser[32];   // ⭐【必须新增】谁发的 (接收方需要靠这个回ACK)
};
#pragma pack()

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    Ui::MainWindow *ui;
    QTcpSocket *m_socket;
    QString m_userName;
    bool m_dragging;
    QPoint m_dragPosition;

    void InitUi();
    void InitSocket();
    void AppendMessage(const QString &sender, const QString &message);
    void SetConnectedState(bool connected);

    QByteArray m_readBuffer;

    // ⭐【新增】文件传输相关
    struct SendingFile {
        QString filePath;
        QFile* file;
        uint32_t totalSize;
        uint32_t sentBytes;
        uint32_t chunkIndex;
        QString targetUser;
        QProgressDialog* progressDialog;
        bool initialized = false;  // ⭐【新增】标记是否收到服务器确认
    };

    std::shared_ptr<SendingFile> m_sendingFile;
    struct ReceivingFile {
        QString filePath;
        QFile* file = nullptr;
        uint32_t totalSize = 0;
        uint32_t receivedBytes = 0;
        QString senderUser;
    };
    std::shared_ptr<ReceivingFile> m_receivingFile;

    void SendFileImpl(const QString& targetUser, const QString& filePath);
    void SendFileChunk();
    void OnFileAcknowledged();
    uint32_t CalculateCRC32(const char* data, uint32_t size);
};
#endif // MAINWINDOW_H
