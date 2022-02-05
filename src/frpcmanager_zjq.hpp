#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "util_zjq.hpp"
#include "easylogging++.h"
using namespace std;

int connect_to(const string& ip, in_port_t port) {
    // 向本地发起连接的地址
    struct sockaddr_in frps_addr;
    // 记录frps_addr
    bzero(&frps_addr, sizeof(frps_addr));
    // IPv4
    frps_addr.sin_family = AF_INET;
    // 字符串转网络字节序，并存入frps_addr.sin_addr中
    inet_pton(AF_INET, ip.c_str(), &frps_addr.sin_addr);
    // 端口
    frps_addr.sin_port = htons(port);

    // 创建socket，变量名和成员变量重了
    int connection = socket(PF_INET, SOCK_STREAM, 0);
    if(connection < 0){
        LOG(ERROR) << "create socket failed!";
        return -1;
    }

    // 建立TCP连接
    if(connect(connection, (struct sockaddr*)&frps_addr, sizeof(frps_addr))){
        LOG(ERROR) << "connect " << ip << ":" << port << " failed!";
        close_file(connection);
        return -1;
    }

    return connection;
}

class FRPCManager{
private:
    // 常量设置
    constexpr static const int BIG_BUFFER_SIZE = 65535;
    constexpr static const int EVENTS_SIZE = 5;

    // 缓冲区设置
    char *forward_buffer, *backward_buffer;
    int forward_read_idx, forward_write_idx;
    int backward_read_idx, backward_write_idx;

    // 侦听
    int epollfd;
    // 与frps的连接
    int connect_server;
    // 与本地端口的连接
    int connect_local;
    // 事件表
    epoll_event events[EVENTS_SIZE];
    // 从本地端口读取信息
    RET_CODE read_local();
    // 转发到本地端口
    RET_CODE write_local();
    // 从服务器端口读取信息
    RET_CODE read_server();
    // 转发到服务器端口
    RET_CODE write_server();

public:
    FRPCManager(in_port_t local_port, in_port_t serv_port, const string& serv_ip);
    ~FRPCManager();

    static void* start_frpc_routine(void* arg);
};

// 负责建立连接
FRPCManager::FRPCManager(in_port_t local_port, in_port_t serv_port, const string& serv_ip):
    forward_buffer(nullptr),
    backward_buffer(nullptr),
    forward_read_idx(0),
    forward_write_idx(0),
    backward_read_idx(0),
    backward_write_idx(0),
    epollfd(-1),
    connect_server(-1),
    connect_local(-1)
{
    forward_buffer  = new char[BIG_BUFFER_SIZE];
    backward_buffer = new char[BIG_BUFFER_SIZE];
    
    connect_local = connect_to("127.0.0.1",local_port);
    if(connect_local < 0) return;

    connect_server = connect_to(serv_ip, serv_port);
    if(connect_server < 0) {
        close_file(connect_local);
        return;
    }

    // 侦听
    epollfd = epoll_create(1);
    if(epollfd < 0) {
        LOG(ERROR) << "epoll create failed!";
        close_file(connect_local);
        close_file(connect_server);
        return;
    }
}

FRPCManager::~FRPCManager(){
    delete []forward_buffer;
    delete []backward_buffer;
    close_file(connect_local);
    close_file(connect_server);
    close_file(epollfd);
}

