#include "ChatServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>

namespace
{
// listen 的等待队列长度。
constexpr int kBacklog = 128;

// epoll_wait 一次最多取出的事件数量。
constexpr int kMaxEvents = 1024;

// 单次 recv 读取缓冲区大小。
constexpr int kBufferSize = 1024;

// ⭐【新增】CRC32查表
uint32_t g_crc32Table[256];

// ⭐【新增】初始化CRC32查表
void InitCRC32Table()
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        g_crc32Table[i] = crc;
    }
}
}

ChatServer::ChatServer()
    : m_listenfd(-1),
      m_epfd(-1)
{
}

ChatServer::~ChatServer()
{
    // 服务器退出时关闭所有客户端连接。
    for (int fd : m_clients)
    {
        close(fd);
    }

    if (m_listenfd >= 0)
    {
        close(m_listenfd);
    }

    if (m_epfd >= 0)
    {
        close(m_epfd);
    }
}

bool ChatServer::Init(int port)
{
    // ⭐【新增】初始化CRC32查表
    InitCRC32Table();

    // 创建 TCP socket。
    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenfd < 0)
    {
        perror("socket");
        return false;
    }

    int opt = 1;

    // 允许服务器重启后尽快复用同一个端口，避免 TIME_WAIT 导致 bind 失败。
    if (setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        return false;
    }

    // epoll 服务器通常配合非阻塞 socket 使用。
    if (!SetNonBlocking(m_listenfd))
    {
        return false;
    }

    // 绑定本机所有网卡的指定端口。
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(m_listenfd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return false;
    }

    // 开始监听客户端连接。
    if (listen(m_listenfd, kBacklog) < 0)
    {
        perror("listen");
        return false;
    }

    // 创建 epoll 实例。
    m_epfd = epoll_create1(0);
    if (m_epfd < 0)
    {
        perror("epoll_create1");
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = m_listenfd;

    // 把监听 socket 加入 epoll；当有新连接到来时，epoll_wait 会返回它。
    if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_listenfd, &ev) < 0)
    {
        perror("epoll_ctl listenfd");
        return false;
    }

    return true;
}

void ChatServer::Run()
{
    epoll_event events[kMaxEvents];

    // 单线程事件驱动主循环：所有连接和消息都由 epoll_wait 驱动。
    while (true)
    {
        int n = epoll_wait(m_epfd, events, kMaxEvents, -1);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            uint32_t eventFlags = events[i].events;

            // 监听 socket 可读，表示有新客户端连接。
            if (fd == m_listenfd)
            {
                HandleAccept();
                continue;
            }

            // 客户端异常、挂断或对端关闭时，清理连接。
            if ((eventFlags & EPOLLERR) || (eventFlags & EPOLLHUP) || (eventFlags & EPOLLRDHUP))
            {
                RemoveClient(fd);
                continue;
            }

            // ⭐【新增优化】socket 可写，先尝试刷新发送缓冲区的积压数据
            if (eventFlags & EPOLLOUT)
            {
                TryFlushSendBuffer(fd);
                // 注意：刷完缓冲区后不 continue，因为同一轮可能同时有 EPOLLIN
            }

            // 普通客户端 socket 可读，表示它发送了聊天数据。
            if (eventFlags & EPOLLIN)
            {
                HandleRead(fd);
            }
        }
    }
}

void ChatServer::HandleAccept()
{
    // 监听 socket 是非阻塞的，因此一次事件里尽量 accept 完所有已到达的连接。
    while (true)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);

        int clientfd = accept(m_listenfd, (sockaddr*)(&clientAddr), &clientLen);
        if (clientfd < 0)
        {
            // EAGAIN/EWOULDBLOCK 表示当前已经没有更多连接可 accept。
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }

            perror("accept");
            break;
        }

        // 客户端 socket 也设为非阻塞，避免 recv/send 卡住事件循环。
        if (!SetNonBlocking(clientfd))
        {
            close(clientfd);
            continue;
        }

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = clientfd;

        // 将新客户端注册进 epoll，之后它发消息时会触发 EPOLLIN。
        if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, clientfd, &ev) < 0)
        {
            perror("epoll_ctl clientfd");
            close(clientfd);
            continue;
        }

        // 保存在线客户端 fd，便于后续广播。
        m_clients.insert(clientfd);
        std::cout << "新客户端已连接: fd=" << clientfd << std::endl;
    }
}

