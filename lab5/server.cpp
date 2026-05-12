#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "messages.h"

struct UserSession {
    int socketDescriptor;
    std::string userName;
    std::string ipAddress;
    int portNumber;
    bool isLoggedIn = false;
};

struct StoredMessage {
    std::string fromUser;
    std::string toUser;
    std::string content;
    time_t timeStamp;
    uint32_t messageId;
};

struct TaskQueue {
    int items[QUEUE_SIZE];
    int head = 0;
    int tail = 0;
    int itemCount = 0;

    pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t hasItems = PTHREAD_COND_INITIALIZER;
    pthread_cond_t hasSpace = PTHREAD_COND_INITIALIZER;
};

TaskQueue connectionQueue;
UserSession activeUsers[MAX_CLIENTS];
int totalUsers = 0;

std::vector<StoredMessage> pendingMessages;

pthread_mutex_t userLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t storageLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t historyLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t idLock = PTHREAD_MUTEX_INITIALIZER;

uint32_t nextMessageId = 1;
const std::string DATA_FILE = "history.json";

std::string getCurrentTimeString(time_t rawTime)
{
    char buffer[MAX_TIME_STR];
    tm* timeInfo = localtime(&rawTime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
    return std::string(buffer);
}

std::string escapeJsonString(const std::string& input)
{
    std::string output;
    for (char character : input)
    {
        if (character == '\\') output += "\\\\";
        else if (character == '"') output += "\\\"";
        else if (character == '\n') output += "\\n";
        else output += character;
    }
    return output;
}

const char* getMessageTypeName(uint8_t typeCode)
{
    switch (typeCode)
    {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        default: return "UNKNOWN";
    }
}

uint32_t generateMessageId()
{
    pthread_mutex_lock(&idLock);
    uint32_t newId = nextMessageId++;
    pthread_mutex_unlock(&idLock);
    return newId;
}


void logIncomingPacket(const NetworkPacket& packet, int socket, ssize_t bytesReceived)
{
    sockaddr_in remoteAddr{}, localAddr{};
    socklen_t addressSize = sizeof(remoteAddr);
    getpeername(socket, (sockaddr*)&remoteAddr, &addressSize);

    addressSize = sizeof(localAddr);
    getsockname(socket, (sockaddr*)&localAddr, &addressSize);

    char sourceIp[INET_ADDRSTRLEN];
    char destIp[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &remoteAddr.sin_addr, sourceIp, sizeof(sourceIp));
    inet_ntop(AF_INET, &localAddr.sin_addr, destIp, sizeof(destIp));

    std::cout << "[Network Access] frame received via network interface" << std::endl;
    std::cout << "[Internet] src=" << sourceIp << " dst=" << destIp << " proto=TCP" << std::endl;
    std::cout << "[Transport] recv() " << bytesReceived << " bytes via TCP" << std::endl;
    std::cout << "[Application] deserialize MessageEx -> " << getMessageTypeName(packet.message_type) << std::endl;
}

void logOutgoingPacket(uint8_t typeCode, int socket)
{
    sockaddr_in remoteAddr{};
    socklen_t addressSize = sizeof(remoteAddr);
    getpeername(socket, (sockaddr*)&remoteAddr, &addressSize);

    char destIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &remoteAddr.sin_addr, destIp, sizeof(destIp));

    std::cout << "[Application] prepare " << getMessageTypeName(typeCode) << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = " << destIp << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
}

ssize_t sendComplete(int socket, const void* dataBuffer, size_t dataLength)
{
    size_t totalSent = 0;
    const char* bufferPtr = (const char*)dataBuffer;

    while (totalSent < dataLength)
    {
        ssize_t bytesSent = send(socket, bufferPtr + totalSent, dataLength - totalSent, 0);
        if (bytesSent <= 0)
            return bytesSent;
        totalSent += bytesSent;
    }

    return totalSent;
}

ssize_t receiveComplete(int socket, void* dataBuffer, size_t dataLength)
{
    size_t totalReceived = 0;
    char* bufferPtr = (char*)dataBuffer;

    while (totalReceived < dataLength)
    {
        ssize_t bytesReceived = recv(socket, bufferPtr + totalReceived, dataLength - totalReceived, 0);
        if (bytesReceived <= 0)
            return bytesReceived;
        totalReceived += bytesReceived;
    }

    return totalReceived;
}

