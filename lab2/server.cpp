#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "messages.h"

using namespace std;


using ::bind;

class TCPServer {
private:
    int serverSock;
    atomic<bool> running{true};
    
public:
    TCPServer() : serverSock(-1) {}
    
    ~TCPServer() {
        stop();
    }
    
    bool start() {
        serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock < 0) {
            perror("socket");
            return false;
        }
        
        //разрешаем переиспользовать порт
        int opt = 1;
        setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(PORT);
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        
        
        if (::bind(serverSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("bind");
            return false;
        }
        
        if (listen(serverSock, 1) < 0) {
            perror("listen");
            return false;
        }
        
        cout << "Server started on port " << PORT << endl;
        return true;
    }
    
    void run() {
        while (running) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            
            int clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &addrLen);
            if (clientSock < 0) {
                if (running) {
                    perror("accept");
                }
                continue;
            }
            
            //создаем поток для обработки клиента
            thread clientThread(&TCPServer::handleClient, this, clientSock, clientAddr);
            clientThread.detach();
        }
    }
    
    void stop() {
        running = false;
        if (serverSock >= 0) {
            close(serverSock);
            serverSock = -1;
        }
    }
    
private:
    void handleClient(int clientSock, sockaddr_in clientAddr) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        int port = ntohs(clientAddr.sin_port);
        
        cout << "Client connected: " << ip << ":" << port << endl;
        
        Message msg;
        
        
        if (!recvMessage(clientSock, msg) || msg.type != MSG_HELLO) {
            cout << "[" << ip << ":" << port << "]: Failed to receive HELLO" << endl;
            close(clientSock);
            return;
        }
        
        cout << "[" << ip << ":" << port << "]: Hello (" << msg.payload << ")" << endl;
        
        
        string welcomeText = string("Welcome ") + ip + ":" + to_string(port);
        sendMessage(clientSock, MSG_WELCOME, welcomeText);
        
        //основной цикл обработки сообщений
        while (running) {
            if (!recvMessage(clientSock, msg)) {
                cout << "[" << ip << ":" << port << "]: Connection closed" << endl;
                break;
            }
            
            switch (msg.type) {
                case MSG_TEXT:
                    cout << "[" << ip << ":" << port << "]: " << msg.payload << endl;
                    break;
                    
                case MSG_PING:
                    cout << "[" << ip << ":" << port << "]: PING received" << endl;
                    sendMessage(clientSock, MSG_PONG, "");
                    break;
                    
                case MSG_BYE:
                    cout << "[" << ip << ":" << port << "]: Client requested disconnect" << endl;
                    sendMessage(clientSock, MSG_BYE, "Goodbye");
                    close(clientSock);
                    return;
                    
                default:
                    cout << "[" << ip << ":" << port << "]: Unknown message type: " 
                         << (int)msg.type << endl;
            }
        }
        
        close(clientSock);
    }
};

int main() {
    TCPServer server;
    
    if (!server.start()) {
        cerr << "Failed to start server" << endl;
        return 1;
    }
    
    server.run();
    
    return 0;
}