#pragma once

#include <unordered_set>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <memory>

// ⭐【新增】文件传输配置常量
constexpr int FILE_CHUNK_SIZE = 262144; // 256KB 单个数据块大小
constexpr int MAX_SEND_BUFFER = 5242880; // 5MB 发送缓冲区上限
constexpr int MAX_RECV_BUFFER = 5242880; // 5MB 接收缓冲区上限

// ⭐【新增】二进制协议帧头结构（所有文件传输都用这个）
#pragma pack(1)
struct FileTransferHeader {
    uint32_t magic;        // 0xDEADBEEF - 魔数标识
    uint32_t type;         // 协议类型：0=文件请求、1=文件数据、2=进度确认、3=完成、4=错误
    uint32_t totalSize;    // 文件总大小
    uint32_t chunkSize;    // 本次数据块大小
    uint32_t chunkIndex;   // 第几个数据块（从0开始）
    uint32_t crc32;        // 本数据块的CRC32校验码
    char fileName[256];    // 文件名（null终止）
    char targetUser[32];   // ⭐【必须新增】目标接收方的用户名
    char senderUser[64];   // ⭐【两边都新增这一行】发送方用户名
};
#pragma pack()

class ChatServer
{
private:
    // 监听 socket：只负责 accept 新连接，不直接收发聊天消息。
    int m_listenfd;

    // epoll 实例 fd：由它统一管理监听 socket 和所有客户端 socket。
    int m_epfd;

    // 当前在线客户端集合。
    std::unordered_set<int> m_clients;

    // ⭐【新增优化】在线用户哈希表：实现名字与管道的精准双向映射
    std::unordered_map<int, std::string> m_fdToUser; // 通过 fd 找 用户名
    std::unordered_map<std::string, int> m_userToFd; // 通过 用户名 找 fd

    // ⭐【新增优化】每个客户端的专属接收缓冲区：彻底解决大文件 TCP 粘包/断包问题
    std::unordered_map<int, std::string> m_clientBuffers;

    // ⭐【新增优化】每个客户端的专属发送缓冲区：解决非阻塞 send() 大文件部分发送导致数据丢失/乱码
    std::unordered_map<int, std::string> m_clientSendBuffers;

    // ⭐【新增】每个客户端的接收缓冲区当前大小（用于背压控制）
    std::unordered_map<int, uint64_t> m_recvBufferSize;

    // ⭐【新增】每个客户端的发送缓冲区当前大小（用于背压控制）
    std::unordered_map<int, uint64_t> m_sendBufferSize;

    // ⭐【新增】正在接收的文件信息：fd -> {文件名, 总大小, 已接收字节数, 目标用户}
    struct RecvingFile {
        std::string fileName;
        uint32_t totalSize;
        uint32_t receivedBytes;
        std::string targetUser;
        std::string tempFileName; // 临时文件路径
    };
    std::unordered_map<int, std::shared_ptr<RecvingFile>> m_recvingFiles;

    // 将 socket 设置为非阻塞。
    bool SetNonBlocking(int fd);

    // ⭐【新增】带缓冲的发送函数：尽量直接发送，未发完的自动存入缓冲区并注册 EPOLLOUT 等待可写时续发
    void SendData(int fd, const std::string& data);

    // ⭐【新增】尝试刷新指定客户端的发送缓冲区（由 EPOLLOUT 事件触发回调）
    void TryFlushSendBuffer(int fd);

    // ⭐【新增】发送二进制帧（用于文件传输协议）
    void SendFrame(int fd, const FileTransferHeader& header, const char* data, uint32_t dataSize);

    // 从 epoll 和在线集合中移除客户端，并关闭对应 socket。
    void RemoveClient(int fd);

    // ⭐【新增优化】从哈希表中获取所有在线用户，拼接并广播最新的用户列表给所有人
    void BroadcastUserList();

    // ⭐【新增优化】处理一条从缓冲区切分出来的完整应用层协议数据包
    void ProcessPacket(int fd, const std::string& packet);

    // ⭐【新增】处理文件传输帧
    void ProcessFileFrame(int fd, const FileTransferHeader& header, const char* data, uint32_t dataSize);

    // ⭐【新增】计算CRC32校验码
    uint32_t CalculateCRC32(const char* data, uint32_t size);

public:
    ChatServer();
    ~ChatServer();

    // 初始化监听 socket、绑定端口、开始监听，并注册到 epoll。
    bool Init(int port);

    // epoll 事件循环：等待连接事件和读事件，然后分发处理。
    void Run();

    // 处理新客户端连接。
    void HandleAccept();

    // 处理某个客户端发来的数据。
    void HandleRead(int fd);

    // 将 fromfd 发来的消息广播给其他所有在线客户端。
    void Broadcast(int fromfd, const char* data, int len);
};