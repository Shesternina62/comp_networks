CXX = g++
all: server client

server: server.cpp messages.h
		$(CXX) server.cpp -o server 
client: client.cpp messages.h
		$(CXX) client.cpp -o client 

clean:
	rm -f server client