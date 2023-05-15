#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include"locker.h"
#include"lst_timer.h"
#include"sql_connection_pool.h"
#include<map>
#include<iostream>
#include<string.h>
#include<sys/epoll.h>
#include<stdio.h>
#include<signal.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/stat.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<sys/uio.h> // 分散写

#define TIMESLOT      2      // 定时器周期：秒

class util_timer;
class sort_timer_lst;

class http_conn {
public:
    util_timer* timer;                                 // 定时器
    MYSQL *mysql;                                      // 数据库连接

    static int m_epollfd;                              // 所有的socket上的事件都被注册到同一个epoll对象中
    static int m_request_cnt;                          // 用于统计请求的数量
    static int m_user_count;                           // 用于统计用户的数量
    static const int FILENAME_LEN = 200;               // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 8 * 1024;      // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 8 * 1024;     // 写缓冲区大小
    static sort_timer_lst m_timer_lst;                 // 初始化定时器

    // 定时器回调函数，删除非活动连接socket上的注册事件并关闭
    static void callback_func(http_conn* user_data);

    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    // 解析客户端请求时，主状态机的状态: 当前正在分析请求行、当前正在分析头部字段、当前正在解析请求体
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    // 服务器处理HTTP请求的可能结果，报文解析的结果
    enum HTTP_CODE { NO_REQUEST,         // 请求不完整，需要继续读取客户数据
                    GET_REQUEST,         // 获得了一个完成的客户请求
                    BAD_REQUEST,         // 客户请求语法错误
                    NO_RESOURCE,         // 服务器没有资源
                    FORBIDDEN_REQUEST,   // 客户对资源没有足够的访问权限
                    FILE_REQUEST,        // 文件请求,获取文件成功
                    INTERNAL_ERROR,      // 服务器内部错误
                    CLOSED_CONNECTION }; // 客户端已经关闭连接
    // 从状态机的三种可能状态，即行的读取状态，分别表示 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    http_conn() {};
    ~http_conn() {};
    void process(); // 处理客户端的请求
    void init(int sockfd, const sockaddr_in &addr, 
        string user, string passwd, string sqlname); // 初始化新接收的连接
    void close_conn(); // 关闭连接
    bool read(); // 非阻塞读 因为要一次性读出所有数据

    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE process_read(); // 解析HTTP请求
    HTTP_CODE parse_request_line(char* text); // 解析请求首行
    HTTP_CODE parse_headers(char* text); // 解析请求头
    HTTP_CODE parse_content(char* text); // 解析请求体
    LINE_STATUS parse_line(); // 解析某一行

    // 下面这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool write(); // 非阻塞写
    bool process_write(HTTP_CODE res);
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);

    sockaddr_in *get_address() { return &m_address; }
    void init_mysql_result(connection_pool *conn_pool);

private:
    int m_sockfd;                       // 该HTTP连接的socket
    sockaddr_in m_address;              // 通信的socket地址
    char m_read_buf[READ_BUFFER_SIZE];  // 读缓冲区
    int m_read_idx;                     // 标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx;                  // 当前正在分析的字符在读缓冲区的位置
    int m_start_line;                   // 当前正在解析行的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];// 写缓冲区
    int m_write_idx;                    // 标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置
    // 请求体的数据和响应的数据不在同一块内存里
    struct iovec m_iv[2];               // 采用writev来执行写操作，m_iv_count表示被写内存块的数量
    int m_iv_count;                     // 有2块分散内存，m_write_buf 和 m_file_address

    CHECK_STATE m_check_state;          // 主状态机当前所处的状态
    char* m_version;                    // 协议版本，只支持HTTP1.1
    METHOD m_method;                    // 请求方法

    char m_real_file[FILENAME_LEN];     // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url;                        // 请求目标文件的文件名
    char* m_file_address;               // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;            // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息

    char* m_host;                       // 主机名
    bool m_linger;                      // HTTP请求是否要保持连接
    int m_content_length;               // HTTP请求的消息总长度

    void init();                        // 初始化连接其余的信息
    char* get_line() { return &m_read_buf[m_start_line]; }
    HTTP_CODE do_request();

    int bytes_to_send;                  // 将要发送的数据的字节数
    int bytes_have_send;                // 已经发送的字节数

    char *m_post_str;                   // 存储请求头数据
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};
#endif