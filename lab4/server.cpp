#include <iostream>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include "messages.h"

using namespace std;

struct Client {
    int sock;
    string nickname;
};

class TCPServer {
private:
    int serverSock;
    vector<Client> clients;
    pthread_mutex_t mutexClients = PTHREAD_MUTEX_INITIALIZER;

public:
    bool start() {
        signal(SIGPIPE, SIG_IGN);

        serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock < 0) return false;

        int opt = 1;
        setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0)
            return false;

        if (listen(serverSock, 10) < 0)
            return false;

        cout << "Server started on port " << PORT << endl;
        return true;
    }

    void run() {
        while (true) {
            int clientSock = accept(serverSock, nullptr, nullptr);

            auto* data = new pair<TCPServer*, int>(this, clientSock);

            pthread_t tid;
            pthread_create(&tid, nullptr, handleClientStatic, data);
            pthread_detach(tid);
        }
    }

private:
    static void* handleClientStatic(void* arg) {
        auto* data = (pair<TCPServer*, int>*)arg;

        TCPServer* server = data->first;
        int sock = data->second;

        delete data;
        server->handleClient(sock);
        return nullptr;
    }

    bool isUnique(const string& nick) {
        for (auto& c : clients)
            if (c.nickname == nick)
                return false;
        return true;
    }

    void sendSystem(const string& msg) {
        pthread_mutex_lock(&mutexClients);
        for (auto& c : clients)
            sendMessage(c.sock, MSG_SERVER_INFO, msg);
        pthread_mutex_unlock(&mutexClients);
    }

    void broadcast(const string& msg, int senderSock) {
        pthread_mutex_lock(&mutexClients);
        for (auto& c : clients) {
            if (c.sock != senderSock)
                sendMessage(c.sock, MSG_TEXT, msg);
        }
        pthread_mutex_unlock(&mutexClients);
    }

    void handleClient(int sock) {
        Message msg;

        cout << "[Layer 4 - Transport] recv()\n";
        if (!recvMessage(sock, msg)) {
            close(sock);
            return;
        }

        cout << "[Layer 6 - Presentation] deserialize\n";

        if (msg.type != MSG_AUTH) {
            sendMessage(sock, MSG_ERROR, "Auth required");
            close(sock);
            return;
        }

        string nick = msg.payload;

        cout << "[Layer 5 - Session] auth check\n";

        pthread_mutex_lock(&mutexClients);
        bool ok = isUnique(nick);
        pthread_mutex_unlock(&mutexClients);

        if (nick.empty() || !ok) {
            sendMessage(sock, MSG_ERROR, "Invalid nickname");
            close(sock);
            return;
        }

        pthread_mutex_lock(&mutexClients);
        clients.push_back({sock, nick});
        pthread_mutex_unlock(&mutexClients);

        cout << "[Layer 5 - Session] auth success\n";

        sendMessage(sock, MSG_WELCOME, "Welcome " + nick);

        sendSystem("User [" + nick + "] connected");

        while (true) {
            cout << "[Layer 4 - Transport] recv()\n";

            if (!recvMessage(sock, msg)) break;

            cout << "[Layer 6 - Presentation] deserialize\n";
            cout << "[Layer 7 - Application] handle\n";

            if (msg.type == MSG_TEXT) {
                broadcast("[" + nick + "]: " + string(msg.payload), sock);
            }

            else if (msg.type == MSG_PRIVATE) {
                string data = msg.payload;
                auto pos = data.find(':');

                if (pos == string::npos) {
                    sendMessage(sock, MSG_ERROR, "Format error");
                    continue;
                }

                string target = data.substr(0, pos);
                string text = data.substr(pos + 1);

                bool found = false;

                pthread_mutex_lock(&mutexClients);
                for (auto& c : clients) {
                    if (c.nickname == target) {
                        sendMessage(c.sock, MSG_PRIVATE,
                            "[PRIVATE][" + nick + "]: " + text);
                        found = true;
                        break;
                    }
                }
                pthread_mutex_unlock(&mutexClients);

                if (!found)
                    sendMessage(sock, MSG_ERROR, "User not found");
            }

            else if (msg.type == MSG_PING) {
                sendMessage(sock, MSG_PONG, "");
            }

            else if (msg.type == MSG_BYE) {
                break;
            }
        }

        pthread_mutex_lock(&mutexClients);
        clients.erase(remove_if(clients.begin(), clients.end(),
            [sock](Client& c) { return c.sock == sock; }),
            clients.end());
        pthread_mutex_unlock(&mutexClients);

        sendSystem("User [" + nick + "] disconnected");
        close(sock);
    }
};

int main() {
    TCPServer server;

    if (!server.start()) {
        cerr << "Server failed\n";
        return 1;
    }

    server.run();
}