bool transmitMessage(int socket, uint8_t typeCode,
                     const std::string& senderName,
                     const std::string& receiverName,
                     const std::string& messageText,
                     uint32_t customId = 0,
                     time_t customTime = 0)
{
    NetworkPacket packet;
    memset(&packet, 0, sizeof(packet));

    packet.message_length = sizeof(NetworkPacket) - sizeof(packet.message_length);
    packet.message_type = typeCode;
    packet.message_id = customId ? customId : generateMessageId();
    packet.message_timestamp = customTime ? customTime : time(nullptr);

    strncpy(packet.from_username, senderName.c_str(), MAX_NAME - 1);
    strncpy(packet.to_username, receiverName.c_str(), MAX_NAME - 1);
    strncpy(packet.message_payload, messageText.c_str(), MAX_PAYLOAD - 1);

    logOutgoingPacket(typeCode, socket);
    return sendComplete(socket, &packet, sizeof(packet)) == sizeof(packet);
}

bool receiveMessage(int socket, NetworkPacket& packet)
{
    ssize_t result = receiveComplete(socket, &packet, sizeof(packet));

    if (result == sizeof(packet))
    {
        logIncomingPacket(packet, socket, result);
        return true;
    }

    return false;
}


void saveToHistory(uint32_t msgId, time_t timeStamp,
                   const std::string& sender,
                   const std::string& receiver,
                   const std::string& msgType,
                   const std::string& content,
                   bool wasDelivered,
                   bool isOfflineMsg)
{
    pthread_mutex_lock(&historyLock);

    std::ofstream outputFile(DATA_FILE, std::ios::app);

    outputFile << "{"
               << "\"msg_id\":" << msgId << ","
               << "\"timestamp\":" << timeStamp << ","
               << "\"sender\":\"" << escapeJsonString(sender) << "\","
               << "\"receiver\":\"" << escapeJsonString(receiver) << "\","
               << "\"type\":\"" << msgType << "\","
               << "\"text\":\"" << escapeJsonString(content) << "\","
               << "\"delivered\":" << (wasDelivered ? "true" : "false") << ","
               << "\"is_offline\":" << (isOfflineMsg ? "true" : "false")
               << "}\n";

    outputFile.close();
    pthread_mutex_unlock(&historyLock);
}

void updateDeliveryStatus(uint32_t msgId)
{
    pthread_mutex_lock(&historyLock);

    std::ifstream inputFile(DATA_FILE);
    std::ostringstream streamBuffer;
    streamBuffer << inputFile.rdbuf();
    std::string fileContent = streamBuffer.str();
    inputFile.close();

    std::string idPattern = "\"msg_id\":" + std::to_string(msgId);
    size_t idPosition = fileContent.find(idPattern);

    if (idPosition != std::string::npos)
    {
        size_t endPosition = fileContent.find("}", idPosition);
        size_t deliveredPosition = fileContent.find("\"delivered\":false", idPosition);

        if (deliveredPosition != std::string::npos && deliveredPosition < endPosition)
        {
            fileContent.replace(deliveredPosition, strlen("\"delivered\":false"), "\"delivered\":true");

            std::ofstream outputFile(DATA_FILE);
            outputFile << fileContent;
            outputFile.close();
        }
    }

    pthread_mutex_unlock(&historyLock);
}

std::vector<std::string> getRecentHistory(int lineCount)
{
    pthread_mutex_lock(&historyLock);

    std::ifstream inputFile(DATA_FILE);
    std::vector<std::string> allLines;
    std::string currentLine;

    while (std::getline(inputFile, currentLine))
    {
        if (!currentLine.empty())
            allLines.push_back(currentLine);
    }

    inputFile.close();
    pthread_mutex_unlock(&historyLock);

    if (lineCount <= 0)
        lineCount = 10;

    int startIndex = (int)allLines.size() - lineCount;
    if (startIndex < 0)
        startIndex = 0;

    std::vector<std::string> result;

    for (int i = startIndex; i < (int)allLines.size(); i++)
        result.push_back(allLines[i]);

    return result;
}