void ChatServer::HandleRead(int fd)
{
    char buf[kBufferSize]; // 1024 字节缓冲区
    while (true)
    {
        int len = recv(fd, buf, sizeof(buf), 0);
        if (len > 0)
        {
            // 背压控制
            auto sizeIt = m_recvBufferSize.find(fd);
            uint64_t currentSize = (sizeIt != m_recvBufferSize.end()) ? sizeIt->second : 0;
            if (currentSize > MAX_RECV_BUFFER) {
                std::cerr << "警告：fd=" << fd << " 接收缓冲区已满(" << currentSize << "B)，触发背压" << std::endl;
                continue;
            }

            // 把新收到的零散字节追加到这个客户端的专属缓冲区里
            m_clientBuffers[fd].append(buf, len);
            m_recvBufferSize[fd] = m_clientBuffers[fd].size();

            // ⭐【修复后的核心解析循环】
            while (!m_clientBuffers[fd].empty())
            {
                // 1. 检查是否可能是二进制文件帧（判断前 4 个字节是否是魔数）
                bool isBinaryFrame = false;
                if (m_clientBuffers[fd].size() >= sizeof(uint32_t)) {
                    uint32_t magic = 0;
                    std::memcpy(&magic, m_clientBuffers[fd].data(), sizeof(uint32_t));
                    if (magic == 0xDEADBEEF) {
                        isBinaryFrame = true;
                    }
                }

                if (isBinaryFrame) {
                    // 如果是二进制文件帧，此时必须保证收齐了完整的 FileTransferHeader
                    if (m_clientBuffers[fd].size() < sizeof(FileTransferHeader)) {
                        break; // 头还没收齐，跳出循环等待下次 recv
                    }

                    FileTransferHeader header;
                    std::memcpy(&header, m_clientBuffers[fd].data(), sizeof(FileTransferHeader));

                    // 检查整个数据侦（头 + 数据块）是否收齐
                    int totalFrameSize = sizeof(FileTransferHeader) + header.chunkSize;
                    if (m_clientBuffers[fd].size() < totalFrameSize) {
                        break; // 数据体没收齐，跳出循环等待
                    }

                    // 帧完整，提取数据部分并处理
                    const char* frameData = m_clientBuffers[fd].data() + sizeof(FileTransferHeader);
                    ProcessFileFrame(fd, header, frameData, header.chunkSize);

                    // 擦除已处理的帧
                    m_clientBuffers[fd].erase(0, totalFrameSize);
                    m_recvBufferSize[fd] = m_clientBuffers[fd].size();
                }
                else {
                    // 2. 如果不是二进制文件帧，说明是纯文本协议（登录包、群聊包、或带有\n的JSON流）
                    size_t pos = m_clientBuffers[fd].find('\n');
                    if (pos != std::string::npos) {
                        std::string packet = m_clientBuffers[fd].substr(0, pos);
                        m_clientBuffers[fd].erase(0, pos + 1);
                        m_recvBufferSize[fd] = m_clientBuffers[fd].size();

                        // 交给业务路由处理（这里会成功触发登录和广播）
                        ProcessPacket(fd, packet);
                    }
                    else {
                        // 缓冲区里有文本数据，但还没有遇到换行符，说明一行还没发完
                        break;
                    }
                }
            }
            continue;
        }

        if (len == 0) // 客户端正常断开
        {
            RemoveClient(fd);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) // 数据读完了
        {
            return;
        }

        perror("recv");
        RemoveClient(fd);
        return;
    }
}
// ⭐【新增】带缓冲的发送函数
// 核心思路：尽量直接 send 发送，如果非阻塞 socket 缓冲区满导致部分发送，
//           自动将剩余数据存入 m_clientSendBuffers[fd]，并注册 EPOLLOUT 事件，
//           待 socket 可写时由 TryFlushSendBuffer() 续发。
// 这样彻底解决了大文件 Base64 数据在非阻塞 socket 上部分发送导致的数据丢失/乱码问题。
void ChatServer::SendData(int fd, const std::string& data)
{
    // ⭐【新增】背压检查：如果缓冲区大小超过上限，则暂不发送新数据
    auto sizeIt = m_sendBufferSize.find(fd);
    uint64_t currentSize = (sizeIt != m_sendBufferSize.end()) ? sizeIt->second : 0;
    if (currentSize > MAX_SEND_BUFFER) {
        std::cerr << "警告：fd=" << fd << " 发送缓冲区已满(" << currentSize << "B)，新数据被丢弃" << std::endl;
        return;
    }

    // 如果该 fd 已有积压数据未发完，直接追加到缓冲区尾部（保证发送顺序）
    auto it = m_clientSendBuffers.find(fd);
    if (it != m_clientSendBuffers.end() && !it->second.empty()) {
        it->second.append(data);
        m_sendBufferSize[fd] += data.size();
        return;
    }

    // 无积压，尝试直接发送
    int totalLen = static_cast<int>(data.size());
    int sent = 0;
    while (sent < totalLen) {
        int n = send(fd, data.data() + sent, totalLen - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += n;
            continue;
        }
        // 非阻塞 socket 发送缓冲区满，跳出循环，剩余数据进入缓冲
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        // 真正的错误（如连接已断开），丢弃剩余数据
        perror("send");
        return;
    }

    // 如果有剩余未发送的数据，存入缓冲区并注册 EPOLLOUT 等待续发
    if (sent < totalLen) {
        m_clientSendBuffers[fd] = data.substr(sent);
        m_sendBufferSize[fd] += (totalLen - sent);
        // 动态注册 EPOLLOUT，让 epoll 在 socket 可写时通知我们
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

// ⭐【新增】计算CRC32校验码
uint32_t ChatServer::CalculateCRC32(const char* data, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ g_crc32Table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// ⭐【新增】发送二进制帧：包含协议头 + 数据
// 格式：[FileTransferHeader(sizeof=XXX)] + [data(dataSize)]
void ChatServer::SendFrame(int fd, const FileTransferHeader& header, const char* data, uint32_t dataSize)
{
    // 先发送头
    std::string frameData;
    frameData.append(reinterpret_cast<const char*>(&header), sizeof(FileTransferHeader));
    if (data && dataSize > 0) {
        frameData.append(data, dataSize);
    }
    SendData(fd, frameData);
}

// ⭐【新增】尝试刷新指定客户端的发送缓冲区（由 EPOLLOUT 事件触发回调）
void ChatServer::TryFlushSendBuffer(int fd)
{
    auto it = m_clientSendBuffers.find(fd);
    if (it == m_clientSendBuffers.end() || it->second.empty()) {
        return;
    }

    std::string& buffer = it->second;
    int totalLen = static_cast<int>(buffer.size());
    int sent = 0;

    while (sent < totalLen) {
        int n = send(fd, buffer.data() + sent, totalLen - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // 缓冲区又满了，下次 EPOLLOUT 再续发
        }
        // 发送出错（连接已断开等），清理缓冲区并移除客户端
        perror("send (flush)");
        RemoveClient(fd);
        return;
    }

    // 更新缓冲区大小统计
    if (m_sendBufferSize.find(fd) != m_sendBufferSize.end()) {
        m_sendBufferSize[fd] = std::max(0UL, m_sendBufferSize[fd] - sent);
    }

    if (sent >= totalLen) {
        // 全部发送完毕，清除缓冲区，并移除 EPOLLOUT 事件（不再监听可写）
        m_clientSendBuffers.erase(it);
        m_sendBufferSize[fd] = 0;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &ev);
    } else {
        // 仍有剩余，只擦除已发送部分，保留未发数据等待下次 EPOLLOUT
        buffer.erase(0, sent);
    }
}

void ChatServer::Broadcast(int fromfd, const char* data, int len)
{
    // ⭐【修复】遍历所有客户端（排除发送者），使用带缓冲的 SendData 发送
    // 修复原代码中 EAGAIN 时 break 会中断后续客户端发送的问题，同时解决大消息部分发送的数据丢失
    for (int fd : m_clients)
    {
        if (fd == fromfd)
        {
            continue;
        }

        // 使用带缓冲的 SendData，由它处理部分发送和积压缓冲逻辑
        SendData(fd, std::string(data, len));
    }
}

bool ChatServer::SetNonBlocking(int fd)
{
    // 先读取原有 flags，再追加 O_NONBLOCK，避免覆盖其他已有标志。
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl F_GETFL");
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl F_SETFL");
        return false;
    }

    return true;
}

void ChatServer::RemoveClient(int fd)
{
    epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr); //
    m_clients.erase(fd); //
    m_clientBuffers.erase(fd); // ⭐ 清理该客户端的专属接收缓冲区
    m_clientSendBuffers.erase(fd); // ⭐【新增】清理该客户端的专属发送缓冲区
    m_recvBufferSize.erase(fd); // ⭐【新增】清理接收缓冲区大小统计
    m_sendBufferSize.erase(fd); // ⭐【新增】清理发送缓冲区大小统计
    m_recvingFiles.erase(fd); // ⭐【新增】清理正在接收的文件信息

    // ⭐【新增优化】如果这个 fd 绑定过用户名，将其从哈希表中移除
    auto it = m_fdToUser.find(fd);
    if (it != m_fdToUser.end()) {
        std::string loggedOutUser = it->second;
        m_userToFd.erase(loggedOutUser);
        m_fdToUser.erase(it);
        std::cout << "用户已离线: " << loggedOutUser << std::endl;

        // 重新广播最新在线列表
        BroadcastUserList();
    }

    close(fd); //
    std::cout << "Client disconnected: fd=" << fd << std::endl; //
}

