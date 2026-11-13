// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <fcntl.h>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <stdexcept>
using namespace std;


// ---- Constants ----
const char* SERVER_PATH = "./server";      // the program we start when the server is down     // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;

    }
    ~Queue()
    {
        while (front != nullptr) {
            Node* n = front;
            front = front->next;
            delete n;
        }
        rear = nullptr;
        count = 0;

    }
    void enqueue(const T& val)
    {
        Node* n = new Node;
        n->data = val;
        n->next = nullptr;
        if (count == 0) {
            front = rear = n;
            count++;

        }
        else {
            rear->next = n;
            rear = n;
            count++;
        }
    }
    T dequeue()
    {
        if (count == 0) {
            throw runtime_error("Queue is empty");
        }
        Node* n = front;
        T val_return = n->data;
        front = front->next;
        if (front == nullptr) {
            rear = nullptr;
        }
        delete n;
        count--;
        return val_return;
    }
    bool isEmpty()
    {
        if (count == 0) {
            return true;
        }
        return false;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    size_t totalBytes_sent = 0;

    while (totalBytes_sent < n){
        ssize_t bytesSent = send(sock,buf + totalBytes_sent,n - totalBytes_sent,MSG_NOSIGNAL
        );

        if (bytesSent == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Send failed");
            return false;
        }

        if (bytesSent == 0){
            return false;
        }

        totalBytes_sent = totalBytes_sent + bytesSent;
    }
    return true;
}
bool sendError(int sock, const string& message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    CRC32 checksum;
    char status = 1;

   
    if (!sendAll(sock, &status, 1)) {
        return false;
    }
   
    checksum.update(&status, 1);

    if (!message.empty()) {

        checksum.update(message.data(), message.size());

        if (!sendAll(sock, message.data(), message.size())) {
            return false;
        }
    }

    uint32_t crcValue = checksum.value();

    char checksumBytes[4];

    checksumBytes[0] = (crcValue >> 24) & 0xFF;
    checksumBytes[1] = (crcValue >> 16) & 0xFF;
    checksumBytes[2] = (crcValue >> 8) & 0xFF;
    checksumBytes[3] = crcValue & 0xFF;

    if (!sendAll(sock, checksumBytes, 4)) {
        return false;
    }

    return true;
}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int listeningSocket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (listeningSocket == -1) {
        perror("Socket creation failed");
        exit(1);
    }


    int opt = 1;

    if (setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR,
        &opt, sizeof(opt)) == -1) {

        perror("setsockopt failed");
        close(listeningSocket);
        exit(1);
    }

    // STEP 3: Prepare server address
    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);
    if (bind(listeningSocket,(struct sockaddr*)&serverAddress,sizeof(serverAddress)) == -1) {

        perror("Binding failed");
        close(listeningSocket);
        exit(1);
    }

    if (listen(listeningSocket, LISTEN_BACKLOG) == -1) {

        perror("Listen failed.Socket is not active");
        close(listeningSocket);
        exit(1);
    }

    return listeningSocket;
}
int connectToServer(const char* host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int serverSide_Sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (serverSide_Sock == -1) {
        perror("Server socket creation failed");
        return -1;
    }
    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);

    int ipResult = inet_pton(AF_INET, host, &serverAddress.sin_addr);

    if (ipResult == 0) {
        cerr << "Invalid server IP address: " << host << endl;
        close(serverSide_Sock);
        return -1;
    }

    if (ipResult == -1) {
        perror("inet_pton failed");
        close(serverSide_Sock);
        return -1;
    }

    if (connect(serverSide_Sock,(struct sockaddr*)&serverAddress,sizeof(serverAddress)) == -1) {

        perror("Connection to backend Server failed");
        close(serverSide_Sock);
        return -1;
    }

    return serverSide_Sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string text_Comm = string(SERVER_PATH) + " " + to_string(port) + " &";

    int system_return = system(text_Comm.c_str());

    if (system_return != 0) {
        cout << "Failed to start backend Server" << endl;
        return false;
    }

    return true;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int serverSide_Sock = connectToServer(serverHost.c_str(), serverPort);

    if (serverSide_Sock != -1) {
        return serverSide_Sock;
    }

    if (!startServer(serverPort)) {
        return -1;
    }

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {

        usleep(STARTUP_POLL_INTERVAL_MS * 1000);

        serverSide_Sock = connectToServer(serverHost.c_str(), serverPort);

        if (serverSide_Sock != -1) {
            cout << "Backend Server connected successfully" << endl;
            return serverSide_Sock;
        }
    }

    cout << "Backend Server failed to start" << endl;

    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo& c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int clientSideSock = c.sock;

    int serverSideSock = connectToServerWithWakeup();

    if (serverSideSock == -1) {

        sendError(clientSideSock, "backend server failed to start");

        close(clientSideSock);

        return;
    }
    char buffer[IO_BUFFER_SIZE];

    TrailerChecker checker;
    vector<char> actualBytes;

    bool uploaded = true;

    while (true){
        ssize_t bytesReceived = recv(clientSideSock,buffer,sizeof(buffer),0);

        if (bytesReceived == 0){
            break;
        }

        if (bytesReceived == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Failed to receive data from Client");
            uploaded = false;
            break;
        }

        checker.feed(buffer, bytesReceived, actualBytes);

        if (!actualBytes.empty()){
            if (!sendAll(serverSideSock, actualBytes.data(), actualBytes.size())){
                cout << "Failed to forward data to Server" << endl;
                uploaded = false;
                break;
            }
        }
    }

    if (uploaded && !checker.verify()){
        cout << "Client upload checksum verification failed" << endl;
        uploaded = false;
    }
    if (uploaded){
        const vector<char>& trailer = checker.getTrailer();

        if (!sendAll(serverSideSock, trailer.data(), trailer.size()))
        {
            cout << "Failed to forward checksum to Server" << endl;
            uploaded = false;
        }
    }
    if (!uploaded){
        close(serverSideSock);
        sendError(clientSideSock, "Client upload failed");
        close(clientSideSock);
        return;
    }

    if (shutdown(serverSideSock, SHUT_WR) == -1){
        perror("Failed to shutdown Server socket");
        close(serverSideSock);
        close(clientSideSock);
        return;
    }
    TrailerChecker responseChkr;
    vector<char> responseBytes;
    char responseBuffer[IO_BUFFER_SIZE];
    bool successfull_Res = true;

    while (true){
        ssize_t bytesReceived = recv(serverSideSock,responseBuffer,sizeof(responseBuffer),0);

        if (bytesReceived == 0){
            break;
        }

        if (bytesReceived == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Failed to receive response from Server");
            successfull_Res = false;
            break;
        }

        responseChkr.feed(responseBuffer,bytesReceived,responseBytes);

        if (!responseBytes.empty()){
            if (!sendAll(clientSideSock,responseBytes.data(),responseBytes.size())){
                cout << "Failed to forward response to Client" << endl;
                successfull_Res = false;
                break;
            }
        }
    }
    if (successfull_Res && !responseChkr.verify()){
        cout << "Server response checksum verification failed" << endl;
        successfull_Res = false;
    }
    if (successfull_Res){
        const vector<char>& trailer = responseChkr.getTrailer();

        if (!sendAll(clientSideSock, trailer.data(), trailer.size())){
            cout << "Failed to forward response checksum to Client" << endl;
            successfull_Res = false;
        }
    }
    close(serverSideSock);
    close(clientSideSock);
}





// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag){
        struct sockaddr_in clientSide_Address {};
        socklen_t clientSide_Length = sizeof(clientSide_Address);
        int clientSide_Sock = accept(listenSock,(struct sockaddr*)&clientSide_Address,&clientSide_Length);

        if (clientSide_Sock == -1){
            if (errno == EINTR)
                continue;

            if (!flag)
                break;

            perror("Failed to accept Client");
            continue;
        }
        int fdFlags = fcntl(clientSide_Sock, F_GETFD);

        if (fdFlags == -1 ||
            fcntl(clientSide_Sock, F_SETFD, fdFlags | FD_CLOEXEC) == -1) {

            perror("Failed to set close-on-exec for Client socket");
            close(clientSide_Sock);
            continue;
        }

        char clientSide_IP[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &clientSide_Address.sin_addr,
            clientSide_IP, sizeof(clientSide_IP)) == nullptr){
            perror("Failed to convert Client IP");
            close(clientSide_Sock);
            continue;
        }

        int clientPort = ntohs(clientSide_Address.sin_port);

        ClientInfo client;

        client.sock = clientSide_Sock;
        client.address = clientSide_IP;
        client.port = clientPort;
        client.arrivedAt = time(nullptr);


        queueMutex.lock();

        if (pendingClients.size() >= MAX_PENDING_CLIENTS){
            queueMutex.unlock();

            sendError(clientSide_Sock, "Failed to accept any other connection as server is busy, try again later");
            close(clientSide_Sock);

            continue;
        }

        pendingClients.enqueue(client);
        cout << "Client queued: "<< client.address << ": "<< client.port << endl;
        queueMutex.unlock();
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (true){
        ClientInfo client;
        bool client_Present = false;

        queueMutex.lock();

        if (!pendingClients.isEmpty()){
            client = pendingClients.dequeue();
            client_Present = true;
        }

        queueMutex.unlock();

        if (!client_Present){
            if (!flag){
                break;
            }
            usleep(10000);
            continue;
        }
        time_t currentTime = time(nullptr);

        double waitingTime = difftime(currentTime,client.arrivedAt);

        if (waitingTime > 30){
            cout << "Client timed out: "<< client.address << endl;

            close(client.sock);
            continue;
        }
        relayOneClient(client);
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char* argv[])
{
    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    if (argc > 1){
        lbPort = atoi(argv[1]);
    }

    int listeningSock = createListenSocket(lbPort);

    cout << "Load Balancer listening on port "<< lbPort << endl;

    thread accepptorThread(acceptLoop, listeningSock);
    
    dispatcherLoop();
    shutdown(listeningSock, SHUT_RDWR);
    close(listeningSock);
    accepptorThread.join();

    cout << "Load Balancer shutting down successfully" << endl;
    
    return 0;
}