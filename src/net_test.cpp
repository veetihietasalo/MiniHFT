#include <iostream>
#include <thread>
#include <chrono>
#include "Network.hpp"

void runServer() {
    TcpServer server(8080);
    server.start();
}

int main() {
    std::cout << "Starting Network Test...\n";

    // Start Server in a separate thread
    std::thread serverThread(runServer);
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Give server time to start

    // Start Client
    TcpClient client("127.0.0.1", 8080);
    if (client.connectToServer()) {
        std::cout << "Connected to server!\n";
        client.sendMsg("Hello HFT World!");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } else {
        std::cout << "Failed to connect.\n";
    }

    // Force exit since server loop is infinite
    std::cout << "Test Complete. Exiting.\n";
    exit(0);
}
