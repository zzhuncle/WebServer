#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include<fstream>
#include<iostream>
#include<string>
#include<stdarg.h>
#include<pthread.h>
#include"block_queue.h"
using namespace std;

// 使用单例模式创建日志系统
class Log
{
public:
    // C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance() {
        static Log instance;
        return &instance;
    }
    
    // 回调函数，创建子线程用于异步写日志
    static void *flush_log_thread(void *args) {
        Log::get_instance()->async_write_log();
        return nullptr;
    }

    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int max_lines = 5000000, 
        int max_queue_size = 0, int log_level = 0);
    void write_log(int level, const char *format, ...);
    void flush(void) {
        m_mutex.lock();
        // 强制刷新写入流缓冲区
        fflush(m_fp);
        m_mutex.unlock();
    }

private:
    Log() {
        m_count = 0;
        m_is_async = false;
    }
    virtual ~Log() {
        if (m_fp) fclose(m_fp);
        if (m_log_queue) delete m_log_queue;
        if (m_buf) delete m_buf;
    }
    void *async_write_log() {
        string single_log;
        // 从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log)) {
            m_mutex.lock();
            // 将日志写入文件
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
        return nullptr;
    }

private:
    char dir_name[128];                 // 路径名
    char log_name[128];                 // log文件名
    int m_max_lines;                    // 日志最大行数
    int m_log_buf_size;                 // 日志缓冲区大小
    long long m_count;                  // 日志行数记录
    int m_today;                        // 因为按天分类,记录当前时间是那一天
    FILE *m_fp;                         // 打开log的文件指针
    char *m_buf;                        // 写入的buffer
    block_queue<string> *m_log_queue;   // 阻塞队列
    bool m_is_async;                    // 是否同步标志位
    locker m_mutex;                     // 互斥锁
public:
    int m_close_log;                    // 关闭日志
    int m_log_level;                    // 日志等级
};

// __VA_ARGS__和##__VA_ARGS__ 定义时宏定义中参数列表的最后一个参数为省略号 
// https://blog.csdn.net/qq_42370809/article/details/124020334
#define LOG_DEBUG(format, ...) if(0 == Log::get_instance()->m_close_log && Log::get_instance()->m_log_level <= 0) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...)  if(0 == Log::get_instance()->m_close_log && Log::get_instance()->m_log_level <= 1) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...)  if(0 == Log::get_instance()->m_close_log && Log::get_instance()->m_log_level <= 2) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == Log::get_instance()->m_close_log && Log::get_instance()->m_log_level <= 3) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
