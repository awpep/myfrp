#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "util_zjq.hpp"
#include "frpcmanager_zjq.hpp"
#include "easylogging++.h"

#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
using namespace std;

class Master{
private:

    // 内部类型
    enum STATE : uint8_t
    {
        UNINIT,
        READY,
        NOTHING,
        DO_SOMETHING,
        STOP
    };

    enum OPERATE : uint8_t
    {
        ERR,
        NEW,
        DEL,
        CLOSED
    };

    struct Ctrl_message{
        OPERATE op;
        in_port_t local_port, server_port;
    };

    //常量设置
    constexpr static const int BUFFER_SIZE      = 512;
    constexpr static const int EVENTS_SIZE      = 5;
    constexpr static const int TIME_OUT         = 10000;
    constexpr static const int MAX_HEART_BEATS  = 3;

    // 缓冲区资源
    char* buffer;
    
    // 服务器ip地址
    string serv_ip;
    // 服务器端口
    in_port_t serv_port;
    // 和frpc的固定连接
    int connection;
    // 侦听
    int epollfd;
    // 心跳包计数
    int heart_count;
    // 状态
    STATE state;
    // epoll事件表
    epoll_event events[EVENTS_SIZE];
    // epoll事件数量
    int event_num;

    // 和frps建立连接
    int Connect();
    // 发送心跳
    int send_heart_beat();
    // 向frps写
    RET_CODE write_to_frps(int length);
    // 从frps读
    RET_CODE read_from_frps();
    //
    STATE do_something(int num);
    //
    Ctrl_message get_message();

public:
    Master(const string& serv_ip, in_port_t serv_port);
    ~Master();
    void run();
    // 和frps建立连接
    static Master Connect(const string& serv_ip, in_port_t serv_port);
    static void Disconnect(Master& m);
};

Master::Master(const string& serv_ip, in_port_t serv_port):
    serv_ip(serv_ip),
    serv_port(serv_port),
    heart_count(0),
    event_num(0),
    state(UNINIT)
{
    buffer = new char[BUFFER_SIZE];
}

Master::~Master() {
    Disconnect(*this);
    delete[] buffer;
}

void Master::run() {
    if(state == UNINIT) return;

    while(state != STOP) {
        switch(state) {
        // 就绪状态
        case READY:
            event_num = epoll_wait(epollfd, events, EVENTS_SIZE, TIME_OUT);
            if(event_num < 0) {
                LOG(ERROR) << "epoll wait failed";
                state = STOP;
            }
            else if(event_num == 0) state = NOTHING;
            else state = DO_SOMETHING;
            break;
        // 空等状态，需要发送心跳包
        case NOTHING:
            if(send_heart_beat() < 0) ++heart_count;
            else {
                heart_count = 0;
                state = READY;
            }
            if(heart_count >= MAX_HEART_BEATS){
                state = STOP;
            }
            break;
        // 处理事件
        case DO_SOMETHING:
            state = do_something(event_num);
            break;
        }
    }
}

Master Master::Connect(const string& serv_ip, in_port_t serv_port) {
    // 管理
    Master ret(serv_ip,serv_port);

    int connection = connect_to(serv_ip, serv_port);
    if(connection < 0) return ret;

    // 侦听
    int epollfd = epoll_create(1);
    if(epollfd < 0) {
        LOG(ERROR) << "epoll create failed!";
        return ret;
    }

    // 注册
    add_readfd(epollfd, connection);

    ret.epollfd     = epollfd;
    ret.connection  = connection;
    ret.state       = READY;
    return ret;
}

void Master::Disconnect(Master& m) {
    close_file(m.connection);
    close_file(m.epollfd);
}

int Master::send_heart_beat(){
    if(BUFFER_SIZE<sizeof(in_port_t)){
        LOG(ERROR) << "the buffer is not enough to send heart beat";
        return -1;
    };
    
    *((in_port_t*)buffer) = -2;

    switch(write_to_frps(sizeof(in_port_t))){
        case IOERR:{
            LOG(ERROR) << "the frps error";
            return -1;
        }
        case CLOSED:{
            LOG(ERROR) << "the frps closed";
            return -1;
        }
        case TRY_AGAIN:{
            LOG(ERROR) << "the kernel is not enough to send heart beat pack";
            return -1;
        }
        default:
            break;
    }
    return 0;
}

//
RET_CODE Master::write_to_frps(int length){
    int bytes_write = 0;
    int buffer_idx = 0;
    while(true){
        if(buffer_idx>=length){
            return BUFFER_EMPTY;
        }
        // send返回0确实是服务器关闭了连接
        int retry = 0;

        // TODO:不要goto，以后再改
        label:
        bytes_write = send(connection, buffer+buffer_idx, length-buffer_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            // 等候缓冲区
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                ++retry;
                if(retry>5){
                    return TRY_AGAIN;
                }
                goto label;
            }
            return IOERR;
        }
        else if(bytes_write==0) return RET_CODE::CLOSED;
        buffer_idx += bytes_write;
    }
    return OK;
}

// 从frps读
RET_CODE Master::read_from_frps(){
    // 读取数量
    int bytes_read = 0;
    // 缓冲地址偏移
    int buffer_idx = 0;

    while(true){
        if(buffer_idx >= BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connection, buffer+buffer_idx, BUFFER_SIZE-buffer_idx, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return RET_CODE::CLOSED;
        buffer_idx += bytes_read;
    }
    return buffer_idx>0? OK: RET_CODE::NOTHING;
}

Master::STATE Master::do_something(int num) {
    STATE sta = READY;
    pthread_t tid = 0;

    for(int i = 0; i < event_num; ++i) {
        if(events[i].data.fd != connection || !(events[i].events & EPOLLIN)) {
            LOG(ERROR) << "the wrong type coming to frpc";
            continue;
        }

        // 可以再改改
        auto [operate, local_port, server_port] = get_message();

        switch(operate) {
        case OPERATE::NEW:{
            FRPCManager* manager = new FRPCManager(local_port,server_port,serv_ip);
            // 创建线程
            if(pthread_create(&tid, nullptr, FRPCManager::start_frpc_routine, (void*)manager)) {
                delete manager;
                LOG(ERROR) << "pthread_create failed!";
            }
            // 分离线程，使其能自动释放资源
            else if(pthread_detach(tid)) {
                LOG(ERROR) << "pthread_detach failed!";
            }
            break;
        }
        case OPERATE::CLOSED:
            sta = STATE::STOP;
            break;
        default:
            break;
        }
    }

    return sta;
}

Master::Ctrl_message Master::get_message() {
    Ctrl_message message;
    
    switch(read_from_frps()){
        case RET_CODE::BUFFER_FULL:
            LOG(ERROR) << "the buffer is not enough to receive data";
            break;
        case RET_CODE::IOERR:
            LOG(ERROR) << "the frps error";
            break;
        case RET_CODE::CLOSED:
            LOG(ERROR) << "the frps closed";
            message.op = OPERATE::CLOSED;
            break;
        default:
            message = *((Ctrl_message*)buffer);
            break;
    }

    return message;
}