#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include "messages.h"

using namespace std;

class TCPClient {
private:
    int sockfd;
    atomic<bool> running{true};
    thread receiverThread;
    
public:
    TCPClient() : sockfd(-1) {}
    
    ~TCPClient() {
        disconnect();
    }
    
    bool connectToServer(const string& ip, int port) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket");
            return false;
        }
        
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
            cerr << "Invalid address" << endl;
            return false;
        }
        
        if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            perror("connect");
            return false;
        }
        
        cout << "Connected" << endl;
        return true;
    }
    
    bool handshake() {
        string nick;
        cout << "Enter nick: ";
        getline(cin, nick);
        
        if (!sendMessage(sockfd, MSG_HELLO, nick)) {
            cerr << "Failed to send HELLO" << endl;
            return false;
        }
        
        Message msg;
        if (!recvMessage(sockfd, msg) || msg.type != MSG_WELCOME) {
            cerr << "Failed to receive WELCOME" << endl;
            return false;
        }
        
        cout << msg.payload << endl;
        return true;
    }
    
    void start() {
        //запускаем поток для приема сообщений
        receiverThread = thread(&TCPClient::receiveMessages, this);
        
        //основной цикл ввода
        string input;
        
        while (running) {
            cout << "> ";
            getline(cin, input);
            
            if (!running) break;
            
            if (input == "/quit") {
                sendMessage(sockfd, MSG_BYE, "");
                running = false;
                break;
            }
            else if (input == "/ping") {
                sendMessage(sockfd, MSG_PING, "");
            }
            else {
                sendMessage(sockfd, MSG_TEXT, input);
            }
        }
        
        if (receiverThread.joinable()) {
            receiverThread.join();
        }
    }
    
    void disconnect() {
        running = false;
        if (sockfd >= 0) {
            close(sockfd);
            sockfd = -1;
        }
    }
    
private:
    void receiveMessages() {
        Message msg;
        
        while (running) {
            //используем select для таймаута, чтобы проверять running
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100 мс
            
            int activity = select(sockfd + 1, &readfds, NULL, NULL, &tv);
            
            if (activity < 0) {
                if (running) {
                    cerr << "Select error" << endl;
                }
                break;
            }
            
            if (activity > 0 && FD_ISSET(sockfd, &readfds)) {
                if (!recvMessage(sockfd, msg)) {
                    if (running) {
                        cout << "\nServer disconnected" << endl;
                    }
                    running = false;
                    break;
                }
                
                switch (msg.type) {
                    case MSG_TEXT:
                        cout << "\nServer: " << msg.payload << endl << "> " << flush;
                        break;
                        
                    case MSG_PONG:
                        cout << "\nPONG" << endl << "> " << flush;
                        break;
                        
                    case MSG_BYE:
                        cout << "\nServer closed connection" << endl;
                        running = false;
                        break;
                        
                    default:
                        cout << "\nUnknown message type: " << (int)msg.type << endl << "> " << flush;
                }
            }
        }
    }
};

int main() {
    TCPClient client;
    
    string server_ip = "127.0.0.1";
    cout << "Enter server IP (default: 127.0.0.1): ";
    string input;
    getline(cin, input);
    if (!input.empty()) {
        server_ip = input;
    }
    
    if (!client.connectToServer(server_ip, PORT)) {
        cerr << "Failed to connect to server" << endl;
        return 1;
    }
    
    if (!client.handshake()) {
        cerr << "Handshake failed" << endl;
        return 1;
    }
    
    client.start();
    
    cout << "Disconnected" << endl;
    return 0;
}