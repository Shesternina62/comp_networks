#ifndef MESSAGES_H
#define MESSAGES_H

#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <string>

#define PORT 8080
#define MAX_PAYLOAD 1024

#pragma pack(push, 1)
struct Message {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];

    Message() : length(0), type(0) {
        memset(payload, 0, MAX_PAYLOAD);
    }
};
#pragma pack(pop)

enum MessageType {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

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

    size_t len = text.size();
    if (len > MAX_PAYLOAD - 1)
        len = MAX_PAYLOAD - 1;

    memcpy(msg.payload, text.c_str(), len);
    msg.payload[len] = '\0';

    msg.length = sizeof(msg.type) + len + 1;

    return sendAll(sockfd, &msg, sizeof(msg.type) + sizeof(msg.length) + len + 1)
           == (ssize_t)(sizeof(msg.type) + sizeof(msg.length) + len + 1);
}

bool recvMessage(int sockfd, Message& msg) {
    if (recvAll(sockfd, &msg.length, sizeof(msg.length)) <= 0)
        return false;

    if (recvAll(sockfd, &msg.type, sizeof(msg.type)) <= 0)
        return false;

    size_t payloadSize = msg.length - sizeof(msg.type);

    if (payloadSize > MAX_PAYLOAD)
        return false;

    return recvAll(sockfd, msg.payload, payloadSize) > 0;
}

#endif