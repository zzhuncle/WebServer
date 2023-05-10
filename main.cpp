#include"locker.h"
#include"threadpool.h"
#include"http_conn.h"
#include<iostream>
#include<signal.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<error.h>
#include<fcntl.h>
#include<sys/epoll.h>
#define MAX_FD 65535        // 最大文件描述符个数
#define MAX_EVENT_NUM 10000 // 监听的最大事件数量

// 添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    // 注册信号的参数
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool oneshot);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 从epoll中修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

// 主线程
int main(int argc, char* argv[])
{
    // 至少要传递端口号
    if (argc <= 1) {
        std::cout << "按照如下格式运行：" << basename(argv[0]) << " port_number" << std::endl;
    }
    // 获取端口号
    int port = atoi(argv[1]);
    // 对SIGPIPE信号做处理
    addsig(SIGPIPE, SIG_IGN); // 忽略SIGPIPE信号
    // 创建线程池，初始化线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    // 创建数组用于所有客户端信息
    http_conn* users = new http_conn[MAX_FD];
    // 创建监听的套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    // 绑定之前设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    listen(listenfd, 5); // 5 未连接的和已经连接的和的最大值

    // 创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUM];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 主线程循环检测有没有哪些事件发生
    while (true) {
        // 检测函数
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUM, -1);
        if ((num < 0) && (errno != EINTR)) {
            std::cout << "epoll failure" << std::endl;
            break;
        }
        // 循环遍历事件数组
        for (int i = 0;i < num;i++) {
            int sockfd = events[i].data.fd;
            // 有客户端连接进来
            if (sockfd == listenfd) { 
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                // 接收客户端连接,默认是一个阻塞的函数,阻塞等待客户端连接
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                // 判断
                if (http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数满，给客户端写信息：服务器内部忙
                    close(connfd);
                    continue;
                }
                // 将新的客户数据初始化，放到数组中
                users[connfd].init(connfd, client_address);
            } 
            // 对方异常断开或者错误事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockfd].close_conn();
            } 
            else if (events[i].events & EPOLLIN) {
                // 一次性将所有数据读出来
                if (users[sockfd].read()) {
                    // 将任务追加到线程池中
                    pool->append(&users[sockfd]);
                } else {
                    users[sockfd].close_conn();
                }
            } 
            else if (events[i].events & EPOLLOUT) {
                // 一次性将所有数据写出去
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
            
        }
    }
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}