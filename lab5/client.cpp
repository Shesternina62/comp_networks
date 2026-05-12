#include <iostream>
#include <cstring>
#include <string>
#include <cstdlib>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "messages.h"


int networkSocket = -1;
std::string currentUser;
bool isRunning = true;
bool isConnected = false;
bool authFailed = false;

pthread_mutex_t socketLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t connectionLock = PTHREAD_MUTEX_INITIALIZER;


std::string formatTimestamp(time_t rawTime)
{
    char buffer[MAX_TIMESTAMP_STR];
    tm* timeInfo = localtime(&rawTime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
    return std::string(buffer);
}


ssize_t sendComplete(int socketDescriptor, const void* dataBuffer, size_t dataLength)
{
    size_t totalSent = 0;
    const char* bufferPtr = (const char*)dataBuffer;

    while (totalSent < dataLength)
    {
        ssize_t bytesSent = send(socketDescriptor, bufferPtr + totalSent, dataLength - totalSent, 0);
        if (bytesSent <= 0)
            return bytesSent;
        totalSent += bytesSent;
    }
    return totalSent;
}

ssize_t receiveComplete(int socketDescriptor, void* dataBuffer, size_t dataLength)
{
    size_t totalReceived = 0;
    char* bufferPtr = (char*)dataBuffer;

    while (totalReceived < dataLength)
    {
        ssize_t bytesReceived = recv(socketDescriptor, bufferPtr + totalReceived, dataLength - totalReceived, 0);
        if (bytesReceived <= 0)
            return bytesReceived;
        totalReceived += bytesReceived;
    }
    return totalReceived;
}


void initializePacket(NetworkPacket& packet, uint8_t msgType, 
                      const std::string& fromUser,
                      const std::string& toUser, 
                      const std::string& content)
{
    memset(&packet, 0, sizeof(packet));

    packet.message_length = sizeof(NetworkPacket) - sizeof(packet.message_length);
    packet.message_type = msgType;
    packet.message_id = 0;
    packet.message_timestamp = time(nullptr);

    strncpy(packet.from_username, fromUser.c_str(), MAX_USERNAME_LEN - 1);
    strncpy(packet.to_username, toUser.c_str(), MAX_USERNAME_LEN - 1);
    strncpy(packet.message_payload, content.c_str(), MAX_PAYLOAD_SIZE - 1);
}

bool sendPacket(int socketDescriptor, uint8_t msgType, 
                const std::string& fromUser,
                const std::string& toUser, 
                const std::string& content)
{
    NetworkPacket packet;
    initializePacket(packet, msgType, fromUser, toUser, content);
    return sendComplete(socketDescriptor, &packet, sizeof(packet)) == sizeof(packet);
}

bool receivePacket(int socketDescriptor, NetworkPacket& packet)
{
    ssize_t result = receiveComplete(socketDescriptor, &packet, sizeof(packet));
    return result == sizeof(packet);
}

void closeCurrentConnection()
{
    pthread_mutex_lock(&socketLock);
    if (networkSocket >= 0)
    {
        close(networkSocket);
        networkSocket = -1;
    }
    pthread_mutex_unlock(&socketLock);
    
    pthread_mutex_lock(&connectionLock);
    isConnected = false;
    pthread_mutex_unlock(&connectionLock);
}

bool establishConnection()
{
    int newSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (newSocket < 0)
        return false;

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(SERVER_PORT);
    serverAddress.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(newSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) < 0)
    {
        close(newSocket);
        return false;
    }

    if (!sendPacket(newSocket, TYPE_HANDSHAKE, currentUser, "", "HELLO"))
    {
        close(newSocket);
        return false;
    }

    NetworkPacket incomingPacket;
    if (!receivePacket(newSocket, incomingPacket))
    {
        close(newSocket);
        return false;
    }

    if (incomingPacket.message_type != TYPE_WELCOME)
    {
        close(newSocket);
        return false;
    }

    if (!sendPacket(newSocket, TYPE_AUTHENTICATE, currentUser, "", currentUser))
    {
        close(newSocket);
        return false;
    }

    if (!receivePacket(newSocket, incomingPacket))
    {
        close(newSocket);
        return false;
    }

    if (incomingPacket.message_type == TYPE_ERROR_RESP)
    {
        std::cout << incomingPacket.message_payload << std::endl;
        authFailed = true;
        close(newSocket);
        return false;
    }

    pthread_mutex_lock(&socketLock);
    networkSocket = newSocket;
    pthread_mutex_unlock(&socketLock);

    pthread_mutex_lock(&connectionLock);
    isConnected = true;
    pthread_mutex_unlock(&connectionLock);

    std::cout << "Connected" << std::endl;
    std::cout << incomingPacket.message_payload << std::endl;

    return true;
}

bool getActiveSocket(int& activeSocket)
{
    pthread_mutex_lock(&connectionLock);
    bool connectedStatus = isConnected;
    pthread_mutex_unlock(&connectionLock);

    if (!connectedStatus)
        return false;

    pthread_mutex_lock(&socketLock);
    activeSocket = networkSocket;
    pthread_mutex_unlock(&socketLock);

    return activeSocket >= 0;
}


