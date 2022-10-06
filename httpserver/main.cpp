#include <iostream>
#include <memory>

#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#include <pthread.h>
#include <vector>
#include <cassert>
#include <cstring>

/* Maximum number of server connections, this indicates the socket listen/accept queue size */
constexpr uint MAX_SERVER_CONNECTION = 10;

constexpr uint WORKERS_COUNT = 2;

/* If client is connected for 3 minutes without any activity, remove him from the server */
constexpr float CLIENT_EXPIRED_TIME = 1. * 60. * 3;

class HTTPServer
{
public:
    HTTPServer()
    {
        FD_ZERO(&mClientsSet);
    }
    ~HTTPServer();
    void openConnection();
    void activate() const;
    void closeConnection();
    void runWorkers();
    void connectClient(const struct pollfd *pFd);
    void disconnectClient(std::int32_t clientPos, std::int32_t clientSocket);
    void waitClients();

    [[nodiscard]] auto isServerRunning() const
    {
        return mIsServerRunning;
    }

    [[nodiscard]] auto getServerFd() const
    {
        return mServerFd;
    }

    /* Returns the count of clients already connected into the server */
    [[nodiscard]] auto getClientsCount()
    {
        pthread_mutex_lock(&mClientMutex);

        const auto clientsCount = mServerClients.size();

        /* std::printf("Server clients count (%lu)\n", clientsCount); */

        pthread_mutex_unlock(&mClientMutex);

        return clientsCount;
    }

    [[nodiscard]] auto existClients()
    {
        return getClientsCount() != 0;
    }

private:
    std::int32_t mServerFd = -1;
    struct sockaddr_in mServerSocket {
            .sin_family = AF_INET,
            .sin_port = htons(80),
            .sin_zero = {}
    };
    bool mIsServerRunning = false;

    /* Condition used for sleep the 'wait for clients' thread */
    pthread_cond_t mWaitClientsCond = PTHREAD_COND_INITIALIZER;

    /* Client information */
    class ClientNet
    {
    public:
        ClientNet()
        {
            std::printf("New client created with clock, %lu\n", mMeasuringTime);
        }
        ~ClientNet()
        {
            if (mClientFd)
            {
                close(mClientFd);
            }
        }

        void resetWaitTime()
        {
            mMeasuringTime = clock();
        }

        void configureFd(std::int32_t socket)
        {
            mClientFd = socket;
        }

        [[nodiscard]] auto getClientFd() const
        {
            return mClientFd;
        }

        void loadSignals(const struct pollfd *pFd)
        {
            std::memcpy(&mClientSignals, pFd, sizeof(struct pollfd));
        }

        /* Checks whether the client is alive, or is inactive */
        [[nodiscard]] auto isClientAlive() const
        {
            return mClientFd != 0 && static_cast<double>((clock() - mMeasuringTime) / CLOCKS_PER_SEC) < CLIENT_EXPIRED_TIME;
        }

    private:
        /* Received from a recent call to accept */
        std::int32_t mClientFd{};
        struct pollfd mClientSignals{};
        /* Measuring the time that's client is alive into the server */
        time_t mMeasuringTime{};
    };

    pthread_mutex_t mClientMutex = PTHREAD_MUTEX_INITIALIZER;
    /* Every connected client will be reserved inside this vector */
    std::vector<std::shared_ptr<ClientNet>> mServerClients;

    /* Client sets, with this we can look up all client information and handler when the asker the server
     * for some response
    */
    fd_set mClientsSet{};

};

namespace WorkersActivities
{
    /* Threat poll from server socket fd */
    bool handlerServerLockResult(const int pollRet, HTTPServer *mainServer)
    {
        if (pollRet == 0)
        {
            /* If there's clients alive, doesn't kill the server */
            std::printf("Server is timeout, checking if there's clients alive");
            if (mainServer->existClients() == false)
            {
                std::printf("The server will be closed, no other clients is alive");
                /* Time out, closing the server */
                raise(SIGINT);
            }
            return false;
        }
        else if (pollRet == -1)
        {
            throw std::runtime_error("Poll wasn't worked, something is wrong");
        }
        return true;
    }
}

