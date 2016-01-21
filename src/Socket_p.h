/*
 * This file is part of libArcus
 *
 * Copyright (C) 2015 Ultimaker b.v. <a.hiemstra@ultimaker.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <thread>
#include <mutex>
#include <string>
#include <list>
#include <unordered_map>
#include <deque>
#include <iostream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <signal.h>
#endif

#include <google/protobuf/message.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>

#include "Types.h"
#include "SocketListener.h"
#include "MessageTypeStore.h"
#include "Error.h"

#include "WireMessage_p.h"

#define VERSION_MAJOR 1
#define VERSION_MINOR 0

#define ARCUS_SIGNATURE 0x2BAD
#define SIG(n) (((n) & 0xffff0000) >> 16)

#ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0x0 //Don't request NOSIGNAL on systems where this is not implemented.
#endif

/**
 * Private implementation details for Socket.
 */
namespace Arcus
{
    class SocketPrivate
    {
    public:
        SocketPrivate()
            : state(SocketState::Initial)
            , nextState(SocketState::Initial)
            , port(0)
            , thread(nullptr)
            , lastKeepAliveSent(std::chrono::system_clock::now())
        {
        #ifdef _WIN32
            initializeWSA();
        #endif
        }

        void run();
        sockaddr_in createAddress();
        void sendMessage(MessagePtr message);
        void receiveNextMessage();
        int readInt32(int32_t *dest);
        int readBytes(int size, char* dest);
        void handleMessage(const std::shared_ptr<WireMessage>& wire_message);
        void setSocketReceiveTimeout(int socketId, int timeout);
        void checkConnectionState();

        void error(ErrorCode::ErrorCode error_code, const std::string& message);
        void fatalError(ErrorCode::ErrorCode error_code, const std::string& msg);

        SocketState::SocketState state = SocketState::Initial;
        SocketState::SocketState next_state = SocketState::Initial;

        std::string address;
        int port;

        std::thread* thread;

        std::list<SocketListener*> listeners;

        MessageTypeStore message_types;

        std::shared_ptr<WireMessage> current_message;

        std::deque<MessagePtr> sendQueue;
        std::mutex sendQueueMutex;
        std::deque<MessagePtr> receiveQueue;
        std::mutex receiveQueueMutex;


        int socketId;
        Error last_error;

        std::chrono::system_clock::time_point lastKeepAliveSent;

        static const int keepAliveRate = 500; //Number of milliseconds between sending keepalive packets

    #ifdef _WIN32
        static bool wsaInitialized;
        static void initializeWSA();
    #endif
    };

#ifdef _WIN32
    bool SocketPrivate::wsaInitialized = false;
#endif
    // Report an error that should not cause the connection to abort.
    void Socket::Private::error(ErrorCode::ErrorCode error_code, const std::string& message)
    {
        Error error{error_code, message};
        last_error = error;

        for(auto listener : listeners)
        {
            listener->error(error);
        }
    }

    // Report an error that should cause the socket to go into an error state and abort the connection.
    void Socket::Private::fatalError(ErrorCode::ErrorCode error_code, const std::string& message)
    {
        Error error{error_code, message};
        error.setFatalError(true);
        last_error = error;

        current_message.reset();
        next_state = SocketState::Error;

        for(auto listener : listeners)
        {
            listener->error(error);
        }
    }

