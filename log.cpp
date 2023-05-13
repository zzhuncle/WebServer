#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int max_lines, int max_queue_size, int log_level)
{
    // 如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        // flush_log_thread为回调函数，这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    m_close_log    = close_log;
    m_log_buf_size = log_buf_size;
    m_buf          = new char[m_log_buf_size];
    m_max_lines    = max_lines;
    m_log_level    = log_level;
    memset(m_buf, 0, m_log_buf_size);

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
 
    // 返回str中最后一次出现字符c的位置 相当于只提取出文件名，把路径忽略掉
    const char *p = strrchr(file_name, '/');
    char log_full_name[512] = {0};

    if (p == NULL) {
        // str -- 目标字符串，用于存储格式化后的字符串的字符数组的指针 size -- 字符数组的大小
        // format -- 格式化字符串 ... -- 可变参数，可变数量的参数根据 format 中的格式化指令进行格式化
        // 输出形式是“年_月_日_文件名”
        snprintf(log_full_name, 511, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 511, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL) {
        printf("创建文件失败\n");
        return false;
    }
    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    // 获取时间
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};

    switch (level)
    {
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    // 日期过期或达到最大行数，则新建log文件
    m_mutex.lock();
    m_count++;
    if (m_today != my_tm.tm_mday || m_count % m_max_lines == 0) {
        char new_log[512] = {0};
        // 刷新文件流
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 511, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            snprintf(new_log, 511, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_max_lines);
        }
        m_fp = fopen(new_log, "a");
        if (m_fp == NULL) {
            printf("创建文件失败\n");
            return;
        }
    }
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();
    // 写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;
    m_mutex.unlock();

    // 判断是同步输出还是异步输出，异步输出要压入队列
    // 如果队列满了，就采用同步直接输出
    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    } else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }
    va_end(valst);
}
