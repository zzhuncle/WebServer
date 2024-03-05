# WebServer
WebServer, C++

## webbench压力测试

./webbench -c 10000 -t 5 http://192.168.163.30:10000/index.html

WSL虚拟机测试可支持28000并发访问

Speed=452712 pages/min, 1199686 bytes/sec.
Requests: 37726 susceed, 0 failed.

## 定时器功能

有序链表实现

## 异步日志功能

单例模式

生产者消费者模型

LOG_DEBUG的使用

## 实现post请求解析 连接数据库

```c++
g++ *.cpp -o main -pthread -lmysqlclient 
```

MySQL8.0后版本不支持密码登入，需配置

```shell
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY "root";
```

数据库初始化命令如下

```sql
-- 建立yourdb库
create database yourdb;

-- 创建user表
USE yourdb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
) ENGINE=InnoDB;

-- 添加数据
INSERT INTO user(username, passwd) VALUES('name', 'passwd');

-- sudo service mysql start
-- sudo mysql -uroot
-- ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY "root";
```