void* FRPCManager::start_frpc_routine(void* arg){
    FRPCManager* manager = (FRPCManager*)arg;
    int epollfd = manager->epollfd;
    int connect_local = manager->connect_local;
    int connect_server = manager->connect_server;
    epoll_event (&events)[EVENTS_SIZE] = manager->events;

    add_readfd(epollfd, connect_local);
    add_readfd(epollfd, connect_server);

    RET_CODE res;
    bool stop=false;
    while(!stop){
        int num = epoll_wait(epollfd, events, EVENTS_SIZE, -1);
        if(num < 0){
            perror("epoll_wait failed!");
            stop = true;
        }

        for(int i=0; i < num; ++i){
            // 当前事件
            const epoll_event& event = events[i];

            // 本地端口事件
            if(event.data.fd == connect_local) {
                // 本地端口可写
                if(event.events & EPOLLOUT) {
                    LOG(INFO) << "write to " << connect_local;
                    switch(manager->write_local()){
                        // 数据发送完毕 只改自己的状态为读侦听
                        case BUFFER_EMPTY:{
                            modfd(epollfd, connect_local, EPOLLIN);
                            break;
                        }
                        // 数据还没完全发送完毕
                        case TRY_AGAIN:{
                            modfd(epollfd, connect_local, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
                // 本地端口可读
                else if(event.events & EPOLLIN) {
                    LOG(INFO) << "read from " << connect_local;
                    switch(manager->read_local()){
                        case OK:
                        case BUFFER_FULL:{
                            modfd(epollfd, connect_server, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            // 服务器端口事件
            else if(event.data.fd == connect_server) {
                // 服务器端口可写
                if(event.events & EPOLLOUT) {
                    LOG(INFO) << "write to " << connect_server;
                    switch(manager->write_server()){
                        case BUFFER_EMPTY:{
                            modfd(epollfd, connect_server, EPOLLIN);
                            break;
                        }
                        case TRY_AGAIN:{
                            modfd(epollfd, connect_server, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
                // 服务器端口可读
                else if(event.events & EPOLLIN){
                    LOG(INFO) << "read from " << connect_server;
                    switch(manager->read_server()){
                        case OK:
                        case BUFFER_FULL:{
                            modfd(epollfd, connect_local, EPOLLOUT);
                            break;
                        }
                        case IOERR:
                        case CLOSED:{
                            stop=true;
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            // 其他事件数据错误
            else{
                LOG(ERROR) <<"the event is is not right";
                stop=true;
            }
        }
    }
    delete manager;
    return nullptr;
}

RET_CODE FRPCManager::read_local(){
    int bytes_read = 0;
    while(true){
        if(forward_read_idx>=BIG_BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connect_local, forward_buffer+forward_read_idx, BIG_BUFFER_SIZE-forward_read_idx, 0);
        if(bytes_read==-1){
            // 内核没数据可读
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        forward_read_idx+=bytes_read;
        LOG(INFO) << "bytes read: " << bytes_read << " ";
    }
    return (forward_read_idx-forward_write_idx>0)? OK: NOTHING;
}

RET_CODE FRPCManager::read_server(){
    int bytes_read = 0;
    while(true){
        if(backward_read_idx>=BIG_BUFFER_SIZE){
            return BUFFER_FULL;
        }
        bytes_read = recv(connect_server, backward_buffer+backward_read_idx, BIG_BUFFER_SIZE-backward_read_idx, 0);
        if(bytes_read==-1){
            // 内核没数据可读
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            return IOERR;
        }
        else if(bytes_read==0) return CLOSED;
        backward_read_idx+=bytes_read;
        LOG(INFO) << "bytes read: " << bytes_read << " ";

    }
    return (backward_read_idx-backward_write_idx>0)? OK: NOTHING;
}

RET_CODE FRPCManager::write_local(){
    int bytes_write = 0;
    while(true){
        // 正常退出都是buffer_empty
        if(backward_write_idx>=backward_read_idx){
            backward_write_idx = backward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(connect_local, backward_buffer+backward_write_idx, backward_read_idx-backward_write_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            // 内核没地方可写
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        backward_write_idx+=bytes_write;
        LOG(INFO) << "bytes write: " << bytes_write << " ";

    }
    return OK;
}

RET_CODE FRPCManager::write_server(){
    int bytes_write = 0;
    while(true){
        if(forward_write_idx>=forward_read_idx){
            forward_write_idx=forward_read_idx=0;
            return BUFFER_EMPTY;
        }
        bytes_write = send(connect_server, forward_buffer+forward_write_idx, forward_read_idx-forward_write_idx, MSG_NOSIGNAL);
        if(bytes_write==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK) return TRY_AGAIN;
            return IOERR;
        }
        else if(bytes_write==0) return CLOSED;
        forward_write_idx+=bytes_write;
        LOG(INFO) << "bytes write: " << bytes_write << " ";

    }
    return OK;
}
