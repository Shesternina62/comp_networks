#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(){
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    char string[1024];

        while (true) {
        std::cout << "введите сообщение: ";
        std::cin.getline(string, 1024);

        sendto(sockfd,string,strlen(string),0,(struct sockaddr*)&serverAddr,sizeof(serverAddr));
        
        socklen_t addrlen= sizeof(serverAddr);
        char buffer[1024];
        int n = recvfrom(sockfd,buffer,1024,0, (struct sockaddr*)&serverAddr, &addrlen);
        buffer[n] = '\0';
        std::cout << "сервер " << buffer << std::endl;
    }

    close(sockfd);
    return 0;
}
