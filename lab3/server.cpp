#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include "messages.h"

using namespace std;

struct Client {
    int sock;
    string name;
    string ip;
    int port;
};

class TCPServer {
private:
    int serverSock;
    bool running = true;

    queue<int> clientQueue;
    vector<Client> clients;

    pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

    pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;

public:
    bool start() {
        signal(SIGPIPE, SIG_IGN); // игнорировать SIGPIPE

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

        for (int i = 0; i < 10; i++) {
            pthread_t tid;
            pthread_create(&tid, nullptr, worker, this);
            pthread_detach(tid);
        }

        return true;
    }

    void run() {
        while (running) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            int clientSock = accept(serverSock, (sockaddr*)&clientAddr, &addrLen);
            if (clientSock < 0) continue;

            pthread_mutex_lock(&queueMutex);
            clientQueue.push(clientSock);
            pthread_cond_signal(&queueCond);
            pthread_mutex_unlock(&queueMutex);
        }
    }

private:
    static void* worker(void* arg) {
        TCPServer* server = (TCPServer*)arg;

        while (true) {
            int sock;

            pthread_mutex_lock(&server->queueMutex);
            while (server->clientQueue.empty()) {
                pthread_cond_wait(&server->queueCond, &server->queueMutex);
            }
            sock = server->clientQueue.front();
            server->clientQueue.pop();
            pthread_mutex_unlock(&server->queueMutex);

            server->handleClient(sock);
        }
        return nullptr;
    }

    void addClient(const Client& c) {
        pthread_mutex_lock(&clientsMutex);
        clients.push_back(c);
        pthread_mutex_unlock(&clientsMutex);
    }

    void removeClient(int sock) {
        pthread_mutex_lock(&clientsMutex);
        auto it = remove_if(clients.begin(), clients.end(),
            [sock](const Client& c) { return c.sock == sock; });
        clients.erase(it, clients.end());
        pthread_mutex_unlock(&clientsMutex);
    }

    void broadcastText(const string& text, int sender = -1) {
        Message msg;
        msg.type = MSG_TEXT;
        size_t len = text.size();
        if (len > MAX_PAYLOAD - 1) len = MAX_PAYLOAD - 1;
        memcpy(msg.payload, text.c_str(), len);
        msg.payload[len] = '\0';
        msg.length = sizeof(msg.type) + len + 1;

        pthread_mutex_lock(&clientsMutex);
        for (auto& c : clients) {
            if (c.sock != sender) {
                sendAll(c.sock, &msg, sizeof(msg));
            }
        }
        pthread_mutex_unlock(&clientsMutex);
    }

    void handleClient(int sock) {
        Message msg;

        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        getpeername(sock, (sockaddr*)&addr, &addrLen);
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
        int port = ntohs(addr.sin_port);

        if (!recvMessage(sock, msg) || msg.type != MSG_HELLO) {
            close(sock);
            return;
        }

        string name = msg.payload;

        sendMessage(sock, MSG_WELCOME, "Welcome, " + name + "!");
        cout << "Client connected: " << name << " [" << ipStr << ":" << port << "]" << endl;

        addClient({sock, name, ipStr, port});
        broadcastText(name + " joined the chat", sock);

        while (true) {
            if (!recvMessage(sock, msg)) break;

            switch (msg.type) {
                case MSG_TEXT: {
                    string fullMsg = name + ": " + msg.payload;
                    cout << fullMsg << endl;
                    broadcastText(fullMsg, sock);
                    break;
                }

                case MSG_PING:
                    sendMessage(sock, MSG_PONG, "");
                    break;

                case MSG_BYE:
                    broadcastText(name + " left the chat", sock);
                    cout << "Client disconnected: " << name << " [" << ipStr << ":" << port << "]" << endl;
                    removeClient(sock);
                    close(sock);
                    return;
            }
        }

        broadcastText(name + " disconnected", sock);
        cout << "Client disconnected: " << name << " [" << ipStr << ":" << port << "]" << endl;
        removeClient(sock);
        close(sock);
    }
};

int main() {
    TCPServer server;

    if (!server.start()) {
        cerr << "Server start failed\n";
        return 1;
    }

    server.run();
}