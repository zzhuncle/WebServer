#include"log.h"
#include"http_conn.h"
#include"sql_connection_pool.h"
#include<cstdio>

// 静态成员变量初始化
int http_conn::m_epollfd     = -1;    // 所有的socket上的事件都被注册到同一个epoll对象中
int http_conn::m_user_count  = 0;     // 用于统计用户的数量
int http_conn::m_request_cnt = 0;    // 用于统计请求的数量
sort_timer_lst http_conn::m_timer_lst;

// 定义HTTP响应的一些状态信息
const char* ok_200_title    = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form  = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form  = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form  = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form  = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root        = "/home/zzh/github/WebServer/resources";

locker m_lock;
map<string, string> users;

// 设置文件描述符非阻塞
void setnonblocking(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 添加文件描述符到epoll中
void addfd(int epollfd, int fd, bool oneshot)
{
    // epoll事件
    epoll_event event;
    event.data.fd = fd;
    // 水平触发模式 EPOLLRDHUP代表对端断开连接
    event.events = EPOLLIN | EPOLLRDHUP;

    // oneshot目的：即使使用ET模式，一个socket上的某个事件还是可能被触发多次，比如一个线程在读完某个socket
    // 上的数据后开始处理这些数据，而在数据的处理过程中该socket上又有新的数据可读，此时另外一个线程被唤醒
    // 读取这些新数据，于是出现两个线程同时操作一个socket的局面。一个socket连接在任一时刻都只被一个线程处理
    // 可以使用epoll的EPOLLONESHOT事件实现
    if (oneshot) {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    } 
    // 对epoll实例进行管理:添加文件描述符信息,删除信息,修改信息
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}


// 从epoll中修改文件描述符，重置socket上的EPOLLONESHOT事件，
// 确保下一次可读时，EPOLLIN事件可以被触发
// 将事件重置为EPOLL_ONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init_mysql_result(connection_pool *conn_pool)
{
    // 从连接池中取一个连接
    connectionRAII mysqlcon(&mysql, conn_pool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    
    MYSQL_RES *result = mysql_store_result(mysql);     // 从表中检索完整的结果集
    int num_fields = mysql_num_fields(result);         // 返回结果集中的列数
    MYSQL_FIELD *fields = mysql_fetch_fields(result);  // 返回所有字段结构的数组
    
    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string tmp1(row[0]);
        string tmp2(row[1]);
        users[tmp1] = tmp2;
    }
}

// 定时器回调函数，删除非活动连接socket上的注册事件并关闭
void http_conn::callback_func(http_conn* user_data)
{
    epoll_ctl(http_conn::m_epollfd, EPOLL_CTL_DEL, user_data->m_sockfd, 0);
    close(user_data->m_sockfd);
    LOG_INFO("close fd %d\n", user_data->m_sockfd);
}

// 初始化新接收的连接
void http_conn::init(int sockfd, const sockaddr_in &addr, 
    string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    http_conn::m_user_count++;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    // 初始化
    init();
    char ip[16] = "";
    const char* str = inet_ntop(AF_INET, &addr.sin_addr.s_addr, ip, sizeof(ip));
    LOG_INFO("The No.%d user. sock_fd = %d, ip = %s.\n", m_user_count, m_sockfd, str);
    
    // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表m_timer_lst中
    util_timer* timer = new util_timer;
    timer->user_data = this;
    timer->callback_func = http_conn::callback_func;
    timer->expire = time(NULL) + 3 * TIMESLOT;
    this->timer = timer;
    m_timer_lst.add_timer(timer);
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        http_conn::m_user_count--; // 关闭一个连接，客户端总数量-1
        LOG_INFO("closing fd: %d, rest user num :%d\n", m_sockfd, http_conn::m_user_count);
    } 
}

// 非阻塞读，循环读取客户数据，直到无数据或断开连接
bool http_conn::read() 
{
    if (timer) { // 更新超时时间
        time_t cur_time = time(NULL);
        timer->expire = cur_time + 3 * TIMESLOT;
        m_timer_lst.adjust_timer(timer);
    }

    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    // 已经读取到的字节
    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, &m_read_buf[m_read_idx], READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    m_request_cnt++;
    LOG_INFO("sock_fd = %d read done. request cnt = %d\n", m_sockfd, m_request_cnt);    // 全部读取完毕
    
    return true;
}

// 非阻塞写，分散写，写HTTP响应
bool http_conn::write()
{
    if (timer) { // 更新超时时间
        time_t cur_time = time(NULL);
        timer->expire = cur_time + 3 * TIMESLOT;
        m_timer_lst.adjust_timer(timer);
    }

    int temp = 0;
    LOG_INFO("sock_fd = %d writing %d bytes. request cnt = %d\n", m_sockfd, bytes_to_send, m_request_cnt);
    
    if (bytes_to_send == 0) {
        // 将要发送的字节为0，这一次响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN); 
        init();
        return true;
    }

    while (1) {
        // 分散写，writev以顺序m_iv[0]、m_iv[1]至m_iv[m_iv-1]从各缓冲区中聚集输出数据到m_sockfd
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {
            // 响应头已经发送完毕
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            // 没有发送完毕，修改下次写数据的位置
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN); // 重置监听事件

            if (m_linger) { // http请求是否要保持连接
                init();
                return true;
            } else {
                return false;
            }
        }
    }   
}

