#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdint.h>
#include <time.h>


#define SERVER_PORT         8080
#define WORKER_THREADS      10
#define CONNECTION_QUEUE    100
#define MAX_USER_LIMIT      100
#define MAX_USERNAME_LEN    32
#define MAX_PAYLOAD_SIZE    256
#define MAX_TIMESTAMP_STR   32

#define ACK_TIMEOUT_SEC     2       
#define MAX_RETRIES         3      


typedef struct
{
    uint32_t message_length;
    uint8_t  message_type;
    uint32_t message_id;
    char     from_username[MAX_USERNAME_LEN];
    char     to_username[MAX_USERNAME_LEN];
    time_t   message_timestamp;
    char     message_payload[MAX_PAYLOAD_SIZE];
} NetworkPacket;


typedef struct {
    NetworkPacket msg;          // Копия отправленного сообщения
    time_t        send_time;    // Время отправки
    int           retries;      // Количество попыток
} PendingMessage;

enum MessageTypes
{
    // Handshake & Connection
    TYPE_HANDSHAKE      = 1,   // MSG_HELLO
    TYPE_WELCOME        = 2,   // MSG_WELCOME
    
    // Basic Messaging
    TYPE_PUBLIC_MSG     = 3,   // MSG_TEXT
    TYPE_KEEPALIVE_REQ  = 4,   // MSG_PING
    TYPE_KEEPALIVE_RESP = 5,   // MSG_PONG
    TYPE_DISCONNECT     = 6,   // MSG_BYE
    
    // User Management
    TYPE_AUTHENTICATE   = 7,   // MSG_AUTH
    TYPE_PRIVATE_MSG    = 8,   // MSG_PRIVATE
    TYPE_ERROR_RESP     = 9,   // MSG_ERROR
    
    // Info & Utilities
    TYPE_SERVER_NOTIFY   = 10,  // MSG_SERVER_INFO
    TYPE_USER_LIST_REQ   = 11,  // MSG_LIST
    TYPE_HISTORY_REQ     = 12,  // MSG_HISTORY
    TYPE_HISTORY_RESP    = 13,  // MSG_HISTORY_DATA
    TYPE_HELP_REQ        = 14,  // MSG_HELP
    
    
    TYPE_ACK             = 15   // MSG_ACK - подтверждение доставки
};


#define PORT                SERVER_PORT
#define THREAD_POOL_SIZE    WORKER_THREADS
#define QUEUE_SIZE          CONNECTION_QUEUE
#define MAX_CLIENTS         MAX_USER_LIMIT

#define MAX_NAME            MAX_USERNAME_LEN
#define MAX_PAYLOAD         MAX_PAYLOAD_SIZE
#define MAX_TIME_STR        MAX_TIMESTAMP_STR

#define MessageEx           NetworkPacket


#define MSG_HELLO           TYPE_HANDSHAKE
#define MSG_WELCOME         TYPE_WELCOME
#define MSG_TEXT            TYPE_PUBLIC_MSG
#define MSG_PING            TYPE_KEEPALIVE_REQ
#define MSG_PONG            TYPE_KEEPALIVE_RESP
#define MSG_BYE             TYPE_DISCONNECT
#define MSG_AUTH            TYPE_AUTHENTICATE
#define MSG_PRIVATE         TYPE_PRIVATE_MSG
#define MSG_ERROR           TYPE_ERROR_RESP
#define MSG_SERVER_INFO     TYPE_SERVER_NOTIFY
#define MSG_LIST            TYPE_USER_LIST_REQ
#define MSG_HISTORY         TYPE_HISTORY_REQ
#define MSG_HISTORY_DATA    TYPE_HISTORY_RESP
#define MSG_HELP            TYPE_HELP_REQ


#define MSG_ACK             TYPE_ACK

#endif // MESSAGES_H