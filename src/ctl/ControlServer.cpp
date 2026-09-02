/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ctl/ControlServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/Log.h"
#include "ctl/ControlApply.h"
#include "engine/Engine.h"

namespace kloud::ctl {

namespace {
constexpr size_t kMaxClients = 16;
constexpr size_t kMaxLine = 4096;     // longest accepted request line
constexpr size_t kMaxOutBuf = 1 << 20;  // slow reader: drop past 1 MiB queued
constexpr int kPollMs = 30;           // state publish cadence

bool setNonBlock(int fd) {
    const int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
}  // namespace

ControlServer::ControlServer(Engine& engine, int port)
    : engine_(engine), port_(port) {
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        KLOUD_LOGE("control: socket: %s", strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (bind(listenFd_, (const sockaddr*)&addr, sizeof addr) != 0 ||
        listen(listenFd_, 8) != 0 || !setNonBlock(listenFd_)) {
        KLOUD_LOGE("control: cannot listen on tcp/%d: %s (remote control off)",
                 port, strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return;
    }
    wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    KLOUD_LOGI("control: listening on tcp/%d", port);
}

ControlServer::~ControlServer() {
    if (thread_.joinable()) {
        thread_.request_stop();
        if (wakeFd_ >= 0) {
            const uint64_t one = 1;
            [[maybe_unused]] const ssize_t n = write(wakeFd_, &one, sizeof one);
        }
        thread_.join();
    }
    for (const auto& c : clients_) close(c.fd);
    if (wakeFd_ >= 0) close(wakeFd_);
    if (listenFd_ >= 0) close(listenFd_);
}

void ControlServer::send(Client& c, const std::string& line) {
    if (c.out.size() + line.size() > kMaxOutBuf) {
        c.out.clear();
        c.in = "\x01";  // poison: reader drops the client this iteration
        return;
    }
    c.out += line;
    c.out += '\n';
}

void ControlServer::sendError(Client& c, const std::string& msg) {
    send(c, "{\"event\":\"error\",\"message\":\"" + jsonEscape(msg) + "\"}");
}

void ControlServer::apply(const Request& r, Client& c) {
    switch (r.op) {
        case Request::Op::Subscribe:
            c.subscribed = true;
            send(c, lastState_.empty() ? toJson(snapshot(engine_)) : lastState_);
            return;
        case Request::Op::Unsubscribe: c.subscribed = false; return;
        default: break;
    }
    const ApplyResult res = ctl::apply(engine_, r);
    if (!res.ok) sendError(c, res.error);
    if (!res.reply.empty()) send(c, res.reply);
}

void ControlServer::run(std::stop_token st) {
    while (!st.stop_requested()) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakeFd_, POLLIN, 0});
        for (const auto& c : clients_)
            fds.push_back({c.fd,
                           short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        if (poll(fds.data(), nfds_t(fds.size()), kPollMs) < 0 &&
            errno != EINTR) {
            KLOUD_LOGE("control: poll: %s", strerror(errno));
            return;
        }
        if (st.stop_requested()) return;

        // Clients accepted below are not in this iteration's fds; the read
        // phase must only walk the clients that were polled.
        const size_t nPolled = clients_.size();
        if (fds[0].revents & POLLIN) {
            const int fd = accept4(listenFd_, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                if (clients_.size() >= kMaxClients) {
                    close(fd);
                } else {
                    const int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Client c;
                    c.fd = fd;
                    send(c, "{\"event\":\"hello\",\"name\":\"8Kloud Switcher\","
                            "\"protocol\":1}");
                    clients_.push_back(std::move(c));
                }
            }
        }

        // Reads + request handling. Client fds start at fds[2].
        for (size_t i = 0; i < nPolled; ++i) {
            Client& c = clients_[i];
            if (c.in == "\x01") {  // poisoned by send() overflow
                close(c.fd);
                c.fd = -1;
                continue;
            }
            if (!(fds[i + 2].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            char buf[4096];
            for (;;) {
                const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, size_t(n));
                    if (c.in.size() > kMaxLine * 4) {  // garbage flood
                        close(c.fd);
                        c.fd = -1;
                        break;
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                close(c.fd);  // orderly close or hard error
                c.fd = -1;
                break;
            }
            if (c.fd < 0) continue;
            size_t nl;
            while ((nl = c.in.find('\n')) != std::string::npos) {
                std::string line = c.in.substr(0, nl);
                c.in.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > kMaxLine) continue;
                std::string err;
                if (const auto req = parseLine(line, err))
                    apply(*req, c);
                else if (!err.empty())
                    sendError(c, err);
            }
        }

        // State publish on change.
        const std::string state = toJson(snapshot(engine_));
        const bool changed = state != lastState_;
        if (changed) lastState_ = state;
        for (auto& c : clients_) {
            if (c.fd < 0) continue;
            if (changed && c.subscribed) send(c, lastState_);
            if (c.out.empty()) continue;
            const ssize_t n =
                ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n > 0)
                c.out.erase(0, size_t(n));
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(c.fd);
                c.fd = -1;
            }
        }
        std::erase_if(clients_, [](const Client& c) { return c.fd < 0; });
    }
}

}  // namespace kloud::ctl
