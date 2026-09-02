/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "web/Http.h"

using namespace kloud::web;

TEST_CASE("http: parses a request head and body") {
    const std::string raw =
        "POST /api/fs?path=%2Fhome%2Fop%20x&k=v+w HTTP/1.1\r\n"
        "Host: localhost:9924\r\n"
        "Content-Type: application/json\r\n"
        "content-length: 5\r\n"
        "\r\n"
        "hellotrailing";
    HttpRequest req;
    size_t consumed = 0;
    REQUIRE(parseHttpRequest(raw, req, consumed) == Parse::Ok);
    CHECK(consumed == raw.size() - 8);
    CHECK(req.method == "POST");
    CHECK(req.path == "/api/fs");
    CHECK(req.query == "path=%2Fhome%2Fop%20x&k=v+w");
    CHECK(queryParam(req.query, "path") == "/home/op x");
    CHECK(queryParam(req.query, "k") == "v w");
    CHECK(queryParam(req.query, "missing").empty());
    CHECK(req.header("Host") == "localhost:9924");
    CHECK(req.body == "hello");
    CHECK_FALSE(req.wantsUpgradeToWebSocket());
}

TEST_CASE("http: incomplete and malformed input") {
    HttpRequest req;
    size_t consumed = 0;
    CHECK(parseHttpRequest("GET / HTTP/1.1\r\nHost: x\r\n", req, consumed) ==
          Parse::Incomplete);
    CHECK(parseHttpRequest("GET / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc",
                           req, consumed) == Parse::Incomplete);
    CHECK(parseHttpRequest("GARBAGE\r\n\r\n", req, consumed) == Parse::Bad);
    CHECK(parseHttpRequest("GET / HTTP/1.1\r\nContent-Length: x\r\n\r\n", req,
                           consumed) == Parse::Bad);
    CHECK(parseHttpRequest("GET / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n",
                           req, consumed) == Parse::Bad);
    CHECK(parseHttpRequest("GET / HTTP/1.1\r\nBroken header\r\n\r\n", req,
                           consumed) == Parse::Bad);
    CHECK(parseHttpRequest(std::string(20000, 'A'), req, consumed) == Parse::Bad);
}

TEST_CASE("http: response framing and content types") {
    const std::string r = httpResponse(404, "text/plain", "nope");
    CHECK(r.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK(r.find("Content-Length: 4\r\n") != std::string::npos);
    CHECK(r.substr(r.size() - 4) == "nope");
    CHECK(contentTypeFor("/index.html") == "text/html; charset=utf-8");
    CHECK(contentTypeFor("/app.js") == "application/javascript; charset=utf-8");
    CHECK(contentTypeFor("/logo.svg") == "image/svg+xml");
    CHECK(contentTypeFor("/x.bin") == "application/octet-stream");
    CHECK(percentDecode("a%20b%zz%4") == "a b%zz%4");
}

TEST_CASE("websocket: RFC 6455 handshake vectors") {
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    static const uint8_t kAbc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81,
                                     0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50,
                                     0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(std::string(reinterpret_cast<const char*>(digest), 20) ==
          std::string(reinterpret_cast<const char*>(kAbc), 20));
    // Multi-block message (> 55 bytes forces a second SHA-1 block).
    const std::string longMsg(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    sha1(reinterpret_cast<const uint8_t*>(longMsg.data()), longMsg.size(),
         digest);
    CHECK(base64(digest, 20) == "hJg+RBw70m66rkqh+VEp5eVGcPE=");
    CHECK(base64(reinterpret_cast<const uint8_t*>("f"), 1) == "Zg==");
    CHECK(base64(reinterpret_cast<const uint8_t*>("fo"), 2) == "Zm8=");
    CHECK(base64(reinterpret_cast<const uint8_t*>("foo"), 3) == "Zm9v");
    CHECK(websocketAccept("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    const std::string raw =
        "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    HttpRequest req;
    size_t consumed = 0;
    REQUIRE(parseHttpRequest(raw, req, consumed) == Parse::Ok);
    CHECK(req.wantsUpgradeToWebSocket());
    const std::string resp =
        websocketHandshakeResponse(req.header("sec-websocket-key"));
    CHECK(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") !=
          std::string::npos);
}

TEST_CASE("websocket: frame codec") {
    // RFC 6455 5.7: masked "Hello" from the client.
    const std::string masked("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    WsFrame f;
    size_t consumed = 0;
    REQUIRE(parseWsFrame(masked, f, consumed) == Parse::Ok);
    CHECK(consumed == 11);
    CHECK(f.opcode == 1);
    CHECK(f.fin);
    CHECK(f.payload == "Hello");
    CHECK(parseWsFrame(masked.substr(0, 7), f, consumed) == Parse::Incomplete);
    // Unmasked client frame is a protocol error.
    CHECK(parseWsFrame(std::string("\x81\x05Hello", 7), f, consumed) == Parse::Bad);
    // Payload cap.
    std::string big("\x82\xfe\x10\x00", 4);
    big += std::string(4 + 4096, 'x');
    CHECK(parseWsFrame(big, f, consumed, 1024) == Parse::Bad);
    CHECK(parseWsFrame(big, f, consumed, 8192) == Parse::Ok);
    CHECK(f.payload.size() == 4096);
    // 64-bit length.
    std::string huge("\x82\xff", 2);
    huge += std::string("\x00\x00\x00\x00\x00\x01\x00\x00", 8);
    huge += std::string(4 + 65536, 'y');
    REQUIRE(parseWsFrame(huge, f, consumed) == Parse::Ok);
    CHECK(f.payload.size() == 65536);
    CHECK(f.opcode == 2);

    CHECK(wsText("Hello") == std::string("\x81\x05Hello", 7));
    const std::string mid = wsBinary(std::string(300, 'z'));
    CHECK(uint8_t(mid[0]) == 0x82);
    CHECK(uint8_t(mid[1]) == 126);
    CHECK(uint8_t(mid[2]) == 1);
    CHECK(uint8_t(mid[3]) == 44);
    CHECK(mid.size() == 304);
    const std::string large = wsBinary(std::string(70000, 'z'));
    CHECK(uint8_t(large[1]) == 127);
    CHECK(large.size() == 70010);
    const std::string close = wsClose(1001);
    CHECK(close == std::string("\x88\x02\x03\xe9", 4));
}
