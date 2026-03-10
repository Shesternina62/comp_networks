#ifndef MESSAGES_H
#define MESSAGES_H

#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <string>

#define PORT 8080
#define MAX_PAYLOAD 1024

//cтруктура сообщения
#pragma pack(push, 1)
struct Message {
    uint32_t length; //длина поля type + payload
    uint8_t type; //тип сообщения
    char payload[MAX_PAYLOAD];//данные
    
    Message() : length(0), type(0) {
        memset(payload, 0, MAX_PAYLOAD);
    }
};
#pragma pack(pop)

//типы сообщений
enum MessageType {
    MSG_HELLO   = 1,//клиент -> сервер (ник)
    MSG_WELCOME = 2,//сервер -> клиент 
    MSG_TEXT    = 3,//текст
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

//вспомогательные функции для отправки/приема
ssize_t sendAll(int sockfd, const void* data, size_t len) {
    size_t total = 0;
    const char* ptr = (const char*)data;

    while (total < len) {
        ssize_t sent = send(sockfd, ptr + total, len - total, 0);
        if (sent <= 0) return sent;
        total += sent;
    }
    return total;
}

ssize_t recvAll(int sockfd, void* data, size_t len) {
    size_t total = 0;
    char* ptr = (char*)data;

    while (total < len) {
        ssize_t received = recv(sockfd, ptr + total, len - total, 0);
        if (received <= 0) return received;
        total += received;
    }
    return total;
}

bool sendMessage(int sockfd, uint8_t type, const std::string& text) {
    Message msg;
    msg.type = type;
    
    size_t textLen = text.size();
    if (textLen > MAX_PAYLOAD - 1)
        textLen = MAX_PAYLOAD - 1;
    
    memcpy(msg.payload, text.c_str(), textLen);
    msg.payload[textLen] = '\0';
    msg.length = sizeof(msg.type) + textLen + 1;
    
    return sendAll(sockfd, &msg, sizeof(msg)) == sizeof(msg);
}

bool recvMessage(int sockfd, Message& msg) {
    return recvAll(sockfd, &msg, sizeof(msg)) == sizeof(msg);
}

#endif 