    // This is run in a thread.
    void SocketPrivate::run()
    {
        while(state != SocketState::Closed && state != SocketState::Error)
        {
            switch(state)
            {
                case SocketState::Connecting:
                {
                    socketId = ::socket(AF_INET, SOCK_STREAM, 0);
                    sockaddr_in address_data = createAddress();
                    if(::connect(socketId, reinterpret_cast<sockaddr*>(&address_data), sizeof(address_data)))
                    {
                    }
                    else
                    {
                        setSocketReceiveTimeout(socketId, 250);
                        next_state = SocketState::Connected;
                    }
                    break;
                }
                case SocketState::Opening:
                {
                    socketId = ::socket(AF_INET, SOCK_STREAM, 0);
                    sockaddr_in address_data = createAddress();
                    if(::bind(socketId, reinterpret_cast<sockaddr*>(&address_data), sizeof(address_data)))
                    {
                    }
                    else
                    {
                        next_state = SocketState::Listening;
                    }
                    break;
                }
                case SocketState::Listening:
                {
                    ::listen(socketId, 1);

                    int newSocket = ::accept(socketId, 0, 0);
                    if(newSocket == -1)
                    {
                        fatalError(ErrorCode::AcceptFailedError, "Could not accept the incoming connection");
                        next_state = SocketState::Connected;
                    }
                #ifdef _WIN32
                    ::closesocket(socketId);
                #else
                    ::close(socketId);
                #endif
                    socketId = newSocket;
                    setSocketReceiveTimeout(socketId, 250);
                    break;
                }
                case SocketState::Connected:
                {
                    //Get all the messages from the queue and store them in a temporary array so we can
                    //unlock the queue before performing the send.
                    std::list<MessagePtr> messagesToSend;
                    sendQueueMutex.lock();
                    while(sendQueue.size() > 0)
                    {
                        messagesToSend.push_back(sendQueue.front());
                        sendQueue.pop_front();
                    }
                    sendQueueMutex.unlock();

                    for(auto message : messagesToSend)
                    {
                        sendMessage(message);
                    }

                    receiveNextMessage();

                    if(next_state != SocketState::Error)
                    {
                        checkConnectionState();
                    }

                    break;
                }
                case SocketState::Closing:
                {
                #ifdef _WIN32
                    ::closesocket(socketId);
                #else
                    ::close(socketId);
                #endif
                    next_state = SocketState::Closed;
                    break;
                }
                default:
                    break;
            }

            if(next_state != state)
            {
                state = next_state;

                for(auto listener : listeners)
                {
                    listener->stateChanged(state);
                }
            }
        }
    }

    // Create a sockaddr_in structure from the address and port variables.
    sockaddr_in SocketPrivate::createAddress()
    {
        sockaddr_in a;
        a.sin_family = AF_INET;
    #ifdef _WIN32
        InetPton(AF_INET, address.c_str(), &(a.sin_addr)); //Note: Vista and higher only.
    #else
        ::inet_pton(AF_INET, address.c_str(), &(a.sin_addr));
    #endif
        a.sin_port = htons(port);
        return a;
    }

    void SocketPrivate::sendMessage(MessagePtr message)
    {
        //TODO: Improve error handling.
        uint32_t hdr = htonl((ARCUS_SIGNATURE << 16) | (VERSION_MAJOR << 8) | VERSION_MINOR);
        size_t sent_size = ::send(socketId, reinterpret_cast<const char*>(&hdr), 4, MSG_NOSIGNAL);

        int size = htonl(message->ByteSize());
        sent_size = ::send(socketId, reinterpret_cast<const char*>(&size), 4, MSG_NOSIGNAL);

        int type = htonl(message_types.getMessageTypeId(message));
        sent_size = ::send(socketId, reinterpret_cast<const char*>(&type), 4, MSG_NOSIGNAL);

        std::string data = message->SerializeAsString();
        sent_size = ::send(socketId, data.data(), data.size(), MSG_NOSIGNAL);
    }