void ChatServer::ProcessPacket(int fd, const std::string& packet)
{
    if (packet.empty()) return;

    // 协议 1：处理登录数据包 "[login] 用户名"
    if (packet.size() >= 8 && packet.substr(0, 8) == "[login] ") {
        std::string username = packet.substr(8);
        m_fdToUser[fd] = username;
        m_userToFd[username] = fd;
        std::cout << "用户成功上线: " << username << " (fd=" << fd << ")" << std::endl;
        BroadcastUserList();
        return;
    }

    // 协议 2：⭐ 智能 JSON 路由拦截
    // 如果首尾是大括号，说明这极大概率是客户端发来的 JSON 文件流
    if (packet.front() == '{' && packet.back() == '}') {
        std::string searchKey = "\"receiver\":\"";
        size_t pos = packet.find(searchKey);

        // 如果在 JSON 里找到了接收方的 Key
        if (pos != std::string::npos) {
            size_t start = pos + searchKey.length();
            size_t end = packet.find("\"", start); // 找到名字后面的引号

            if (end != std::string::npos) {
                // 巧妙提取出目标接收方的名字
                std::string targetUser = packet.substr(start, end - start);
                std::cout << "【JSON文件路由】收到文件，目标接收方: [" << targetUser << "]" << std::endl;

                // 利用哈希表精准投递
                auto it = m_userToFd.find(targetUser);
                if (it != m_userToFd.end()) {
                    int targetFd = it->second;
                    // ⭐【修复】使用带缓冲的 SendData，防止大文件 Base64 数据在非阻塞 socket 上部分发送导致乱码
                    std::string forwardData = packet + "\n";
                    SendData(targetFd, forwardData);
                    std::cout << "   => 成功定点投递 JSON 流给 " << targetUser << " (fd=" << targetFd << ")" << std::endl;
                }
                else {
                    // ⭐【修复】错误提示也使用 SendData，避免消息丢失
                    std::string errorPacket = "系统：发送失败，目标用户 [" + targetUser + "] 当前不在线。\n";
                    SendData(fd, errorPacket);
                }
                return; // 成功拦截，路由熔断
            }
        }
    }

    // 协议 3：普通的群聊文本消息，原样广播给其他人
    std::string broadcastData = packet + "\n";
    Broadcast(fd, broadcastData.c_str(), broadcastData.length());
}
void ChatServer::ProcessFileFrame(int fd, const FileTransferHeader& header, const char* data, uint32_t dataSize)
{
    // 1. 提取当前帧宣称的“目标接收方”
    std::string targetName(header.targetUser);

    // 2. 在线哈希表中查找目标用户的 fd
    auto it = m_userToFd.find(targetName);
    if (it == m_userToFd.end()) {
        std::cerr << "【文件路由失败】目标用户 [" << targetName
            << "] 不在线，无法转发 type=" << header.type << std::endl;
        // 可选：可以给当前 fd 发回一个 type=4 的错误帧
        return;
    }
    int targetFd = it->second;

    // 3. 打印精简的业务路由日志，方便你观察数据流向
    switch (header.type) {
    case 0:
        std::cout << "【文件传输·请求】" << header.senderUser << " -> " << header.targetUser << " [" << header.fileName << "]" << std::endl;
        break;
    case 1:
        if (header.chunkIndex % 20 == 0) { // 每20块打印一次，防止刷屏
            std::cout << "【文件传输·数据】" << header.senderUser << " -> " << header.targetUser << " | 块索引: " << header.chunkIndex << std::endl;
        }
        break;
    case 2:
        std::cout << "【文件传输·应答】" << header.senderUser << " -> " << header.targetUser << " (接收方已就绪/已确认块)" << std::endl;
        break;
    case 3:
        std::cout << "【文件传输·完成】" << header.senderUser << " -> " << header.targetUser << " 传输结束！" << std::endl;
        break;
    default:
        std::cout << "【文件传输·其他】type=" << header.type << std::endl;
        break;
    }

    // 4. ⭐【核心转发逻辑】不管是请求、数据、还是ACK(type 2)，原封不动直接透传给目标 fd
    SendFrame(targetFd, header, data, dataSize);
}

void ChatServer::BroadcastUserList()
{
    // 拼接所有哈希表里的在线用户名，组成格式：[USER_LIST]:用户A,用户B,用户C\n
    std::string listPacket = "[USER_LIST]:";
    bool first = true;
    for (const auto& pair : m_userToFd) {
        if (!first) {
            listPacket += ",";
        }
        listPacket += pair.first;
        first = false;
    }
    listPacket += "\n"; // 必须带上分界换行符

    // ⭐【修复】广播给当前所有连接的客户端，使用带缓冲的 SendData 防止部分发送
    for (int fd : m_clients) {
        SendData(fd, listPacket);
    }
}
