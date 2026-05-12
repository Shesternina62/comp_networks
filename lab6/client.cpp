#include <iostream>
#include <cstring>
#include <string>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <vector>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "messages.h"

typedef struct {
    uint32_t msg_id;
    std::chrono::steady_clock::time_point send_time;
    bool received;
    double rtt_ms;
} PingRecord;

typedef struct {
    double avg_rtt;
    double avg_jitter;
    double loss_percent;
} NetworkStats;

int networkSocket = -1;
std::string currentUser;
bool isRunning = true;
bool isConnected = false;
bool authFailed = false;

std::map<uint32_t, PendingMessage> pendingQueue;
pthread_mutex_t pendingMutex = PTHREAD_MUTEX_INITIALIZER;


std::map<uint32_t, PingRecord> pingRecords;
pthread_mutex_t pingMutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<double> rttHistory;
std::vector<double> jitterHistory;
int totalPingsSent = 0;
int totalPingsReceived = 0;
pthread_mutex_t statsMutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t nextMsgId = 1;
pthread_mutex_t idMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t socketLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t connectionLock = PTHREAD_MUTEX_INITIALIZER;

ssize_t sendComplete(int socketDescriptor, const void* dataBuffer, size_t dataLength);
const char* getMessageTypeName(uint8_t typeCode);

std::string formatTimestamp(time_t rawTime)
{
    char buffer[MAX_TIMESTAMP_STR];
    tm* timeInfo = localtime(&rawTime);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
    return std::string(buffer);
}

uint32_t generateMessageId()
{
    pthread_mutex_lock(&idMutex);
    uint32_t newId = nextMsgId++;
    pthread_mutex_unlock(&idMutex);
    return newId;
}

const char* getMessageTypeName(uint8_t typeCode)
{
    switch (typeCode)
    {
        case TYPE_HANDSHAKE: return "MSG_HELLO";
        case TYPE_WELCOME: return "MSG_WELCOME";
        case TYPE_PUBLIC_MSG: return "MSG_TEXT";
        case TYPE_KEEPALIVE_REQ: return "MSG_PING";
        case TYPE_KEEPALIVE_RESP: return "MSG_PONG";
        case TYPE_DISCONNECT: return "MSG_BYE";
        case TYPE_AUTHENTICATE: return "MSG_AUTH";
        case TYPE_PRIVATE_MSG: return "MSG_PRIVATE";
        case TYPE_ERROR_RESP: return "MSG_ERROR";
        case TYPE_SERVER_NOTIFY: return "MSG_SERVER_INFO";
        case TYPE_USER_LIST_REQ: return "MSG_LIST";
        case TYPE_HISTORY_REQ: return "MSG_HISTORY";
        case TYPE_HISTORY_RESP: return "MSG_HISTORY_DATA";
        case TYPE_HELP_REQ: return "MSG_HELP";
        case TYPE_ACK: return "MSG_ACK";
        default: return "UNKNOWN";
    }
}

void addToPendingQueue(const NetworkPacket& packet)
{
    pthread_mutex_lock(&pendingMutex);
    PendingMessage pending;
    pending.msg = packet;
    pending.send_time = time(nullptr); 
    pending.retries = 0;
    pendingQueue[packet.message_id] = pending;
    pthread_mutex_unlock(&pendingMutex);
    
    std::cout << "[Transport][RETRY] send " << getMessageTypeName(packet.message_type) 
              << " (id=" << packet.message_id << ")" << std::endl;
}

void removeFromPendingQueue(uint32_t msg_id)
{
    pthread_mutex_lock(&pendingMutex);
    auto it = pendingQueue.find(msg_id);
    if (it != pendingQueue.end()) {
        std::cout << "[Transport][RETRY] ACK received (id=" << msg_id << ")" << std::endl;
        pendingQueue.erase(it);
    }
    pthread_mutex_unlock(&pendingMutex);
}

void markForRetransmission(uint32_t msg_id)
{
    pthread_mutex_lock(&pendingMutex);
    auto it = pendingQueue.find(msg_id);
    if (it != pendingQueue.end()) {
        it->second.retries++;
        std::cout << "[Transport][RETRY] wait ACK timeout" << std::endl;
        std::cout << "[Transport][RETRY] resend " << it->second.retries << "/" << MAX_RETRIES 
                  << " (id=" << msg_id << ")" << std::endl;
        
        if (it->second.retries >= MAX_RETRIES) {
            std::cout << "[Transport][RETRY] max retries exceeded, giving up (id=" << msg_id << ")" << std::endl;
            pendingQueue.erase(it);
        }
    }
    pthread_mutex_unlock(&pendingMutex);
}


bool resendMessage(const PendingMessage& pending) {
    int socket;
    pthread_mutex_lock(&socketLock);
    socket = networkSocket;
    pthread_mutex_unlock(&socketLock);
    
    if (socket < 0) return false;
    
    return sendComplete(socket, &pending.msg, sizeof(pending.msg)) == sizeof(pending.msg);
}

