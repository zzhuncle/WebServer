#ifndef _CONNECTION_POOL_H
#define _CONNECTION_POOL_H
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h"
#include "log.h"
using namespace std;

class connection_pool
{
public:
    static connection_pool *get_instance()              // 单例模式
    {
        static connection_pool conn_pool;
        return &conn_pool;
    }             
    MYSQL *get_connection();                            // 获取数据库连接
    bool release_connection(MYSQL *conn);               // 释放连接
    int get_free_conn()                                 // 获取连接
    {
        return m_free_conn;
    }
    void destroy_pool();                                // 销毁所有连接 
    void init(string url, string user, string password, 
        string database_name, int port, int max_conn);  // 初始化
private:
    connection_pool()
    {
        m_cur_conn = 0;
        m_free_conn = 0;
    }
    ~connection_pool()
    {
        destroy_pool();
    }
public:
    string m_url;                                       // 主机地址
    string m_port;                                      // 数据库端口号
    string m_user;                                      // 登录数据库用户名
    string m_password;                                  // 登录数据库密码
    string m_database_name;
private:
    int m_max_conn;                                     // 最大连接数
    int m_cur_conn;                                     // 当前连接数
    int m_free_conn;                                    // 当前空闲的连接数
    locker lock;
    list<MYSQL*> conn_list;                             // 连接池
    sem reserve;                                        // 信号量
};

class connectionRAII
{
public:
    connectionRAII(MYSQL **conn, connection_pool *conn_pool)
    {
        *conn = conn_pool->get_connection();
        poolRAII = conn_pool;
    }
    ~connectionRAII()
    {
        poolRAII->release_connection(connRAII);
    }
private:
    MYSQL *connRAII;
    connection_pool *poolRAII;
};

#endif
