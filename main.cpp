#include "ChatServer.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[])
{
    // 默认监听 8888 端口；运行时也可以通过命令行参数指定端口。
    int port = 8888;

    if (argc >= 2)
    {
        port = std::atoi(argv[1]);
    }

    // main 只负责启动服务器，具体的 socket 和 epoll 逻辑都封装在 ChatServer 中。
    ChatServer server;

    if (!server.Init(port))
    {
        std::cerr << "Server init failed." << std::endl;
        return 1;
    }

    std::cout << "Chat server is running on port " << port << std::endl;

    // 进入 epoll 主循环后，服务器会一直等待并处理客户端事件。
    server.Run();

    return 0;
}