    void SocketPrivate::receiveNextMessage()
    {
        int result = 0;

        if(!current_message)
        {
            current_message = std::make_shared<WireMessage>();
        }

        if(current_message->getState() == WireMessage::MessageStateHeader)
        {
            int32_t header = 0;
            readInt32(&header);

            if(header == 0) // Keep-alive, just return
                return;

            if (SIG(header) != ARCUS_SIGNATURE)
            {
                // Someone might be speaking to us in a different protocol?
                error(ErrorCode::ReceiveFailedError, "Header mismatch");
                return;
            }

            current_message->setState(WireMessage::MessageStateSize);
        }

        if(current_message->getState() == WireMessage::MessageStateSize)
        {
            int32_t size = 0;
            result = readInt32(&size);
            if(result)
            {
                #ifndef _WIN32
                if (errno == EAGAIN)
                    return;
                #endif

                error(ErrorCode::ReceiveFailedError, "Size invalid");
                return;
            }

            if(size < 0)
            {
                error(ErrorCode::ReceiveFailedError, "Size invalid");
                return;
            }

            current_message->setSize(size);
            current_message->setState(WireMessage::MessageStateType);
        }

        if (current_message->getState() == WireMessage::MessageStateType)
        {
            int32_t type = 0;
            result = readInt32(&type);
            if(result)
            {
                #ifndef _WIN32
                if (errno == EAGAIN)
                    return;
                #endif
                current_message->setValid(false);
            }

            uint32_t real_type = static_cast<uint32_t>(type);

            try
            {
                current_message->allocateData();
            }
            catch (std::bad_alloc& ba)
            {
                // Either way we're in trouble.
                fatalError(ErrorCode::ReceiveFailedError, "Out of memory");
                return;
            }

            current_message->setType(real_type);
            current_message->setState(WireMessage::MessageStateData);
        }

        if (current_message->getState() == WireMessage::MessageStateData)
        {
            result = readBytes(current_message->getRemainingSize(), &current_message->getData()[current_message->getSizeReceived()]);

            if(result == -1)
            {
            #ifndef _WIN32
                if(errno != EAGAIN)
            #endif
                {
                    current_message.reset();
                }
            }
            else
            {
                current_message->setSizeReceived(current_message->getSizeReceived() + result);
                if(current_message->isComplete())
                {
                    if(!current_message->isValid())
                    {
                        current_message.reset();
                        return;
                    }

                    current_message->setState(WireMessage::MessageStateDispatch);
                }
            }
        }

        if (current_message->getState() == WireMessage::MessageStateDispatch)
        {
            handleMessage(current_message);
            current_message.reset();
        }
    }

    int SocketPrivate::readInt32(int32_t *dest)
    {
        int32_t buffer;
        int num = ::recv(socketId, reinterpret_cast<char*>(&buffer), 4, 0);

        if(num != 4)
        {
            return -1;
        }

        *dest = ntohl(buffer);
        return 0;
    }

    int SocketPrivate::readBytes(int size, char* dest)
    {
        int num = ::recv(socketId, dest, size, 0);
        if(num == -1)
        {
            return -1;
        }
        else
        {
            return num;
        }
    }

    void SocketPrivate::handleMessage(const std::shared_ptr<WireMessage>& wire_message)
    {
        if(!message_types.hasType(wire_message->getType()))
        {
            error(ErrorCode::UnknownMessageTypeError, "Unknown message type");
            return;
        }

        MessagePtr message = message_types.createMessage(wire_message->getType());

        google::protobuf::io::ArrayInputStream array(wire_message->getData(), wire_message->getSize());
        google::protobuf::io::CodedInputStream stream(&array);
        stream.SetTotalBytesLimit(500 * 1048576, 128 * 1048576); //Set size limit to 500MiB, warn at 128MiB
        if(!message->ParseFromCodedStream(&stream))
        {
            error(ErrorCode::ParseFailedError, "Failed to parse message");
            return;
        }

        receiveQueueMutex.lock();
        receiveQueue.push_back(message);
        receiveQueueMutex.unlock();

        for(auto listener : listeners)
        {
            listener->messageReceived();
        }
    }

    // Set socket timeout value in milliseconds
    void SocketPrivate::setSocketReceiveTimeout(int socketId, int timeout)
    {
        timeval t;
        t.tv_sec = 0;
        t.tv_usec = timeout * 1000;
        ::setsockopt(socketId, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    }

    // Send a keepalive packet to check whether we are still connected.
    void SocketPrivate::checkConnectionState()
    {
        auto now = std::chrono::system_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeepAliveSent);

        if(diff.count() > keepAliveRate)
        {
            int32_t keepalive = 0;
            if(::send(socketId, reinterpret_cast<const char*>(&keepalive), 4, MSG_NOSIGNAL) == -1)
            {
                error(ErrorCode::ConnectionResetError, "Connection reset by peer");
                next_state = SocketState::Closing;
            }
            lastKeepAliveSent = now;
        }
    }

#ifdef _WIN32
    void SocketPrivate::initializeWSA()
    {
        if(!wsaInitialized)
        {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            wsaInitialized = true;
        }
    }
#endif
}


