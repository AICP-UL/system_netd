/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "NetdClient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include "Fwmark.h"
#include "FwmarkClient.h"
#include "FwmarkCommand.h"
#include "resolv_netid.h"
#include "Stopwatch.h"

namespace {

std::atomic_uint netIdForProcess(NETID_UNSET);
std::atomic_uint netIdForResolv(NETID_UNSET);

typedef int (*Accept4FunctionType)(int, sockaddr*, socklen_t*, int);
typedef int (*ConnectFunctionType)(int, const sockaddr*, socklen_t);
typedef int (*SocketFunctionType)(int, int, int);
typedef unsigned (*NetIdForResolvFunctionType)(unsigned);

// These variables are only modified at startup (when libc.so is loaded) and never afterwards, so
// it's okay that they are read later at runtime without a lock.
Accept4FunctionType libcAccept4 = 0;
ConnectFunctionType libcConnect = 0;
SocketFunctionType libcSocket = 0;

int closeFdAndSetErrno(int fd, int error) {
    close(fd);
    errno = -error;
    return -1;
}

int netdClientAccept4(int sockfd, sockaddr* addr, socklen_t* addrlen, int flags) {
    int acceptedSocket = libcAccept4(sockfd, addr, addrlen, flags);
    if (acceptedSocket == -1) {
        return -1;
    }
    int family;
    if (addr) {
        family = addr->sa_family;
    } else {
        socklen_t familyLen = sizeof(family);
        if (getsockopt(acceptedSocket, SOL_SOCKET, SO_DOMAIN, &family, &familyLen) == -1) {
            return closeFdAndSetErrno(acceptedSocket, -errno);
        }
    }
    if (FwmarkClient::shouldSetFwmark(family)) {
        FwmarkCommand command = {FwmarkCommand::ON_ACCEPT, 0, 0, 0};
        if (int error = FwmarkClient().send(&command, acceptedSocket, nullptr)) {
            return closeFdAndSetErrno(acceptedSocket, error);
        }
    }
    return acceptedSocket;
}

int netdClientConnect(int sockfd, const sockaddr* addr, socklen_t addrlen) {
    const bool shouldSetFwmark = (sockfd >= 0) && addr
            && FwmarkClient::shouldSetFwmark(addr->sa_family);
    if (shouldSetFwmark) {
        FwmarkCommand command = {FwmarkCommand::ON_CONNECT, 0, 0, 0};
        if (int error = FwmarkClient().send(&command, sockfd, nullptr)) {
            errno = -error;
            return -1;
        }
    }
    // Latency measurement does not include time of sending commands to Fwmark
    Stopwatch s;
    const int ret = libcConnect(sockfd, addr, addrlen);
    // Save errno so it isn't clobbered by sending ON_CONNECT_COMPLETE
    const int connectErrno = errno;
    const unsigned latencyMs = lround(s.timeTaken());
    // Send an ON_CONNECT_COMPLETE command that includes sockaddr and connect latency for reporting
    if (shouldSetFwmark && FwmarkClient::shouldReportConnectComplete(addr->sa_family)) {
        FwmarkConnectInfo connectInfo(ret == 0 ? 0 : connectErrno, latencyMs, addr);
        // TODO: get the netId from the socket mark once we have continuous benchmark runs
        FwmarkCommand command = {FwmarkCommand::ON_CONNECT_COMPLETE, /* netId (ignored) */ 0,
                                 /* uid (filled in by the server) */ 0, 0};
        // Ignore return value since it's only used for logging
        FwmarkClient().send(&command, sockfd, &connectInfo);
    }
    errno = connectErrno;
    return ret;
}

int netdClientSocket(int domain, int type, int protocol) {
    int socketFd = libcSocket(domain, type, protocol);
    if (socketFd == -1) {
        return -1;
    }
    unsigned netId = netIdForProcess;
    if (netId != NETID_UNSET && FwmarkClient::shouldSetFwmark(domain)) {
        if (int error = setNetworkForSocket(netId, socketFd)) {
            return closeFdAndSetErrno(socketFd, error);
        }
    }
    return socketFd;
}

unsigned getNetworkForResolv(unsigned netId) {
    if (netId != NETID_UNSET) {
        return netId;
    }
    // Special case for DNS-over-TLS bypass; b/72345192 .
    if ((netIdForResolv & ~NETID_USE_LOCAL_NAMESERVERS) != NETID_UNSET) {
        return netIdForResolv;
    }
    netId = netIdForProcess;
    if (netId != NETID_UNSET) {
        return netId;
    }
    return netIdForResolv;
}

int setNetworkForTarget(unsigned netId, std::atomic_uint* target) {
    const unsigned requestedNetId = netId;
    netId &= ~NETID_USE_LOCAL_NAMESERVERS;

    if (netId == NETID_UNSET) {
        *target = netId;
        return 0;
    }
    // Verify that we are allowed to use |netId|, by creating a socket and trying to have it marked
    // with the netId. Call libcSocket() directly; else the socket creation (via netdClientSocket())
    // might itself cause another check with the fwmark server, which would be wasteful.
    int socketFd;
    if (libcSocket) {
        socketFd = libcSocket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    } else {
        socketFd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    }
    if (socketFd < 0) {
        return -errno;
    }
    int error = setNetworkForSocket(netId, socketFd);
    if (!error) {
        *target = (target == &netIdForResolv) ? requestedNetId : netId;
    }
    close(socketFd);
    return error;
}

int checkSocket(int socketFd) {
    if (socketFd < 0) {
        return -EBADF;
    }
    int family;
    socklen_t familyLen = sizeof(family);
    if (getsockopt(socketFd, SOL_SOCKET, SO_DOMAIN, &family, &familyLen) == -1) {
        return -errno;
    }
    if (!FwmarkClient::shouldSetFwmark(family)) {
        return -EAFNOSUPPORT;
    }
    return 0;
}

}  // namespace

