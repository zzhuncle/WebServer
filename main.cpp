#include"log.h"
#include"locker.h"
#include"sql_connection_pool.h"
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
#include<iostream>

#define MAX_FD        65535  // 最大文件描述符个数
#define MAX_EVENT_NUM 10000  // 监听的最大事件数量
static int pipefd[2];        // 管道文件描述符 0为读，1为写

// 向管道写数据的信号捕捉回调函数
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// 添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    // 注册信号的参数
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = 0;
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

// 设置文件非阻塞
extern void setnonblocking(int fd);

// 初始化日志
int m_close_log = 0;

// 主线程
int main(int argc, char* argv[])
{
    // 至少要传递端口号
    if (argc <= 1) {
        LOG_ERROR("run as: %s port_number\n", basename(argv[0]));
        return 0;
    }
    // 获取端口号
    int port = atoi(argv[1]);
    // 对SIGPIPE信号做处理
    addsig(SIGPIPE, SIG_IGN); // 忽略SIGPIPE信号 https://blog.csdn.net/chengcheng1024/article/details/108104507
    
    // 初始化日志
    Log::get_instance()->init("./ServerLog", m_close_log, 2000, 10000, 800);

    // 初始化数据库
    string user = "root";
    string passwd = "root";
    string database_name = "yourdb";
    connection_pool::get_instance()->init("localhost", user, passwd, database_name, 3306, 8);

    // 创建线程池，初始化线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    // 创建数组用于所有客户端信息
    http_conn* users = new http_conn[MAX_FD];

    // 初始化数据库读取表
    users->init_mysql_result(connection_pool::get_instance());

    // 创建监听的套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    // 绑定之前设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int res = bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    res = listen(listenfd, 5); // 5 未连接的和已经连接的和的最大值

    // 创建epoll对象，事件数组，添加
    // 服务器通过epoll这种IO复用技术来实现对监听socket和连接socket的同时监听
    // epoll在不设置EPOLLET时，默认的事件触发模式为水平触发模式
    // socket在不设置非阻塞（nonblock）参数时，默认是阻塞的
    epoll_event events[MAX_EVENT_NUM];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中
    // 设置fd非阻塞 并且oneshot
    // 当listen到新的客户连接时，listenfd变为就绪事件
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道
    res = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblocking(pipefd[1]);        // 写管道非阻塞
    addfd(epollfd, pipefd[0], false); // epoll检测读管道

    // 设置信号处理函数
    addsig(SIGALRM, sig_handler);     // 定时器信号
    addsig(SIGTERM, sig_handler);     // SIGTERM 关闭服务器
    bool stop_server = false;         // 关闭服务器标志位
    bool timeout     = false;         // 定时器周期已到
    alarm(TIMESLOT);                  // 定时产生SIGALRM信号

    // 采用同步I/O模拟的Proactor事件处理模式
    // Proactor模式：将所有的I/O操作都交给主线程和内核来处理（进行读、写），
    // 工作线程仅负责处理逻辑，如主线程读完成后users[sockfd].read()，
    // 选择一个工作线程来处理客户请求pool->append(users + sockfd)。

    // 主线程循环检测有没有哪些事件发生
    while (true) {
        // 检测函数
        // 主线程调用epoll_wait等待一组文件描述符上的事件，
        // 并将当前所有就绪的epoll_event复制到events数组中
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUM, -1);                
        if ((num < 0) && (errno != EINTR)) {
            LOG_ERROR("EPOLL failed.\n");
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
                users[connfd].init(connfd, client_address, user, passwd, database_name);
            } 
            // 读管道有数据，SIGALRM 或 SIGTERM信号触发
            else if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) {  
                int sig;
                char signals[1024];
                res = recv(pipefd[0], signals, sizeof(signals), 0);
                if (res == -1) {
                    continue;
                } else if(res == 0) {
                    continue;
                } else {
                    for(int i = 0; i < res; i++) {
                        switch (signals[i]) {
                            case SIGALRM:
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                        }
                    }
                }
            }
            // 对方异常断开或者错误事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                LOG_INFO("-------EPOLLRDHUP | EPOLLHUP | EPOLLERR--------\n");
                users[sockfd].close_conn();
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);      // 移除其对应的定时器
            } 
            else if (events[i].events & EPOLLIN) {
                LOG_INFO("-------EPOLLIN-------\n");
                // 一次性将所有数据读出来
                if (users[sockfd].read()) {
                    // 将任务追加到线程池中
                    pool->append(&users[sockfd]);
                } else {
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            } 
            else if (events[i].events & EPOLLOUT) {
                LOG_INFO("-------EPOLLOUT--------\n");
                // 一次性将所有数据写出去
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级
        if (timeout) {
            http_conn::m_timer_lst.tick(); // 定时处理任务，实际上就是调用tick()函数
            // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号
            alarm(TIMESLOT);
            timeout = false;             // 重置timeout
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;
    return 0;
}