// 由线程池中的工作线程调用，处理HTTP请求的入口函数，业务逻辑
void http_conn::process()
{
    LOG_DEBUG("=======parse request, create response.=======\n");
    LOG_DEBUG("=============process_reading=============\n");
    // 解析HTTP请求
    HTTP_CODE read_res = process_read();
    LOG_DEBUG("========PROCESS_READ HTTP_CODE : %d========\n", read_res);
    if (read_res == NO_RESOURCE) { // 读取不完整，继续获取客户端数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    } 

    // 生成响应
    bool write_res = process_write(read_res);
    if (!write_res) {
        close_conn();
        if(timer) // 移除其对应的定时器
            m_timer_lst.del_timer(timer);
    } 
    // 因为用了modfd，所以每次都需要重新修改状态
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 需要重置一些数组，否则会输出乱码
void http_conn::init()
{
    mysql = NULL;

    bytes_to_send = 0;
    bytes_have_send = 0; 

    m_content_length = 0;
    m_host = 0;

    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;

    memset(m_read_buf, 0, sizeof(m_read_buf));
    memset(m_write_buf, 0, sizeof(m_write_buf));
    memset(m_real_file, 0, FILENAME_LEN);
}

// 主状态机：解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE res = NO_REQUEST;
    char* text = 0;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
     || ((line_status = parse_line()) == LINE_OK)) {
        // 解析到了一行完整的数据，或者解析到了请求体，也是完成的数据

        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_DEBUG(">>>>>> %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
            {
                res = parse_request_line(text);
                if (res == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                res = parse_headers(text);
                if (res == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (res == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                // 上传文件 仅支持8KB以下文本文件
                const char *p = strrchr(m_url, '/');
                if (m_method == POST && p[1] == '9') { 
                    string body = text, filename;
                    size_t st = 0, ed = body.find("\r\n");
                    string boundary = body.substr(0, ed);

                    // 解析文件信息
                    st = body.find("filename=\"", ed) + strlen("filename=\"");
                    ed = body.find("\"", st);
                    filename = body.substr(st, ed - st);
                    LOG_INFO("upload file %s", filename.c_str());

                    // 解析文件内容，文件内容以\r\n\r\n开始
                    st = body.find("\r\n\r\n", ed) + strlen("\r\n\r\n");
                    ed = body.find(boundary, st) - 2; // 文件结尾也有\r\n
                    string content = body.substr(st, ed - st);
                    LOG_DEBUG("upload content %s", content.c_str());
                    printf("%d", 3);

                    std::ofstream out("./resources/files/" + filename, ios::ate);
                    out << content;
                    out.close();
                }

                res = parse_content(text);
                if (res == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN; // 行数据尚不完整
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 解析请求首行
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //  GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (!m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if (strcasecmp(method, "GET") == 0) { // 忽略大小写比较
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    // http://192.168.110.129:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    // 检查状态变成检查头
    m_check_state = CHECK_STATE_HEADER; 
    return NO_REQUEST;
} 

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    // 遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        LOG_DEBUG("oop! unknow header: %s\n", text);
    }
    return NO_REQUEST;
}

// 解析请求体
// 没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_post_str = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 解析某一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char tmp;
    for (;m_checked_idx < m_read_idx;m_checked_idx++) {
        tmp = m_read_buf[m_checked_idx];
        if (tmp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (tmp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/zzh/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');

    // 处理POST
    if (m_method == POST && (p[1] == '2' || p[1] == '3')) {
        // 根据标志判断是登录检测还是注册检测
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来 user=123&passwd=123
        char name[100], password[100];
        int i, j = 0;
        for (i = 5; m_post_str[i] != '&'; i++)
            name[i - 5] = m_post_str[i];
        name[i - 5] = '\0';

        for (i = i + 10; m_post_str[i] != '\0'; ++i, ++j)
            password[j] = m_post_str[i];
        password[j] = '\0';

        if (p[1] == '3') {
            connectionRAII mysqlcon(&mysql, connection_pool::get_instance());
            // 如果是注册，先检测数据库中是否有重名的 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            if (users.find(name) == users.end()) {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/register_error.html");
            }
            else {
                strcpy(m_url, "/register_error.html");
            }
        }
        // 如果是登录，直接判断，若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (p[1] == '2') {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/log_error.html");
        }
    }

    if (p[1] == '0') { // 登录页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (p[1] == '1') { // 注册页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (p[1] == '5') { // 图片页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (p[1] == '6') { // 视频页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (p[1] == '7' || p[1] == '9') { // 上传页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/upload.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (p[1] == '8') { // 下载页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/download.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    // 创建内存映射，非常关键，将此地址发送给客户端
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    // https://blog.csdn.net/sinat_31608641/article/details/120576549
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(&m_write_buf[m_write_idx],  WRITE_BUFFER_SIZE - 1 - m_write_idx,
     format, arg_list);
    if (len >= WRITE_BUFFER_SIZE - 1 - m_write_idx) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) 
{
    LOG_DEBUG("<<<<<<< %s %d %s\r\n", "HTTP/1.1", status, title); 
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
 {
    LOG_DEBUG("<<<<<<< Content-Length: %d\r\n", content_len);  
    if (!add_response("Content-Length: %d\r\n", content_len)) {
        return false;
    }
    LOG_DEBUG("<<<<<<< Content-Type:%s\r\n", "text/html"); 
    if (!add_response("Content-Type:%s\r\n", "text/html")) {
        return false;
    }
    LOG_DEBUG("<<<<<<< Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
    if (!add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close")) {
        return false;
    }
    LOG_DEBUG("<<<<<<< %s", "\r\n" ); 
    if (!add_response("%s", "\r\n")) {
        return false;
    }
    return true;
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE res) 
{
    switch (res) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                LOG_DEBUG("<<<<<<< %s\n", m_file_address);
                // 封装m_iv
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                // 初始化m_iv_count
                m_iv_count = 2;
                // 响应头大小 + 文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        default:
            return false;
    }
    // 没有请求文件时
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