#define CHECK_SOCKET_IS_MARKABLE(sock)          \
    do {                                        \
        int err;                                \
        if ((err = checkSocket(sock)) != 0) {   \
            return err;                         \
        }                                       \
    } while (false);

// accept() just calls accept4(..., 0), so there's no need to handle accept() separately.
extern "C" void netdClientInitAccept4(Accept4FunctionType* function) {
    if (function && *function) {
        libcAccept4 = *function;
        *function = netdClientAccept4;
    }
}

extern "C" void netdClientInitConnect(ConnectFunctionType* function) {
    if (function && *function) {
        libcConnect = *function;
        *function = netdClientConnect;
    }
}

extern "C" void netdClientInitSocket(SocketFunctionType* function) {
    if (function && *function) {
        libcSocket = *function;
        *function = netdClientSocket;
    }
}

extern "C" void netdClientInitNetIdForResolv(NetIdForResolvFunctionType* function) {
    if (function) {
        *function = getNetworkForResolv;
    }
}

extern "C" int getNetworkForSocket(unsigned* netId, int socketFd) {
    if (!netId || socketFd < 0) {
        return -EBADF;
    }
    Fwmark fwmark;
    socklen_t fwmarkLen = sizeof(fwmark.intValue);
    if (getsockopt(socketFd, SOL_SOCKET, SO_MARK, &fwmark.intValue, &fwmarkLen) == -1) {
        return -errno;
    }
    *netId = fwmark.netId;
    return 0;
}

extern "C" unsigned getNetworkForProcess() {
    return netIdForProcess;
}

extern "C" int setNetworkForSocket(unsigned netId, int socketFd) {
    CHECK_SOCKET_IS_MARKABLE(socketFd);
    FwmarkCommand command = {FwmarkCommand::SELECT_NETWORK, netId, 0, 0};
    return FwmarkClient().send(&command, socketFd, nullptr);
}

extern "C" int setNetworkForProcess(unsigned netId) {
    return setNetworkForTarget(netId, &netIdForProcess);
}

extern "C" int setNetworkForResolv(unsigned netId) {
    return setNetworkForTarget(netId, &netIdForResolv);
}

extern "C" int protectFromVpn(int socketFd) {
    if (socketFd < 0) {
        return -EBADF;
    }
    FwmarkCommand command = {FwmarkCommand::PROTECT_FROM_VPN, 0, 0, 0};
    return FwmarkClient().send(&command, socketFd, nullptr);
}

extern "C" int setNetworkForUser(uid_t uid, int socketFd) {
    CHECK_SOCKET_IS_MARKABLE(socketFd);
    FwmarkCommand command = {FwmarkCommand::SELECT_FOR_USER, 0, uid, 0};
    return FwmarkClient().send(&command, socketFd, nullptr);
}

extern "C" int queryUserAccess(uid_t uid, unsigned netId) {
    FwmarkCommand command = {FwmarkCommand::QUERY_USER_ACCESS, netId, uid, 0};
    return FwmarkClient().send(&command, -1, nullptr);
}

extern "C" int tagSocket(int socketFd, uint32_t tag, uid_t uid) {
    CHECK_SOCKET_IS_MARKABLE(socketFd);
    FwmarkCommand command = {FwmarkCommand::TAG_SOCKET, 0, uid, tag};
    return FwmarkClient().send(&command, socketFd, nullptr);
}

extern "C" int untagSocket(int socketFd) {
    CHECK_SOCKET_IS_MARKABLE(socketFd);
    FwmarkCommand command = {FwmarkCommand::UNTAG_SOCKET, 0, 0, 0};
    return FwmarkClient().send(&command, socketFd, nullptr);
}

extern "C" int setCounterSet(uint32_t counterSet, uid_t uid) {
    FwmarkCommand command = {FwmarkCommand::SET_COUNTERSET, 0, uid, counterSet};
    return FwmarkClient().send(&command, -1, nullptr);
}

extern "C" int deleteTagData(uint32_t tag, uid_t uid) {
    FwmarkCommand command = {FwmarkCommand::DELETE_TAGDATA, 0, uid, tag};
    return FwmarkClient().send(&command, -1, nullptr);
}
