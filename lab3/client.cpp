#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>
#include "messages.h"

using namespace std;

class TCPClient {
private:
    int sockfd;
    atomic<bool> running{true};
    string server_ip;
    string nickname;

public:
    bool connectToServer() {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr);

        return connect(sockfd, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    bool handshake() {
        sendMessage(sockfd, MSG_HELLO, nickname);

        Message msg;
        if (recvMessage(sockfd, msg) && msg.type == MSG_WELCOME) {
            cout << msg.payload << endl;
            return true;
        }
        return false;
    }

    void receiveLoop() {
        Message msg;

        while (running) {
            if (!recvMessage(sockfd, msg)) {
                cout << "\nDisconnected from server\n";
                reconnect();
                return;
            }

            switch (msg.type) {
                case MSG_TEXT:
                    cout << msg.payload << endl;
                    cout << "> "; // возвращаем уголки
                    break;

                case MSG_PONG:
                    cout << "PONG" << endl;
                    cout << "> ";
                    break;

                default:
                    break;
            }
        }
    }

    void reconnect() {
        close(sockfd);

        while (true) {
            cout << "Reconnecting..." << endl;
            this_thread::sleep_for(chrono::seconds(2));

            if (connectToServer() && handshake()) {
                cout << "Reconnected!" << endl;
                thread(&TCPClient::receiveLoop, this).detach();
                return;
            }
        }
    }

    void start(string ip) {
        server_ip = ip;

        cout << "Enter nickname: ";
        getline(cin, nickname);

        if (!connectToServer() || !handshake()) {
            reconnect();
        }

        thread(&TCPClient::receiveLoop, this).detach();

        string input;
        while (running) {
            cout << "> ";
            getline(cin, input);

            if (input == "/quit") {
                sendMessage(sockfd, MSG_BYE, "");
                running = false;
                break;
            } else if (input == "/ping") {
                sendMessage(sockfd, MSG_PING, "");
            } else if (!input.empty()) {
                sendMessage(sockfd, MSG_TEXT, input);
            }
        }

        close(sockfd);
    }
};

int main() {
    TCPClient client;

    string ip = "127.0.0.1";
    cout << "Server IP (default 127.0.0.1): ";
    string input;
    getline(cin, input);
    if (!input.empty()) ip = input;

    client.start(ip);
}