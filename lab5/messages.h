#ifndef MESSAGES_H
#define MESSAGES_H

#include <sys/socket.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <ctime>
#include <atomic>

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256

#pragma pack(push, 1)
struct MessageEx {
    uint32_t length;
    uint8_t  type;        
    uint32_t msg_id;

    char sender[MAX_NAME];    
    char receiver[MAX_NAME];

    time_t timestamp; 

    char payload[MAX_PAYLOAD];

    MessageEx() {
        length = 0;
        type = 0;
        msg_id = 0;
        timestamp = time(nullptr);

        memset(sender, 0, MAX_NAME);
        memset(receiver, 0, MAX_NAME);
        memset(payload, 0, MAX_PAYLOAD);
    }
};
#pragma pack(pop)

enum MessageType {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,

    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,

    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14
};

inline ssize_t sendAll(int sockfd, const void* data, size_t len) {
    size_t total = 0;
    const char* ptr = (const char*)data;

    while (total < len) {
        ssize_t sent = send(sockfd, ptr + total, len - total, 0);
        if (sent <= 0) return sent;
        total += sent;
    }
    return total;
}

inline ssize_t recvAll(int sockfd, void* data, size_t len) {
    size_t total = 0;
    char* ptr = (char*)data;

    while (total < len) {
        ssize_t r = recv(sockfd, ptr + total, len - total, 0);
        if (r <= 0) return r;
        total += r;
    }
    return total;
}

inline bool sendMessageEx(int sockfd, MessageEx& msg) {
    msg.length = sizeof(MessageEx) - sizeof(msg.length);
    return sendAll(sockfd, &msg, sizeof(MessageEx)) == sizeof(MessageEx);
}

inline bool recvMessageEx(int sockfd, MessageEx& msg) {
    return recvAll(sockfd, &msg, sizeof(MessageEx)) == sizeof(MessageEx);
}

extern std::atomic<uint32_t> global_msg_id;

#endif