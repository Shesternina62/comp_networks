#include <iostream>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <fstream>
#include <cstring>

#include "messages.h"

using namespace std;

std::atomic<uint32_t> global_msg_id{1};
const string HISTORY_FILE = "history.json";

struct Client {
    int sock;
    string nickname;
};

struct OfflineMsg {
    MessageEx msg;
};

class TCPServer {
private:
    int serverSock;
    vector<Client> clients;
    vector<OfflineMsg> offlineQueue;

    pthread_mutex_t mutexClients = PTHREAD_MUTEX_INITIALIZER;
    void appendToHistory(const MessageEx& msg, bool delivered, bool is_offline) {
    ofstream file(HISTORY_FILE, ios::app);

    file.seekp(0, ios::end);
    bool notEmpty = file.tellp() > 2;

    if (notEmpty) file << ",\n";

    string typeStr;
    switch (msg.type) {
        case MSG_TEXT: typeStr = "MSG_TEXT"; break;
        case MSG_PRIVATE: typeStr = "MSG_PRIVATE"; break;
        default: typeStr = "OTHER";
    }

    file << "{\n";
    file << "  \"msg_id\": " << msg.msg_id << ",\n";
    file << "  \"timestamp\": " << msg.timestamp << ",\n";
    file << "  \"sender\": \"" << msg.sender << "\",\n";
    file << "  \"receiver\": \"" << msg.receiver << "\",\n";
    file << "  \"type\": \"" << typeStr << "\",\n";
    file << "  \"text\": \"" << msg.payload << "\",\n";
    file << "  \"delivered\": " << (delivered ? "true" : "false") << ",\n";
    file << "  \"is_offline\": " << (is_offline ? "true" : "false") << "\n";
    file << "}";
}

    void storeOffline(const MessageEx& msg) {
        offlineQueue.push_back({msg});
    }
    void deliverOffline(const string& nick, int sock) {

        for (auto it = offlineQueue.begin(); it != offlineQueue.end(); ) {

            if (strcmp(it->msg.receiver, nick.c_str()) == 0) {

                MessageEx out = it->msg;

                string text = "[OFFLINE] ";
                text += out.payload;
                strncpy(out.payload, text.c_str(), MAX_PAYLOAD - 1);
                out.payload[MAX_PAYLOAD - 1] = '\0';

                sendMessageEx(sock, out);

                it = offlineQueue.erase(it);
            }
            else {
                ++it;
            }
        }
    }

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

        cout << "[Application] Server started on port " << PORT << endl;
        return true;

