#include <iostream>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "messages.h"

using namespace std;

int sock;

void* receiver(void*) {
    MessageEx msg;

    while (true) {
        if (!recvMessageEx(sock, msg)) {
            cout << "Disconnected from server\n";
            exit(0);
        }

        switch (msg.type) {
            case MSG_TEXT:
                cout << msg.sender << ": " << msg.payload << endl;
                break;

            case MSG_PRIVATE:
                cout << "[PM] " << msg.sender << " -> " << msg.payload << endl;
                break;

            case MSG_SERVER_INFO:
                cout << "[SERVER] " << msg.payload << endl;
                break;

            case MSG_WELCOME:
                cout << msg.payload << endl;
                break;

            case MSG_ERROR:
                cout << "[ERROR] " << msg.payload << endl;
                break;

            case MSG_PONG:
                cout << "[PING OK]" << endl;
                break;

            case MSG_HISTORY_DATA:
                cout << "[HISTORY] " << msg.payload << endl;
                break;

            default:
                cout << "[UNKNOWN MESSAGE]" << endl;
                break;
        }
    }

    return nullptr;
}

void sendText(const string& text) {
    MessageEx msg;
    msg.type = MSG_TEXT;
    strcpy(msg.payload, text.c_str());
    sendMessageEx(sock, msg);
}

void sendPrivate(const string& nick, const string& text) {
    MessageEx msg;
    msg.type = MSG_PRIVATE;
    strcpy(msg.receiver, nick.c_str());
    strcpy(msg.payload, text.c_str());
    sendMessageEx(sock, msg);
}

void sendCommand(MessageType type, const string& payload = "") {
    MessageEx msg;
    msg.type = type;
    strcpy(msg.payload, payload.c_str());
    sendMessageEx(sock, msg);
}

int main() {
    string ip, nickname;

    cout << "Server IP: ";
    cin >> ip;

    cout << "Nickname: ";
    cin >> nickname;
    cin.ignore();

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cerr << "Socket error\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "Connection failed\n";
        return 1;
    }

    MessageEx auth;
    auth.type = MSG_AUTH;
    strcpy(auth.payload, nickname.c_str());
    sendMessageEx(sock, auth);

    pthread_t tid;
    pthread_create(&tid, nullptr, receiver, nullptr);

    cout << "Commands:\n";
    cout << "/pm nick message\n";
    cout << "/list\n";
    cout << "/ping\n";
    cout << "/history N\n";
    cout << "/exit\n";

    string line;

    while (true) {
        getline(cin, line);

        if (line == "/exit") {
            MessageEx bye;
            bye.type = MSG_BYE;
            sendMessageEx(sock, bye);
            break;
        }

        if (line == "/list") {
            sendCommand(MSG_LIST);
            continue;
        }

        if (line == "/ping") {
            sendCommand(MSG_PING);
            continue;
        }

        if (line.rfind("/history", 0) == 0) {
            string arg = line.substr(8);
            sendCommand(MSG_HISTORY, arg);
            continue;
        }

        if (line.rfind("/pm ", 0) == 0) {
            size_t sp = line.find(' ', 4);
            if (sp != string::npos) {
                string nick = line.substr(4, sp - 4);
                string text = line.substr(sp + 1);
                sendPrivate(nick, text);
            }
            continue;
        }

        sendText(line);
    }

    close(sock);
    return 0;
}