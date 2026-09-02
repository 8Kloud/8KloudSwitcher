/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "web/Http.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace kloud::web {

namespace {

constexpr size_t kMaxHead = 16 * 1024;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = char(std::tolower((unsigned char)c));
    return out;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string HttpRequest::header(std::string_view name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}

bool HttpRequest::wantsUpgradeToWebSocket() const {
    const std::string upgrade = lower(header("upgrade"));
    const std::string connection = lower(header("connection"));
    return method == "GET" && upgrade == "websocket" &&
           connection.find("upgrade") != std::string::npos &&
           !header("sec-websocket-key").empty();
}

Parse parseHttpRequest(std::string_view in, HttpRequest& out, size_t& consumed,
                       size_t maxBody) {
    const size_t headEnd = in.find("\r\n\r\n");
    if (headEnd == std::string_view::npos)
        return in.size() > kMaxHead ? Parse::Bad : Parse::Incomplete;
    if (headEnd > kMaxHead) return Parse::Bad;
    const std::string_view head = in.substr(0, headEnd);

    HttpRequest req;
    size_t lineStart = 0;
    bool first = true;
    while (lineStart <= head.size()) {
        size_t lineEnd = head.find("\r\n", lineStart);
        if (lineEnd == std::string_view::npos) lineEnd = head.size();
        const std::string_view line = head.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 2;
        if (first) {
            first = false;
            const size_t sp1 = line.find(' ');
            const size_t sp2 = line.rfind(' ');
            if (sp1 == std::string_view::npos || sp2 == sp1) return Parse::Bad;
            req.method = std::string(line.substr(0, sp1));
            req.target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
            const std::string_view version = line.substr(sp2 + 1);
            if (version.rfind("HTTP/1.", 0) != 0) return Parse::Bad;
            if (req.method.empty() || req.target.empty() || req.target[0] != '/')
                return Parse::Bad;
            const size_t q = req.target.find('?');
            req.path = percentDecode(
                std::string_view(req.target).substr(0, q));
            if (q != std::string::npos) req.query = req.target.substr(q + 1);
            continue;
        }
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) return Parse::Bad;
        const std::string name = lower(trimView(line.substr(0, colon)));
        const std::string_view value = trimView(line.substr(colon + 1));
        auto& slot = req.headers[name];
        if (!slot.empty()) slot += ", ";
        slot.append(value);
    }

    size_t bodyLen = 0;
    if (const std::string cl = req.header("content-length"); !cl.empty()) {
        const auto [p, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), bodyLen);
        if (ec != std::errc() || p != cl.data() + cl.size()) return Parse::Bad;
        if (bodyLen > maxBody) return Parse::Bad;
    }
    if (lower(req.header("transfer-encoding")).find("chunked") != std::string::npos)
        return Parse::Bad;  // not needed by the GUI; refuse rather than mis-frame
    const size_t total = headEnd + 4 + bodyLen;
    if (in.size() < total) return Parse::Incomplete;
    req.body = std::string(in.substr(headEnd + 4, bodyLen));
    consumed = total;
    out = std::move(req);
    return Parse::Ok;
}

const char* httpStatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 101: return "Switching Protocols";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string httpResponse(int status, std::string_view contentType,
                         std::string_view body, std::string_view extraHeaders) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " +
                      httpStatusText(status) + "\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-cache\r\nConnection: keep-alive\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += extraHeaders;
    out += "\r\n";
    out += body;
    return out;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += char(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string queryParam(std::string_view query, std::string_view key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        const std::string_view pair = query.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        const std::string_view k = pair.substr(0, eq);
        if (k == key) {
            std::string v = percentDecode(
                eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1));
            std::replace(v.begin(), v.end(), '+', ' ');
            return v;
        }
        pos = amp + 1;
    }
    return {};
}

std::string_view contentTypeFor(std::string_view path) {
    const size_t dot = path.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? "" : path.substr(dot + 1);
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    return "application/octet-stream";
}

void sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    const uint64_t bitLen = uint64_t(len) * 8;
    std::string msg(reinterpret_cast<const char*>(data), len);
    msg += char(0x80);
    while (msg.size() % 64 != 56) msg += char(0);
    for (int i = 7; i >= 0; --i) msg += char((bitLen >> (i * 8)) & 0xff);
    auto rol = [](uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(msg[off + i * 4])) << 24) |
                   (uint32_t(uint8_t(msg[off + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(msg[off + i * 4 + 2])) << 8) |
                   uint32_t(uint8_t(msg[off + i * 4 + 3]));
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    const uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = uint8_t(h[i] >> 24);
        digest[i * 4 + 1] = uint8_t(h[i] >> 16);
        digest[i * 4 + 2] = uint8_t(h[i] >> 8);
        digest[i * 4 + 3] = uint8_t(h[i]);
    }
}

std::string base64(const uint8_t* data, size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) |
                           (i + 1 < len ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < len ? uint32_t(data[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::string websocketAccept(std::string_view key) {
    const std::string material =
        std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(material.data()), material.size(),
         digest);
    return base64(digest, 20);
}

std::string websocketHandshakeResponse(std::string_view key) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " +
           websocketAccept(key) + "\r\n\r\n";
}

Parse parseWsFrame(std::string_view in, WsFrame& out, size_t& consumed,
                   size_t maxPayload) {
    if (in.size() < 2) return Parse::Incomplete;
    const uint8_t b0 = uint8_t(in[0]);
    const uint8_t b1 = uint8_t(in[1]);
    if (b0 & 0x70) return Parse::Bad;  // RSV bits: no extensions negotiated
    const bool masked = b1 & 0x80;
    if (!masked) return Parse::Bad;    // clients must mask
    uint64_t len = b1 & 0x7f;
    size_t pos = 2;
    if (len == 126) {
        if (in.size() < 4) return Parse::Incomplete;
        len = (uint64_t(uint8_t(in[2])) << 8) | uint8_t(in[3]);
        pos = 4;
    } else if (len == 127) {
        if (in.size() < 10) return Parse::Incomplete;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(in[2 + i]);
        pos = 10;
    }
    if (len > maxPayload) return Parse::Bad;
    const uint8_t opcode = b0 & 0x0f;
    if ((opcode >= 8) && (len > 125 || !(b0 & 0x80))) return Parse::Bad;
    if (in.size() < pos + 4 + len) return Parse::Incomplete;
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) mask[i] = uint8_t(in[pos + i]);
    pos += 4;
    out.opcode = opcode;
    out.fin = b0 & 0x80;
    out.payload.resize(size_t(len));
    for (size_t i = 0; i < len; ++i)
        out.payload[i] = char(uint8_t(in[pos + i]) ^ mask[i & 3]);
    consumed = pos + size_t(len);
    return Parse::Ok;
}

std::string wsFrame(uint8_t opcode, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 10);
    out += char(0x80 | (opcode & 0x0f));
    const size_t n = payload.size();
    if (n < 126) {
        out += char(n);
    } else if (n < 65536) {
        out += char(126);
        out += char((n >> 8) & 0xff);
        out += char(n & 0xff);
    } else {
        out += char(127);
        for (int i = 7; i >= 0; --i) out += char((uint64_t(n) >> (i * 8)) & 0xff);
    }
    out.append(payload);
    return out;
}

}  // namespace kloud::web
