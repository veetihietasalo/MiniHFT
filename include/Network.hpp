#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <iostream>
#include <vector>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

class TcpServer {
private:
    SOCKET listenSocket;
    int port;

public:
    TcpServer(int p) : port(p), listenSocket(INVALID_SOCKET) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    ~TcpServer() {
        closesocket(listenSocket);
        WSACleanup();
    }

    void start() {
        struct addrinfo *result = NULL, hints;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        std::string portStr = std::to_string(port);
        getaddrinfo(NULL, portStr.c_str(), &hints, &result);

        listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
        freeaddrinfo(result);

        listen(listenSocket, SOMAXCONN);
        std::cout << "Server listening on port " << port << "...\n";

        while (true) {
            SOCKET clientSocket = accept(listenSocket, NULL, NULL);
            if (clientSocket != INVALID_SOCKET) {
                std::cout << "Client connected!\n";
                std::thread(&TcpServer::handleClient, this, clientSocket).detach();
            }
        }
    }

    void handleClient(SOCKET clientSocket) {
        char buffer[512];
        int res;
        do {
            res = recv(clientSocket, buffer, 512, 0);
            if (res > 0) {
                std::cout << "Received " << res << " bytes: " << std::string(buffer, res) << "\n";
                // Echo back
                send(clientSocket, buffer, res, 0);
            }
        } while (res > 0);
        closesocket(clientSocket);
        std::cout << "Client disconnected.\n";
    }
};

class TcpClient {
private:
    SOCKET connectSocket;
    std::string ip;
    int port;

public:
    TcpClient(std::string i, int p) : ip(i), port(p), connectSocket(INVALID_SOCKET) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    ~TcpClient() {
        closesocket(connectSocket);
        WSACleanup();
    }

    bool connectToServer() {
        struct addrinfo *result = NULL, *ptr = NULL, hints;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string portStr = std::to_string(port);
        getaddrinfo(ip.c_str(), portStr.c_str(), &hints, &result);

        for(ptr=result; ptr != NULL ;ptr=ptr->ai_next) {
            connectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (connectSocket == INVALID_SOCKET) continue;

            if (connect(connectSocket, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
                closesocket(connectSocket);
                connectSocket = INVALID_SOCKET;
                continue;
            }
            break;
        }
        freeaddrinfo(result);

        return (connectSocket != INVALID_SOCKET);
    }

    void sendMsg(const std::string& msg) {
        send(connectSocket, msg.c_str(), (int)msg.length(), 0);
    }
};
