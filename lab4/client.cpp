#include <iostream>
#include <thread>
#include <atomic>
#include <arpa/inet.h>
#include <unistd.h>
#include "messages.h"

using namespace std;

class TCPClient {
private:
    int sock;
    atomic<bool> running{true};
    string nickname;

public:
    bool connectToServer(const string& ip) {
        sock = socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        return connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    bool auth() {
        sendMessage(sock, MSG_AUTH, nickname);

        Message msg;
        if (recvMessage(sock, msg)) {
            if (msg.type == MSG_WELCOME) {
                cout << msg.payload << endl;
                return true;
            }
            cout << "Error: " << msg.payload << endl;
        }
        return false;
    }

    void receiver() {
        Message msg;

        while (running) {
            if (!recvMessage(sock, msg)) {
                cout << "Disconnected\n";
                exit(0);
            }

            if (msg.type == MSG_TEXT ||
                msg.type == MSG_PRIVATE ||
                msg.type == MSG_SERVER_INFO)
                cout << msg.payload << endl;

            else if (msg.type == MSG_ERROR)
                cout << "[ERROR] " << msg.payload << endl;

            else if (msg.type == MSG_PONG)
                cout << "PONG\n";

            cout << "> ";
        }
    }

    void start(const string& ip) {
        cout << "Nickname: ";
        getline(cin, nickname);

        if (!connectToServer(ip) || !auth()) {
            cout << "Connection failed\n";
            return;
        }

        thread(&TCPClient::receiver, this).detach();

        string input;

        while (running) {
            cout << "> ";
            getline(cin, input);

            if (input == "/quit") {
                sendMessage(sock, MSG_BYE, "");
                break;
            }

            else if (input == "/ping") {
                sendMessage(sock, MSG_PING, "");
            }

            else if (input.rfind("/w ", 0) == 0) {
                auto pos = input.find(' ', 3);
                if (pos == string::npos) continue;

                string nick = input.substr(3, pos - 3);
                string text = input.substr(pos + 1);

                sendMessage(sock, MSG_PRIVATE, nick + ":" + text);
            }

            else {
                sendMessage(sock, MSG_TEXT, input);
            }
        }

        close(sock);
    }
};

int main() {
    TCPClient c;

    string ip = "127.0.0.1";
    cout << "Server IP: ";
    string in;
    getline(cin, in);
    if (!in.empty()) ip = in;

    c.start(ip);
}