void addToQueue(int socketDescriptor)
{
    pthread_mutex_lock(&connectionQueue.queueMutex);

    while (connectionQueue.itemCount == QUEUE_SIZE)
        pthread_cond_wait(&connectionQueue.hasSpace, &connectionQueue.queueMutex);

    connectionQueue.items[connectionQueue.tail] = socketDescriptor;
    connectionQueue.tail = (connectionQueue.tail + 1) % QUEUE_SIZE;
    connectionQueue.itemCount++;

    pthread_cond_signal(&connectionQueue.hasItems);
    pthread_mutex_unlock(&connectionQueue.queueMutex);
}

int removeFromQueue()
{
    pthread_mutex_lock(&connectionQueue.queueMutex);

    while (connectionQueue.itemCount == 0)
        pthread_cond_wait(&connectionQueue.hasItems, &connectionQueue.queueMutex);

    int socketDescriptor = connectionQueue.items[connectionQueue.head];
    connectionQueue.head = (connectionQueue.head + 1) % QUEUE_SIZE;
    connectionQueue.itemCount--;

    pthread_cond_signal(&connectionQueue.hasSpace);
    pthread_mutex_unlock(&connectionQueue.queueMutex);

    return socketDescriptor;
}


bool isNicknameTaken(const std::string& nickname)
{
    for (int i = 0; i < totalUsers; i++)
    {
        if (activeUsers[i].isLoggedIn && activeUsers[i].userName == nickname)
            return true;
    }
    return false;
}

UserSession* findUserByNickname(const std::string& nickname)
{
    for (int i = 0; i < totalUsers; i++)
    {
        if (activeUsers[i].isLoggedIn && activeUsers[i].userName == nickname)
            return &activeUsers[i];
    }
    return nullptr;
}

void registerUser(const UserSession& newUser)
{
    pthread_mutex_lock(&userLock);
    if (totalUsers < MAX_CLIENTS)
        activeUsers[totalUsers++] = newUser;
    pthread_mutex_unlock(&userLock);
}

void unregisterUser(int socketDescriptor)
{
    pthread_mutex_lock(&userLock);
    for (int i = 0; i < totalUsers; i++)
    {
        if (activeUsers[i].socketDescriptor == socketDescriptor)
        {
            for (int j = i; j < totalUsers - 1; j++)
                activeUsers[j] = activeUsers[j + 1];
            totalUsers--;
            break;
        }
    }
    pthread_mutex_unlock(&userLock);
}


void broadcastPublicMessage(const std::string& sender, const std::string& content)
{
    uint32_t msgId = generateMessageId();
    time_t currentTime = time(nullptr);

    std::cout << "[" << getCurrentTimeString(currentTime) << "][id=" << msgId << "][" << sender << "]: " << content << std::endl;

    saveToHistory(msgId, currentTime, sender, "", "MSG_TEXT", content, true, false);

    pthread_mutex_lock(&userLock);
    for (int i = 0; i < totalUsers; i++)
        transmitMessage(activeUsers[i].socketDescriptor, MSG_TEXT, sender, "", content, msgId, currentTime);
    pthread_mutex_unlock(&userLock);
}

bool sendPrivateMessageOnline(const std::string& fromUser,
                              const std::string& toUser,
                              const std::string& content,
                              uint32_t msgId,
                              time_t timeStamp)
{
    pthread_mutex_lock(&userLock);
    UserSession* recipient = findUserByNickname(toUser);
    if (recipient != nullptr)
    {
        transmitMessage(recipient->socketDescriptor, MSG_PRIVATE, fromUser, toUser, content, msgId, timeStamp);
        pthread_mutex_unlock(&userLock);
        return true;
    }
    pthread_mutex_unlock(&userLock);
    return false;
}

void queueOfflineMessage(const std::string& fromUser,
                         const std::string& toUser,
                         const std::string& content,
                         uint32_t msgId,
                         time_t timeStamp)
{
    StoredMessage pending;
    pending.fromUser = fromUser;
    pending.toUser = toUser;
    pending.content = content;
    pending.messageId = msgId;
    pending.timeStamp = timeStamp;

    pthread_mutex_lock(&storageLock);
    pendingMessages.push_back(pending);
    pthread_mutex_unlock(&storageLock);

    saveToHistory(msgId, timeStamp, fromUser, toUser, "MSG_PRIVATE", content, false, true);
}