namespace ServerWorkers
{
    /* This thread will receive all incoming connections to the server */
    [[maybe_unused]] void* connectionWorker([[maybe_unused]] void* userData)
    {
        auto mainServer = reinterpret_cast<HTTPServer*>(userData);
        const auto serverFd = mainServer->getServerFd();
        /* Waiting until there's data to read in server socket fd */
        struct pollfd serverPoll = {.fd = serverFd, .events = POLLIN};

        while (mainServer->isServerRunning())
        {
            std::puts("Waiting for connections");
            /* Waiting until a new connection is attempted */
            /* Waiting for a new connections on the server socket fd, if 2 minutes is passed
             * and no connection is received, the server is closed, otherwise,
             * the time is restored
             * */
            const auto pollRet = poll(&serverPoll, 1, 1000 * 60 * 2);
            if (WorkersActivities::handlerServerLockResult(pollRet, mainServer) == false)
            {
                /* There's no clients, but the server can't stop now... */
                continue;
            }
            /* Accept the new client */
            mainServer->connectClient(&serverPoll);
        }

        return nullptr;
    }
    /* This threads will handler all information, like requests */
    [[maybe_unused]] void* handlerWorker([[maybe_unused]] void* userData)
    {
        auto mainServer = reinterpret_cast<HTTPServer*>(userData);
        while (mainServer->isServerRunning())
        {
            std::puts("Waiting for handler some new client");
            mainServer->waitClients();
        }

        return nullptr;
    }
}

HTTPServer::~HTTPServer()
{
    if (mIsServerRunning)
    {
        closeConnection();
    }
}

void HTTPServer::openConnection()
{
    /* Opening an end-point connection */
    mServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mServerFd == -1)
    {
        throw std::runtime_error("Can't open a socket connection");
    }
    /* Assigning the server socket with his properties */
    const std::int32_t bindRet = bind(mServerFd, reinterpret_cast<sockaddr*>(&mServerSocket),
                                      sizeof(mServerSocket));
    if (bindRet == -1)
    {
        throw std::runtime_error("Can't assign the recently created socket");
    }

    std::puts("The server is running");
    mIsServerRunning = true;
}

void HTTPServer::closeConnection()
{
    std::puts("The server will be closed");
    close(mServerFd);
    mServerFd = -1;
    mIsServerRunning = false;
}

void HTTPServer::runWorkers()
{
    constexpr uint INCOMING_WORKER = 0;
    constexpr uint HANDLER_WORKER = 1;

    pthread_t workerThreads[WORKERS_COUNT];

    /* Spawning two threads for handler connections and request/send information */
    pthread_create(&workerThreads[INCOMING_WORKER], nullptr, ServerWorkers::connectionWorker, this);
    pthread_create(&workerThreads[HANDLER_WORKER], nullptr, ServerWorkers::handlerWorker, this);


}

void HTTPServer::activate() const
{
    const auto listenRet = listen(mServerFd, MAX_SERVER_CONNECTION);
    if (listenRet == -1)
    {
        throw std::runtime_error("Can't put the server in listen mode");
    }
}

/* Accept a new client into the server */
void HTTPServer::connectClient(const struct pollfd *pFd)
{
    std::puts("A new client is trying to connect to the server");
    assert(pFd->revents & POLLIN);

    /* Creating a new client and connecting him to the server */
    auto newClient = std::make_shared<HTTPServer::ClientNet>();

    struct sockaddr userClientSockInfo{};
    socklen_t userInfoLen = sizeof(userClientSockInfo);

    /* Accepting client communication */
    const auto acceptConn = accept(mServerFd, &userClientSockInfo, &userInfoLen);

    if (acceptConn == -1)
    {
        std::printf("The server can't open a communication with the client because, %s\n",
                    strerror(errno));
        /* newClient will be destroyed, before return occurs */
        return;
    }

    std::puts("Server now was opened an end-point communication with the client");

    /* Configuring client information */
    newClient->configureFd(acceptConn);
    newClient->loadSignals(pFd);

    /* Acquiring the client mutex, so on we can add the new client into */
    pthread_mutex_lock(&mClientMutex);

    mServerClients.push_back(newClient);
    std::puts("Client has been added");

    /* Updating clients FD list */
    FD_SET(newClient->getClientFd(), &mClientsSet);

    newClient->resetWaitTime();

    std::puts("New client added");

    pthread_cond_signal(&mWaitClientsCond);
    pthread_mutex_unlock(&mClientMutex);

}

