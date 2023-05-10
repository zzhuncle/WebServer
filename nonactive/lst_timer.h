#include<stdio.h>
#include<time.h>
#include<arpa/inet.h>

#define BUFFER_SIZE 64
class util_timer;

// 用户数据结构
struct client_data
{
    sockaddr_in address;                 // 客户端socket地址
    int         sockfd;                  // socket文件描述符
    char        buf[BUFFER_SIZE];        // 读缓存
    util_timer* timer;                   // 定时器
};