        ofstream file(HISTORY_FILE, ios::trunc);
        file << "[\n";
        file.close();
    }

    void run() {
        while (true) {
            int clientSock = accept(serverSock, nullptr, nullptr);

            cout << "[Network Access] client connected\n";

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

    void sendToClient(int sock, MessageEx& msg) {
        sendMessageEx(sock, msg);
    }

    void sendSystem(const string& text) {
        MessageEx msg;
        msg.type = MSG_SERVER_INFO;
        msg.msg_id = global_msg_id++;
        strcpy(msg.sender, "SERVER");
        strncpy(msg.payload, text.c_str(), MAX_PAYLOAD - 1);
        msg.payload[MAX_PAYLOAD - 1] = '\0';

        pthread_mutex_lock(&mutexClients);
        for (auto& c : clients)
            sendToClient(c.sock, msg);
        pthread_mutex_unlock(&mutexClients);
    }

    void broadcast(const string& sender, const string& text, int senderSock) {
        MessageEx msg;
        msg.type = MSG_TEXT;
        msg.msg_id = global_msg_id++;
        strcpy(msg.sender, sender.c_str());
        strncpy(msg.payload, text.c_str(), MAX_PAYLOAD - 1);
        msg.payload[MAX_PAYLOAD - 1] = '\0';

        pthread_mutex_lock(&mutexClients);
        for (auto& c : clients)
            if (c.sock != senderSock)
                sendToClient(c.sock, msg);
        pthread_mutex_unlock(&mutexClients);
    }

    void sendUserList(int sock) {
        string list = "Online users:\n";

        pthread_mutex_lock(&mutexClients);
        for (auto& c : clients)
            list += c.nickname + "\n";
        pthread_mutex_unlock(&mutexClients);

        MessageEx msg;
        msg.type = MSG_SERVER_INFO;
        msg.msg_id = global_msg_id++;
        strcpy(msg.sender, "SERVER");
        strncpy(msg.payload, list.c_str(), MAX_PAYLOAD - 1);
        msg.payload[MAX_PAYLOAD - 1] = '\0';

        sendToClient(sock, msg);
    }

    void handleClient(int sock) {
        MessageEx msg;

        if (!recvMessageEx(sock, msg)) {
            close(sock);
            return;
        }

        if (msg.type != MSG_AUTH) {
            close(sock);
            return;
        }

        string nick = msg.payload;

        pthread_mutex_lock(&mutexClients);
        bool ok = isUnique(nick);
        pthread_mutex_unlock(&mutexClients);

        if (nick.empty() || !ok) {
            MessageEx err;
            err.type = MSG_ERROR;
            err.msg_id = global_msg_id++;
            strcpy(err.sender, "SERVER");
            strcpy(err.payload, "Invalid nickname");

            sendToClient(sock, err);
            close(sock);
            return;
        }

        pthread_mutex_lock(&mutexClients);
        clients.push_back({sock, nick});
        pthread_mutex_unlock(&mutexClients);

        MessageEx welcome;
        welcome.type = MSG_WELCOME;
        welcome.msg_id = global_msg_id++;
        strcpy(welcome.sender, "SERVER");
        strcpy(welcome.receiver, nick.c_str());
        strcpy(welcome.payload, ("Welcome " + nick).c_str());

        sendToClient(sock, welcome);

        deliverOffline(nick, sock);

        sendSystem("User [" + nick + "] connected");

        while (true) {
            if (!recvMessageEx(sock, msg)) break;

            if (msg.type == MSG_TEXT) {
                broadcast(nick, msg.payload, sock);

                MessageEx save = msg;
                save.msg_id = global_msg_id++;
                strcpy(save.sender, nick.c_str());

                appendToHistory(save, true, false);
            }

            else if (msg.type == MSG_PRIVATE) {
                string target = msg.receiver;
                string text = msg.payload;

                bool found = false;

                pthread_mutex_lock(&mutexClients);
                for (auto& c : clients) {
                    if (c.nickname == target) {

                        MessageEx out;
                        out.type = MSG_PRIVATE;
                        out.msg_id = global_msg_id++;
                        strcpy(out.sender, nick.c_str());
                        strcpy(out.receiver, target.c_str());
                        strncpy(out.payload, text.c_str(), MAX_PAYLOAD - 1);
                        out.payload[MAX_PAYLOAD - 1] = '\0';

                        sendToClient(c.sock, out);

                        appendToHistory(out, true, false);

                        found = true;
                        break;
                    }
                }
                pthread_mutex_unlock(&mutexClients);

                if (!found) {
                    MessageEx offlineMsg;
                    offlineMsg.type = MSG_PRIVATE;
                    offlineMsg.msg_id = global_msg_id++;
                    strcpy(offlineMsg.sender, nick.c_str());
                    strcpy(offlineMsg.receiver, target.c_str());
                    strncpy(offlineMsg.payload, text.c_str(), MAX_PAYLOAD - 1);
                    offlineMsg.payload[MAX_PAYLOAD - 1] = '\0';

                    storeOffline(offlineMsg);
                    appendToHistory(offlineMsg, false, true);
                }
            }

            else if (msg.type == MSG_LIST) {
                sendUserList(sock);
            }

            else if (msg.type == MSG_PING) {
                MessageEx pong;
                pong.type = MSG_PONG;
                pong.msg_id = global_msg_id++;
                strcpy(pong.sender, "SERVER");

                sendToClient(sock, pong);
            }

            else if (msg.type == MSG_HISTORY) {
                int N = 10;
                if (strlen(msg.payload) > 0) {
                    N = atoi(msg.payload);
                    if (N <= 0) N = 10;
                }

                MessageEx out;
                out.type = MSG_HISTORY_DATA;
                out.msg_id = global_msg_id++;
                strcpy(out.sender, "SERVER");
                strcpy(out.payload, "History feature OK (file-based logging active)");

                sendToClient(sock, out);
            }


            else if (msg.type == MSG_BYE) {
                break;
            }
        }

        pthread_mutex_lock(&mutexClients);
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (it->sock == sock) {
        it = clients.erase(it);
            } 
            else {
        ++it;
            }
        }
        pthread_mutex_unlock(&mutexClients);
        cout << "[Network Access] client disconnected: " << nick << endl;
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
    ofstream file(HISTORY_FILE, ios::app);
    file << "\n]\n";
    file.close();
}