void HTTPServer::waitClients()
{
    /* If there's no clients, then does nothing, returns */
    if (existClients() == false)
    {
        std::puts("Waiting for some user begin found");
        pthread_mutex_lock(&mClientMutex);
        /* When no client is found, sleep while there isn't */
        pthread_cond_wait(&mWaitClientsCond, &mClientMutex);
    }
    /* Wait until a non-blocking read operation is permitted into the server, unless wait for 3 minutes,
     * All clients that does nothing, need to be dropped from the server
    */
    const auto lastUsedOpenedFd = mServerClients.back()->getClientFd();

    /* TODO: Fix this, carriage return isn't working */
    /*
    std::printf("The last client FD is (%d), using %d for select operation, "
                "waiting for all clients (%lu) inside 3 minutes\r", lastUsedOpenedFd,
                lastUsedOpenedFd + 1, mServerClients.size());
    */
    pthread_mutex_unlock(&mClientMutex);

    static struct timeval waitSelect = { .tv_sec = 60 * 3, .tv_usec = 0 };

    fd_set readSet = mClientsSet;
    fd_set writeSet = mClientsSet;

    const auto selectRet = select(lastUsedOpenedFd + 1, &readSet, &writeSet, nullptr,
                                  &waitSelect);

    if (selectRet == 0)
    {
        std::puts("Nobody is opened for write or read operations, then, the server will turn off");
        closeConnection();
    }

    std::int32_t clientsIndex = 0;

    /* Searching for the client that has ready for read/write */
    for (const auto& client : mServerClients)
    {
        if (client->isClientAlive() == false)
        {
            disconnectClient(clientsIndex, client->getClientFd());
            continue;
        }

        if (FD_ISSET(client->getClientFd(), &readSet))
        {
            std::printf("Client %d is ready for reading\n", client->getClientFd());
            /* Client is ready for read */
            client->resetWaitTime();
        }

        if (FD_ISSET(client->getClientFd(), &writeSet))
        {
            std::printf("Client %d is ready for writing\n", client->getClientFd());
            /* The client is ready for writing */
            client->resetWaitTime();
        }

        clientsIndex++;
    }

}

void HTTPServer::disconnectClient(std::int32_t clientPos, std::int32_t clientSocket)
{
    pthread_mutex_lock(&mClientMutex);

    assert(mServerClients[clientPos]->getClientFd() == clientSocket);

    mServerClients.erase(std::remove_if(mServerClients.begin(), mServerClients.end(),
                         [clientPos](const std::shared_ptr<HTTPServer::ClientNet>& client){
        return client->getClientFd() == clientPos;
    }), mServerClients.end());

    pthread_mutex_unlock(&mClientMutex);

}

std::shared_ptr<HTTPServer> gServer;

static pthread_cond_t gMainThreadSleep = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gMainThreadMutex = PTHREAD_MUTEX_INITIALIZER;

void stopServerSignal([[maybe_unused]] int unused) {
    std::puts("\nThe server will been closed, all resources will be freed");
    /* Closing server connection and deallocating used resources */
    if (gServer != nullptr)
    {
        gServer->closeConnection();
    }

    /* Advise the main thread */
    pthread_cond_signal(&gMainThreadSleep);

}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"
int main()
{
    pthread_mutex_lock(&gMainThreadMutex);

    gServer = std::make_shared<HTTPServer>();
    /* Opening server connection end point */
    gServer->openConnection();

    /* Putting the server in listen mode */
    gServer->activate();
    /* Starting server routines */
    gServer->runWorkers();

    /* Registering server stop signal into the process signal handler */
    /* struct sigaction signal = {.sa_handler = stopServerSignal};
     * sigaction(SIGINT, &signal, nullptr);
    */
    signal(SIGINT, stopServerSignal);

    pthread_cond_wait(&gMainThreadSleep, &gMainThreadMutex);

    pthread_mutex_unlock(&gMainThreadMutex);

    return 0;

}
#pragma clang diagnostic pop