void displayHelp()
{
    std::cout << "/help - Show this help" << std::endl;
    std::cout << "/history - Show last 10 messages" << std::endl;
    std::cout << "/history N - Show last N messages" << std::endl;
    std::cout << "/list - Show online users" << std::endl;
    std::cout << "/quit - Exit the chat" << std::endl;
    std::cout << "/w <nick> <message> - Send private message" << std::endl;
    std::cout << "/ping - Check server connection" << std::endl;
}

void displayPacket(const NetworkPacket& packet)
{
    if (packet.message_type == TYPE_PUBLIC_MSG)
    {
        std::cout << "[" << formatTimestamp(packet.message_timestamp) 
                  << "][id=" << packet.message_id
                  << "][" << packet.from_username << "]: " 
                  << packet.message_payload << std::endl;
    }
    else if (packet.message_type == TYPE_PRIVATE_MSG)
    {
        std::string messageText = packet.message_payload;
        bool isOffline = false;

        if (messageText.rfind("[OFFLINE] ", 0) == 0)
        {
            isOffline = true;
            messageText = messageText.substr(10);
        }

        std::cout << "[" << formatTimestamp(packet.message_timestamp) 
                  << "][id=" << packet.message_id << "]";
        if (isOffline)
            std::cout << "[OFFLINE]";

        std::cout << "[PRIVATE][" << packet.from_username << " -> " 
                  << packet.to_username << "]: " << messageText << std::endl;
    }
    else if (packet.message_type == TYPE_HISTORY_RESP)
    {
        std::cout << packet.message_payload << std::endl;
    }
    else if (packet.message_type == TYPE_SERVER_NOTIFY)
    {
        std::cout << packet.message_payload << std::endl;
    }
    else if (packet.message_type == TYPE_ERROR_RESP)
    {
        std::cout << packet.message_payload << std::endl;
    }
    else if (packet.message_type == TYPE_KEEPALIVE_RESP)
    {
        std::cout << "[SERVER]: PONG" << std::endl;
    }
    else if (packet.message_type == TYPE_DISCONNECT)
    {
        std::cout << "Disconnected" << std::endl;
    }
}


void* messageReceiver(void*)
{
    while (isRunning)
    {
        if (authFailed)
        {
            isRunning = false;
            break;
        }

        pthread_mutex_lock(&connectionLock);
        bool connectedStatus = isConnected;
        pthread_mutex_unlock(&connectionLock);

        if (!connectedStatus)
        {
            sleep(2);
            if (!isRunning || authFailed)
                break;
            std::cout << "Reconnecting..." << std::endl;
            if (establishConnection())
                std::cout << "Reconnected" << std::endl;
            continue;
        }

        pthread_mutex_lock(&socketLock);
        int currentSocket = networkSocket;
        pthread_mutex_unlock(&socketLock);

        NetworkPacket incomingPacket;
        if (!receivePacket(currentSocket, incomingPacket))
        {
            std::cout << "Connection lost" << std::endl;
            closeCurrentConnection();
            continue;
        }

        displayPacket(incomingPacket);
    }
    return nullptr;
}


int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, currentUser);

    if (currentUser.empty())
    {
        std::cout << "Nickname cannot be empty" << std::endl;
        return 1;
    }

    if (establishConnection())
        std::cout << "Type /help for commands" << std::endl;
    else
        std::cout << "Initial connection failed, waiting for reconnect..." << std::endl;

    if (authFailed)
        return 1;

    pthread_t receiverThread;
    pthread_create(&receiverThread, nullptr, messageReceiver, nullptr);

    std::string userInput;

    while (isRunning)
    {
        std::cout << "> ";
        if (!std::getline(std::cin, userInput))
            break;

        int activeSocket;
        
        if (userInput == "/help")
        {
            displayHelp();
        }
        else if (userInput == "/quit")
        {
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_DISCONNECT, currentUser, "", "");
            isRunning = false;
            break;
        }
        else if (userInput == "/ping")
        {
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_KEEPALIVE_REQ, currentUser, "", "");
        }
        else if (userInput == "/list")
        {
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_USER_LIST_REQ, currentUser, "", "");
        }
        else if (userInput == "/history")
        {
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_HISTORY_REQ, currentUser, "", "");
        }
        else if (userInput.rfind("/history ", 0) == 0)
        {
            std::string lineCount = userInput.substr(9);
            if (lineCount.empty())
            {
                std::cout << "Usage: /history N" << std::endl;
                continue;
            }
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_HISTORY_REQ, currentUser, "", lineCount);
        }
        else if (userInput.rfind("/w ", 0) == 0)
        {
            std::string remaining = userInput.substr(3);
            size_t firstSpace = remaining.find(' ');
            if (firstSpace == std::string::npos)
            {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
                continue;
            }
            std::string targetUser = remaining.substr(0, firstSpace);
            std::string messageContent = remaining.substr(firstSpace + 1);
            if (targetUser.empty() || messageContent.empty())
            {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
                continue;
            }
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_PRIVATE_MSG, currentUser, targetUser, messageContent);
        }
        else if (!userInput.empty())
        {
            if (getActiveSocket(activeSocket))
                sendPacket(activeSocket, TYPE_PUBLIC_MSG, currentUser, "", userInput);
        }
    }

    closeCurrentConnection();
    pthread_join(receiverThread, nullptr);

    return 0;
}