void* timeoutChecker(void*)
{
    while (isRunning) {
        sleep(1);  // Проверяем каждую секунду
        
        time_t now = time(nullptr);
        std::vector<uint32_t> timedOut;
        
        pthread_mutex_lock(&pendingMutex);
        for (auto& pair : pendingQueue) {
            auto elapsed = now - pair.second.send_time;
            if (elapsed >= ACK_TIMEOUT_SEC) {
                timedOut.push_back(pair.first);
            }
        }
        pthread_mutex_unlock(&pendingMutex);
        
        for (uint32_t msg_id : timedOut) {
            pthread_mutex_lock(&pendingMutex);
            auto it = pendingQueue.find(msg_id);
            if (it != pendingQueue.end()) {
                if (it->second.retries < MAX_RETRIES) {
                    // Обновляем время отправки и отправляем снова
                    it->second.send_time = now;
                    it->second.retries++;
                    std::cout << "[Transport][RETRY] resend " << it->second.retries << "/" << MAX_RETRIES 
                              << " (id=" << msg_id << ")" << std::endl;
                    pthread_mutex_unlock(&pendingMutex);
                    
                    resendMessage(it->second);
                } else {
                    std::cout << "[Transport][RETRY] max retries exceeded, giving up (id=" << msg_id << ")" << std::endl;
                    pendingQueue.erase(it);
                    pthread_mutex_unlock(&pendingMutex);
                }
            } else {
                pthread_mutex_unlock(&pendingMutex);
            }
        }
    }
    return nullptr;
}


void recordPingSent(uint32_t msg_id)
{
    pthread_mutex_lock(&pingMutex);
    PingRecord record;
    record.msg_id = msg_id;
    record.send_time = std::chrono::steady_clock::now();
    record.received = false;
    record.rtt_ms = 0;
    pingRecords[msg_id] = record;
    
    pthread_mutex_lock(&statsMutex);
    totalPingsSent++;
    pthread_mutex_unlock(&statsMutex);
    pthread_mutex_unlock(&pingMutex);
}

void recordPongReceived(uint32_t msg_id)
{
    pthread_mutex_lock(&pingMutex);
    auto it = pingRecords.find(msg_id);
    if (it != pingRecords.end() && !it->second.received) {
        auto now = std::chrono::steady_clock::now();
        double rtt_ms = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.send_time).count() / 1000.0;
        it->second.received = true;
        it->second.rtt_ms = rtt_ms;
        
        pthread_mutex_lock(&statsMutex);
        totalPingsReceived++;
        rttHistory.push_back(rtt_ms);
        
        
        if (rttHistory.size() >= 2) {
            double jitter = fabs(rttHistory[rttHistory.size() - 1] - rttHistory[rttHistory.size() - 2]);
            jitterHistory.push_back(jitter);
            std::cout << "PING " << rttHistory.size() << " → RTT=" << rtt_ms << "ms | Jitter=" << jitter << "ms" << std::endl;
        } else {
            std::cout << "PING " << rttHistory.size() << " → RTT=" << rtt_ms << "ms" << std::endl;
        }
        
        pthread_mutex_unlock(&statsMutex);
    }
    pthread_mutex_unlock(&pingMutex);
}

void printAndSaveStats()
{
    pthread_mutex_lock(&statsMutex);
    
    NetworkStats stats;
    stats.avg_rtt = 0;
    stats.avg_jitter = 0;
    stats.loss_percent = 0;
    
   
    if (!rttHistory.empty()) {
        double sum_rtt = 0;
        for (double rtt : rttHistory) {
            sum_rtt += rtt;
        }
        stats.avg_rtt = sum_rtt / rttHistory.size();
    }
    
    // Вычисляем средний джиттер
    if (!jitterHistory.empty()) {
        double sum_jitter = 0;
        for (double jitter : jitterHistory) {
            sum_jitter += jitter;
        }
        stats.avg_jitter = sum_jitter / jitterHistory.size();
    }
    
    // Вычисляем процент потерь
    if (totalPingsSent > 0) {
        stats.loss_percent = (1.0 - (double)totalPingsReceived / totalPingsSent) * 100.0;
    }
    
    // Выводим статистику
    std::cout << "\n=== NETWORK DIAGNOSTICS ===" << std::endl;
    std::cout << "RTT avg : " << stats.avg_rtt << " ms" << std::endl;
    std::cout << "Jitter  : " << stats.avg_jitter << " ms" << std::endl;
    std::cout << "Loss    : " << stats.loss_percent << " %" << std::endl;
    std::cout << "===========================" << std::endl;
    
    
    std::string filename = "net_diag_" + currentUser + ".json";
    std::ofstream file(filename);
    if (file.is_open()) {
        file << "{\n";
        file << "  \"nickname\": \"" << currentUser << "\",\n";
        file << "  \"timestamp\": \"" << formatTimestamp(time(nullptr)) << "\",\n";
        file << "  \"total_pings_sent\": " << totalPingsSent << ",\n";
        file << "  \"total_pings_received\": " << totalPingsReceived << ",\n";
        file << "  \"avg_rtt_ms\": " << stats.avg_rtt << ",\n";
        file << "  \"avg_jitter_ms\": " << stats.avg_jitter << ",\n";
        file << "  \"loss_percent\": " << stats.loss_percent << "\n";
        file << "}\n";
        file.close();
        std::cout << "Statistics saved to " << filename << std::endl;
    }
    
    pthread_mutex_unlock(&statsMutex);
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
                      const std::string& content,
                      uint32_t custom_id = 0)
{
    memset(&packet, 0, sizeof(packet));

    packet.message_length = sizeof(NetworkPacket) - sizeof(packet.message_length);
    packet.message_type = msgType;
    packet.message_id = custom_id ? custom_id : generateMessageId();
    packet.message_timestamp = time(nullptr);

    strncpy(packet.from_username, fromUser.c_str(), MAX_USERNAME_LEN - 1);
    strncpy(packet.to_username, toUser.c_str(), MAX_USERNAME_LEN - 1);
    strncpy(packet.message_payload, content.c_str(), MAX_PAYLOAD_SIZE - 1);
}

