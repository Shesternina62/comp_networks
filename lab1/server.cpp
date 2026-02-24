#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr, clientAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

    char buffer[1024];
    socklen_t addrLen = sizeof(clientAddr);

    while (true) {
        int n = recvfrom(sockfd, buffer, 1024, 0,(struct sockaddr*)&clientAddr, &addrLen);

        buffer[n] = '\0';

        char IP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, IP, sizeof(IP));
        int port = ntohs(clientAddr.sin_port);

        std::cout << "клиент" << IP << ":" << port << " -> " << buffer << std::endl;

        sendto(sockfd, buffer, n, 0,(struct sockaddr*)&clientAddr, addrLen);
    }

    close(sockfd);
    return 0;
}