#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"
using namespace std;

// 构造初始化
void connection_pool::init(string url, string user, string password, 
        string database_name, int port, int max_conn)
{
    m_url = url;
    m_port = port;
    m_user = user;
    m_password = password;
    m_database_name = database_name;

    for (int i = 0;i < max_conn;i++) {
        MYSQL *conn = NULL;
        conn = mysql_init(conn);
        if (!conn) {
            LOG_ERROR("MySQL init error\n");
            exit(1);
        }
        conn = mysql_real_connect(conn, url.c_str(), user.c_str(), 
            password.c_str(), database_name.c_str(), port, NULL, 0);
        if (!conn) {
            LOG_ERROR("MySQL connect error\n");
            exit(1);
        }
        conn_list.push_back(conn);
        m_free_conn++;
    }
    reserve = sem(m_free_conn);
    m_max_conn = m_free_conn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::get_connection()
{
    MYSQL *conn = NULL;
    if (0 == conn_list.size())
        return NULL;
    reserve.wait();
    
    lock.lock();
    conn = conn_list.front();
    conn_list.pop_front();
    m_free_conn--;
    m_cur_conn++;
    lock.unlock();
    return conn;
}

// 释放当前使用的连接
bool connection_pool::release_connection(MYSQL *conn)
{
    if (NULL == conn)
        return false;
    lock.lock();
    conn_list.push_back(conn);
    m_free_conn++;
    m_cur_conn--;
    lock.unlock();
    reserve.post();
    return true;
}

// 销毁数据库连接池
void connection_pool::destroy_pool()
{
    lock.lock();
    if (conn_list.size() > 0) {
        list<MYSQL*>::iterator it;
        for (it = conn_list.begin();it != conn_list.end();it++) {
            MYSQL *conn = *it;
            mysql_close(conn);
        }
    }
    m_cur_conn = 0;
    m_free_conn = 0;
    conn_list.clear();
    lock.unlock();
}