bool sendPacket(int socketDescriptor, uint8_t msgType, 
                const std::string& fromUser,
                const std::string& toUser, 
                const std::string& content,
                bool need_ack = true)
{
    NetworkPacket packet;
    initializePacket(packet, msgType, fromUser, toUser, content);
    
    bool result = sendComplete(socketDescriptor, &packet, sizeof(packet)) == sizeof(packet);
    
    
    if (result && need_ack && msgType != TYPE_KEEPALIVE_REQ && 
        msgType != TYPE_KEEPALIVE_RESP && msgType != TYPE_DISCONNECT &&
        msgType != TYPE_ACK) {
        addToPendingQueue(packet);
    }
    
    if (result && msgType == TYPE_KEEPALIVE_REQ) {
        recordPingSent(packet.message_id);
    }
    
    return result;
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

    if (!sendPacket(newSocket, TYPE_HANDSHAKE, currentUser, "", "HELLO", false))
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

    if (!sendPacket(newSocket, TYPE_AUTHENTICATE, currentUser, "", currentUser, false))
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
    std::cout << "/ping - Send 10 ping requests" << std::endl;
    std::cout << "/ping N - Send N ping requests" << std::endl;
    std::cout << "/netdiag - Show network diagnostics" << std::endl;
}

void handlePingCommand(const std::string& arg)
{
    int count = 10;
    if (!arg.empty()) {
        try {
            count = std::stoi(arg);
            if (count <= 0) count = 10;
            if (count > 100) {
                std::cout << "Maximum 100 pings allowed" << std::endl;
                count = 100;
            }
        } catch (...) {
            std::cout << "Invalid number" << std::endl;
            return;
        }
    }
    
    std::cout << "Sending " << count << " ping requests..." << std::endl;
    
    // Очищаем предыдущую статистику
    pthread_mutex_lock(&statsMutex);
    rttHistory.clear();
    jitterHistory.clear();
    totalPingsSent = 0;
    totalPingsReceived = 0;
    pthread_mutex_unlock(&statsMutex);
    
    pthread_mutex_lock(&pingMutex);
    pingRecords.clear();
    pthread_mutex_unlock(&pingMutex);
    
    int socket;
    if (!getActiveSocket(socket)) {
        std::cout << "Not connected to server" << std::endl;
        return;
    }
    
    for (int i = 0; i < count; i++) {
        sendPacket(socket, TYPE_KEEPALIVE_REQ, currentUser, "", "", false);
        usleep(100000);  // Небольшая задержка между пингами (100 мс)
    }
    
    
    std::cout << "Waiting for responses..." << std::endl;
    sleep(3);
    
    
    pthread_mutex_lock(&pingMutex);
    int timeouts = 0;
    for (auto& pair : pingRecords) {
        if (!pair.second.received) {
            timeouts++;
            std::cout << "PING ? → timeout" << std::endl;
        }
    }
    pthread_mutex_unlock(&pingMutex);
    
    if (timeouts > 0) {
        std::cout << timeouts << " requests timed out" << std::endl;
    }
}

void handleNetDiag()
{
    printAndSaveStats();
}

void displayPacket(const NetworkPacket& packet)
{
    
    if (packet.message_type == TYPE_ACK) {
        std::cout << "[Transport][ACK] recv ACK (id=" << packet.message_id << ")" << std::endl;
        removeFromPendingQueue(packet.message_id);
        return;
    }
    
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
        
        recordPongReceived(packet.message_id);
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

    
    pthread_t timeoutThread;
    pthread_create(&timeoutThread, nullptr, timeoutChecker, nullptr);
    
    
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
                sendPacket(activeSocket, TYPE_DISCONNECT, currentUser, "", "", false);
            isRunning = false;
            break;
        }
        else if (userInput == "/ping")
        {
            handlePingCommand("");
        }
        else if (userInput.rfind("/ping ", 0) == 0)
        {
            handlePingCommand(userInput.substr(6));
        }
        else if (userInput == "/netdiag")
        {
            handleNetDiag();
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
    pthread_cancel(timeoutThread);
    pthread_join(timeoutThread, nullptr);

    return 0;
}