void deliverPendingMessages(const UserSession& user)
{
    pthread_mutex_lock(&storageLock);
    bool hasMessages = false;
    for (auto iterator = pendingMessages.begin(); iterator != pendingMessages.end(); )
    {
        if (iterator->toUser == user.userName)
        {
            hasMessages = true;
            transmitMessage(user.socketDescriptor,
                          MSG_PRIVATE,
                          iterator->fromUser,
                          iterator->toUser,
                          "[OFFLINE] " + iterator->content,
                          iterator->messageId,
                          iterator->timeStamp);
            updateDeliveryStatus(iterator->messageId);
            iterator = pendingMessages.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    if (!hasMessages)
        std::cout << "[Application] no offline messages for " << user.userName << std::endl;
    pthread_mutex_unlock(&storageLock);
}


void sendUserListResponse(int socket)
{
    std::string userList = "[SERVER]: Online users\n";
    pthread_mutex_lock(&userLock);
    for (int i = 0; i < totalUsers; i++)
    {
        if (activeUsers[i].isLoggedIn)
            userList += activeUsers[i].userName + "\n";
    }
    pthread_mutex_unlock(&userLock);
    transmitMessage(socket, MSG_SERVER_INFO, "server", "", userList);
}

void sendChatHistory(int socket, const std::string& parameter)
{
    int requestedLines = 10;
    if (!parameter.empty())
    {
        try
        {
            requestedLines = std::stoi(parameter);
            if (requestedLines <= 0)
                requestedLines = 10;
        }
        catch (...)
        {
            transmitMessage(socket, MSG_ERROR, "server", "", "[SERVER]: Invalid history parameter");
            return;
        }
    }
    std::vector<std::string> historyLines = getRecentHistory(requestedLines);
    if (historyLines.empty())
    {
        transmitMessage(socket, MSG_HISTORY_DATA, "server", "", "[SERVER]: History is empty");
        return;
    }
    for (const std::string& line : historyLines)
        transmitMessage(socket, MSG_HISTORY_DATA, "server", "", line);
}


void* handleClientConnection(void*)
{
    while (true)
    {
        int clientSocket = removeFromQueue();
        sockaddr_in remoteAddress{};
        socklen_t addressLength = sizeof(remoteAddress);
        getpeername(clientSocket, (sockaddr*)&remoteAddress, &addressLength);
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &remoteAddress.sin_addr, clientIp, sizeof(clientIp));
        int clientPort = ntohs(remoteAddress.sin_port);
        std::cout << "Client connected" << std::endl;
        
        NetworkPacket incomingPacket;
        if (!receiveMessage(clientSocket, incomingPacket))
        {
            close(clientSocket);
            continue;
        }
        
        if (incomingPacket.message_type != MSG_HELLO)
        {
            transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Expected HELLO");
            close(clientSocket);
            continue;
        }
        
        transmitMessage(clientSocket, MSG_WELCOME, "server", "", "Welcome");
        
        if (!receiveMessage(clientSocket, incomingPacket))
        {
            close(clientSocket);
            continue;
        }
        
        if (incomingPacket.message_type != MSG_AUTH)
        {
            transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Expected AUTH");
            close(clientSocket);
            continue;
        }
        
        std::string chosenNickname = incomingPacket.from_username;
        if (chosenNickname.empty())
            chosenNickname = incomingPacket.message_payload;
        
        if (chosenNickname.empty())
        {
            transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Empty nickname");
            close(clientSocket);
            continue;
        }
        
        if (chosenNickname.size() >= MAX_NAME)
        {
            transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Nickname too long");
            close(clientSocket);
            continue;
        }
        
        pthread_mutex_lock(&userLock);
        bool nicknameTaken = isNicknameTaken(chosenNickname);
        pthread_mutex_unlock(&userLock);
        
        if (nicknameTaken)
        {
            transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Nickname already in use");
            close(clientSocket);
            continue;
        }
        
        std::cout << "[Application] authentication success: " << chosenNickname << std::endl;
        
        UserSession newUser;
        newUser.socketDescriptor = clientSocket;
        newUser.userName = chosenNickname;
        newUser.ipAddress = clientIp;
        newUser.portNumber = clientPort;
        newUser.isLoggedIn = true;
        
        registerUser(newUser);
        transmitMessage(clientSocket, MSG_SERVER_INFO, "server", "", "[SERVER]: authentication success");
        deliverPendingMessages(newUser);
        
        std::cout << "User [" << newUser.userName << "] connected" << std::endl;
        
        while (true)
        {
            if (!receiveMessage(clientSocket, incomingPacket))
            {
                std::cout << "User [" << newUser.userName << "] disconnected" << std::endl;
                unregisterUser(clientSocket);
                close(clientSocket);
                break;
            }
            
            if (incomingPacket.message_type == MSG_TEXT)
            {
                std::cout << "[Application] handle MSG_TEXT" << std::endl;
                broadcastPublicMessage(newUser.userName, incomingPacket.message_payload);
            }
            else if (incomingPacket.message_type == MSG_PRIVATE)
            {
                std::string targetUser = incomingPacket.to_username;
                std::string messageText = incomingPacket.message_payload;
                
                if (targetUser.empty())
                {
                    transmitMessage(clientSocket, MSG_ERROR, "server", "", "[SERVER]: Empty receiver");
                    continue;
                }
                
                uint32_t msgId = generateMessageId();
                time_t currentTime = time(nullptr);
                
                std::cout << "[" << getCurrentTimeString(currentTime) << "][id=" << msgId << "][PRIVATE]["
                          << newUser.userName << " -> " << targetUser << "]: " << messageText << std::endl;
                
                if (sendPrivateMessageOnline(newUser.userName, targetUser, messageText, msgId, currentTime))
                {
                    saveToHistory(msgId, currentTime, newUser.userName, targetUser, "MSG_PRIVATE", messageText, true, false);
                }
                else
                {
                    std::cout << "[Application] receiver " << targetUser << " is offline" << std::endl;
                    std::cout << "[Application] store message in offline queue" << std::endl;
                    std::cout << "[Application] append record to history file delivered=false" << std::endl;
                    
                    queueOfflineMessage(newUser.userName, targetUser, messageText, msgId, currentTime);
                    transmitMessage(clientSocket, MSG_SERVER_INFO, "server", "", "[SERVER]: user offline, message stored");
                }
            }
            else if (incomingPacket.message_type == MSG_LIST)
            {
                std::cout << "[Application] handle MSG_LIST" << std::endl;
                sendUserListResponse(clientSocket);
            }
            else if (incomingPacket.message_type == MSG_HISTORY)
            {
                std::cout << "[Application] handle MSG_HISTORY" << std::endl;
                sendChatHistory(clientSocket, incomingPacket.message_payload);
            }
            else if (incomingPacket.message_type == MSG_PING)
            {
                transmitMessage(clientSocket, MSG_PONG, "server", newUser.userName, "PONG");
            }
            else if (incomingPacket.message_type == MSG_BYE)
            {
                std::cout << "User [" << newUser.userName << "] disconnected" << std::endl;
                transmitMessage(clientSocket, MSG_BYE, "server", newUser.userName, "Disconnected");
                unregisterUser(clientSocket);
                close(clientSocket);
                break;
            }
            else
            {
                transmitMessage(clientSocket, MSG_ERROR, "server", newUser.userName, "[SERVER]: Unsupported message type");
            }
        }
    }
    return nullptr;
}


int main()
{
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        perror("socket");
        return 1;
    }
    
    int socketOption = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &socketOption, sizeof(socketOption)) < 0)
    {
        perror("setsockopt");
        close(serverSocket);
        return 1;
    }
    
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(serverSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) < 0)
    {
        perror("bind");
        close(serverSocket);
        return 1;
    }
    
    if (listen(serverSocket, 10) < 0)
    {
        perror("listen");
        close(serverSocket);
        return 1;
    }
    
    pthread_t workerThreads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++)
    {
        pthread_create(&workerThreads[i], nullptr, handleClientConnection, nullptr);
        pthread_detach(workerThreads[i]);
    }
    
    std::cout << "Server started on port " << PORT << std::endl;
    
    while (true)
    {
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        int clientSocket = accept(serverSocket, (sockaddr*)&clientAddress, &addressLength);
        if (clientSocket < 0)
        {
            perror("accept");
            continue;
        }
        addToQueue(clientSocket);
    }
    
    close(serverSocket);
    return 0;
}