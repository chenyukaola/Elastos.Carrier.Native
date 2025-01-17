/*
 * Copyright (c) 2022 trinity-tech.io
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <uv.h>

#include <array>
#include <vector>
#include <algorithm>
#include <memory>
#include <cstdint>
#include <stdlib.h>

#include "carrier/blob.h"

#include "connection.h"
#include "activeproxy.h"
#include "packetflag.h"
#include "exceptions.h"
#include "utils/log.h"

namespace elastos {
namespace carrier {
namespace activeproxy {

using Logger = elastos::carrier::Logger;

static std::shared_ptr<Logger> log = Logger::get("AcriveProxy");

const static size_t PACKET_HEADER_BYTES = sizeof(uint16_t) + sizeof(uint8_t);
const static uint32_t KEEP_ALIVE_INTERVAL = 60000;      // 60 seconds
const static uint32_t MAX_KEEP_ALIVE_RETRY = 3;
static const uint32_t KEEP_ALIVE_CHECK_INTERVAL = 5000; // 5 seconds

static const size_t MAX_DATA_PACKET_SIZE = 0x7FFF;      // 32767
static const size_t MAX_CONTROL_PACKET_SIZE = 0x1000;   // 4096
static const size_t MAX_UPSTREAM_READ_BUFFER_SIZE = MAX_DATA_PACKET_SIZE - PACKET_HEADER_BYTES - CryptoBox::MAC_BYTES;

static const size_t MAX_RELAY_WRITE_QUEUE_SIZE = 2 * 1024 * 1024; // 2M bytes

static uint32_t lastConnectionId = 0;

struct ConnectRequest {
    uv_connect_t request;

    ConnectRequest(ProxyConnection* connection) {
        request.data = (void*)connection->ref();
    }

    ~ConnectRequest() {
        ((ProxyConnection*)request.data)->unref();
    };

    ProxyConnection* connection() {
        return (ProxyConnection*)request.data;
    }
};

struct WriteRequest {
    uv_write_t request;
    uv_buf_t buf;
    uint32_t tag;

    WriteRequest(ProxyConnection* connection, size_t size) {
        request.data = (void*)connection->ref();
        buf.base = new char[size];
        buf.len = buf.base ? size : 0;
    }

    ~WriteRequest() {
        delete[] buf.base;
        ((ProxyConnection*)request.data)->unref();
    };

    ProxyConnection* connection() {
        return (ProxyConnection*)request.data;
    }
};

struct ShutdownRequest {
    uv_shutdown_t request;

    ShutdownRequest(ProxyConnection* connection) {
        request.data = (void*)connection->ref();
    }

    ~ShutdownRequest() {
        ((ProxyConnection*)request.data)->unref();
    };

    ProxyConnection* connection() {
        return (ProxyConnection*)request.data;
    }
};

ProxyConnection::ProxyConnection(ActiveProxy& proxy) noexcept :
        id(lastConnectionId++), proxy(proxy)
{
    log->trace("Connection {} created.", id);
}

ProxyConnection::~ProxyConnection() noexcept
{
    log->trace("Connection {} destoried.", id);
}

static const std::string StateNames[] = {
    "Connecting",
    "Authenticating",
    "Idling",
    "Relaying",
    "Closed"
};

std::string ProxyConnection::status() const noexcept
{
    std::string s {};
    s.reserve(128);

    s.append("Connection[").append(std::to_string(id)).append("]: ");
    s.append("ref=").append(std::to_string(refCount)).append(", ");
    s.append("state=").append(StateNames[(int)state]).append(", ");
    s.append("lastReceive=").append(std::to_string((uv_now(proxy.getLoop())-keepAliveTimestamp)/1000)).append("s, ");

    return s;
}

void ProxyConnection::close() noexcept
{
    if (state == ConnectionState::Closed)
        return;

    ConnectionState old = state;
    state = ConnectionState::Closed;

    log->debug("Connection {} is closing...", id);

    if (old <= ConnectionState::Attaching)
        onOpenFailed();

    if (old == ConnectionState::Relaying)
        onIdle();

    if (keepAliveTimer.data) {
        uv_timer_stop(&keepAliveTimer);
        uv_close((uv_handle_t*)&keepAliveTimer, [](uv_handle_t* handle) {
            ProxyConnection* pc = (ProxyConnection*)handle->data;
            handle->data = nullptr;
            pc->unref(); // keepAliveTimer.data
        });
    }

    if (upstream.data) {
        uv_read_stop((uv_stream_t*)&upstream);
        if (!uv_is_closing((uv_handle_t*)&upstream)) {
            uv_close((uv_handle_t*)&upstream, [](uv_handle_t* handle) {
                ProxyConnection* pc = (ProxyConnection*)handle->data;
                handle->data = nullptr;
                pc->unref(); // upstream.data
            });
        }
    }

    if (relay.data) {
        uv_read_stop((uv_stream_t*)&relay);
        if (!uv_is_closing((uv_handle_t*)&relay)) {
            uv_close((uv_handle_t*)&relay, [](uv_handle_t* handle) {
                ProxyConnection* pc = (ProxyConnection*)handle->data;
                handle->data = nullptr;
                log->info("Connection {} closed.", pc->id);
                pc->unref(); // relay.data
            });
        }
    }

    onClosed();
}

int ProxyConnection::connectServer() noexcept
{
    log->info("Connection {} connecting to the server {}...", id, proxy.serverEndpoint());

    auto rc = uv_tcp_init(proxy.getLoop(), &relay);
    if (rc < 0) {
        log->error("Connection {} failed to initialize the tcp conection({}): {}",
                id, rc, uv_strerror(rc));
        return rc;
    }

    // Mandatory
    relay.data = (void*)ref(); // release in close()

    ConnectRequest* request = new ConnectRequest(this);
    rc = uv_tcp_connect((uv_connect_t*)request, &relay, proxy.serverAddress().addr(), [](uv_connect_t* req, int status) {
        ConnectRequest* request = (ConnectRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} connect to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        } else {
            log->info("Connection {} connected to server {}", pc->id, pc->proxy.serverEndpoint());
            pc->establish();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} connect to server {} failed({}): {}", id, rc, uv_strerror(rc));
        delete request;
        unref(); // relay.data
        uv_close((uv_handle_t*)&relay, nullptr);
        return rc;
    }

    return 0;
}

void ProxyConnection::establish() noexcept {
    // The server/Java side not support the socket keep-alive idle time,
    // so we don't use the built-in socket keep-alive mechanism.
    /*
    auto rc = uv_tcp_keepalive(&relay, true, KEEP_ALIVE_INTERVAL);
    if (rc < 0) {
        log->error("Set socket keep-alive failed({}): {}", rc, uv_strerror(rc));
        close()
        return;
    }
    */

    // Setup the keep-alive timer
    log->trace("Connection {} start the keep-alive timer.", id);
    uv_timer_init(proxy.getLoop(), &keepAliveTimer); // always success
    keepAliveTimer.data = (void*)ref(); // release in close()

    // we make sure timer should start success
    uv_timer_start(&keepAliveTimer, [](uv_timer_t* handle) {
        ProxyConnection* pc = (ProxyConnection*)handle->data;
        pc->keepAlive();
    }, KEEP_ALIVE_CHECK_INTERVAL, KEEP_ALIVE_CHECK_INTERVAL);

    state = ConnectionState::Initializing;

    log->trace("Connection {} start reading from the server.", id);
    auto rc = uv_read_start((uv_stream_t*)&relay, [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        ProxyConnection* pc = (ProxyConnection*)handle->data;

        if (pc->state != ConnectionState::Relaying) {
            suggested_size = MAX_CONTROL_PACKET_SIZE;
        } else {
            if (suggested_size > MAX_DATA_PACKET_SIZE)
                suggested_size = MAX_DATA_PACKET_SIZE;
        }

        buf->base = (char*)new char[suggested_size];
        buf->len = buf->base ? suggested_size : 0;
    }, [](uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
        ProxyConnection* pc = (ProxyConnection*)handle->data;
        if (nread > 0) {
            if (pc->state == ConnectionState::Initializing) {
                pc->onChallenge((const uint8_t*)buf->base, nread);
            }
            else {
                pc->onRelayRead((const uint8_t*)buf->base, nread);
            }
        } else if (nread < 0) {
            if (nread == UV_EOF) {
                log->info("Connection {} closed by the server.", pc->id);
            } else {
                log->error("Connection {} read server error({}): {}.", pc->id, nread, uv_strerror(nread));
            }
            pc->close();
        }
        // Regarding to the libuv document: nread might be 0,
        // which does not indicate an error or EOF.
        // This is equivalent to EAGAIN or EWOULDBLOCK under read(2).

        delete[] buf->base;
    });
    if (rc < 0) {
        log->error("Connection {} start read from server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        close();
        return;
    }
}

void ProxyConnection::keepAlive() noexcept
{
    if (state == ConnectionState::Relaying)
        return;

    // Dead connection check
    if (uv_now(proxy.getLoop()) - keepAliveTimestamp >= MAX_KEEP_ALIVE_RETRY * KEEP_ALIVE_INTERVAL) {
        log->warn("Connection {} is dead.", id);
        close();
        return;
    }

    // Keep-alive check
    uint32_t randomShift = Random::uint32(KEEP_ALIVE_CHECK_INTERVAL * 2);
    if (state == ConnectionState::Idling && (uv_now(proxy.getLoop()) - keepAliveTimestamp) >=
            (KEEP_ALIVE_INTERVAL - randomShift))
        sendPingRequest();
}

static inline size_t randomPadding(void)
{
    return Random::uint32(256);
}

/*
 * AUTH packet payload:
 *   - plain
 *     - clientNodeId
 *   - encrypted
 *     - sessionPk[client]
 *     - connectionNonce
 *     - signature[challenge]
 *     - domain length[uint8]
 *     - domain[optional]
 *     - padding
 */
void ProxyConnection::sendAuthenticateRequest(const uint8_t* data, size_t size) noexcept
{
    if (state == ConnectionState::Closed)
        return;

    state = ConnectionState::Authenticating;

    const Id& nodeId = proxy.getNodeId();
    const CryptoBox::PublicKey& pk = proxy.getSessionKey();
    nonce = CryptoBox::Nonce::random();

    auto domain = proxy.getDomainName();
    size_t padding = randomPadding();

    std::vector<uint8_t> plain(CryptoBox::PublicKey::BYTES + CryptoBox::Nonce::BYTES + Signature::BYTES + 1 + domain.size() + padding);
    uint8_t* ptr = plain.data();
    // session key
    std::memcpy(ptr, pk.bytes(), pk.size());
    ptr += pk.size();
    // nonce
    std::memcpy(ptr, nonce.bytes(), nonce.size());
    ptr += nonce.size();
    // signature challenge
    auto challenge = proxy.getNode()->sign({data, size});
    std::memcpy(ptr, challenge.data(), Signature::BYTES);
    ptr += Signature::BYTES;
    // domain length
    *ptr++ = domain.size();
    // domain
    if (domain.size() > 0) {
        std::memcpy(ptr, domain.c_str(), domain.size());
        ptr += domain.size();
    }
    // random padding
    Random::buffer(ptr, padding);

    size = PACKET_HEADER_BYTES + nodeId.size() + CryptoBox::MAC_BYTES + plain.size();

    WriteRequest* request = new WriteRequest{this, size};

    ptr = (uint8_t*)request->buf.base;
    // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::auth();
    // node id
    std::memcpy(ptr, nodeId.data(), nodeId.size());
    ptr += nodeId.size();
    // encrypted: session pk, nonce, challenge signature, domain, padding
    Blob _cipher{ptr, plain.size() + CryptoBox::MAC_BYTES};
    Blob _plain{plain};
    proxy.encryptWithNode(_cipher, _plain);

    log->debug("Connection {} send AUTH to server {}.", id, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send AUTH to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send AUTH to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
    }
}

/*
 * ATTACH packet:
 *   - plain
 *     - clientNodeId
 *   - encrypted
 *     - sessionPk[client]
 *     - connectionNonce
 *     - signature[challenge]
 *   - plain
 *     - padding
 */
void ProxyConnection::sendAttachRequest(const uint8_t* data, size_t size) noexcept
{
    if (state == ConnectionState::Closed)
        return;

    state = ConnectionState::Attaching;

    const Id& nodeId = proxy.getNodeId();
    const CryptoBox::PublicKey& pk = proxy.getSessionKey();
    nonce = CryptoBox::Nonce::random();

    std::vector<uint8_t> plain(CryptoBox::PublicKey::BYTES + CryptoBox::Nonce::BYTES + Signature::BYTES);
    uint8_t* ptr = plain.data();
    // session key
    std::memcpy(ptr, pk.bytes(), pk.size());
    ptr += pk.size();
    // nonce
    std::memcpy(ptr, nonce.bytes(), nonce.size());
    ptr += nonce.size();
    //signature challenge
    auto challenge = proxy.getNode()->sign({data, size});
    std::memcpy(ptr, challenge.data(), Signature::BYTES);

    size_t padding = randomPadding();
    size = PACKET_HEADER_BYTES + nodeId.size() + CryptoBox::MAC_BYTES + plain.size() + padding;
    WriteRequest* request = new WriteRequest{this, size};

    ptr = (uint8_t*)request->buf.base;
    // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::attach();
    // node id
    std::memcpy(ptr, nodeId.data(), nodeId.size());
    ptr += nodeId.size();
    // encrypted: session pk, nonce, challenge signature
    Blob _cipher{ptr, plain.size() + CryptoBox::MAC_BYTES};
    Blob _plain{plain};
    proxy.encryptWithNode(_cipher, _plain);
    ptr += _cipher.size();
    // random padding
    Random::buffer(ptr, padding);

    log->debug("Connection {} send ATTACH to server {}.", id, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send ATTACH to server {} failed({}): {}",
                pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send ATTACH to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
    }
}

/*
 * PING packet:
 *   - plain
 *     - padding
 */
void ProxyConnection::sendPingRequest() noexcept
{
    if (state == ConnectionState::Closed)
        return;

    size_t padding = randomPadding();
    size_t size = PACKET_HEADER_BYTES + padding;

    WriteRequest* request = new WriteRequest{this, size};

    uint8_t* ptr = (uint8_t*)request->buf.base;
    // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::ping();
    // random padding
    Random::buffer(ptr, padding);

    log->debug("Connection {} send PING to server {}.", id, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send PING to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send PING to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
    }
}

static uint8_t randomBoolean(bool v)
{
    uint8_t i = Random::uint8();
    return v ? i | 0x01 : i & 0xFE;
}

/*
 * CONNECTACK packet payload:
 * - plain
 *   - success[uint8]
 *   - padding
 */
void ProxyConnection::sendConnectResponse(bool success) noexcept
{
    if (state == ConnectionState::Closed)
        return;

    size_t padding = randomPadding();
    size_t size = PACKET_HEADER_BYTES + sizeof(uint8_t) + padding;

    WriteRequest* request = new WriteRequest{this, size};
    request->tag = success ? 1 : 0;

    uint8_t* ptr = (uint8_t*)request->buf.base;
    // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::connectAck();
    // success?
    *ptr++ = randomBoolean(success);
    // random padding
    Random::buffer(ptr, padding);

    log->debug("Connection {} send CONNECT ACK({}) to server {}.", id, success, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send CONNECT ACK to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        } else {
            // connect upstream success
            if (request->tag && pc->upstream.data)
                pc->startReadUpstream();

            // connect upstream failed
            if (!request->tag) {
                pc->state = ConnectionState::Idling;
                pc->onIdle();
            }
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send CONNECT ACK to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
    }
}

/*
 * DISCONNECT packet:
 *   - plain
 *     - padding
 */
void ProxyConnection::sendDisconnectRequest() noexcept
{
    if (state == ConnectionState::Closed)
        return;

    size_t padding = randomPadding();
    size_t size = PACKET_HEADER_BYTES + padding;

    WriteRequest* request = new WriteRequest{this, size};

    uint8_t* ptr = (uint8_t*)request->buf.base;
   // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::disconnect();
    // random padding
    Random::buffer(ptr, padding);

    log->debug("Connection {} send DISCONNECT to server {}.", id, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send DISCONNECT to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send DISCONNECT to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
    }
}

/*
 * DATA packet payload:
 * - encrypted
 *   - data
 */
void ProxyConnection::sendDataRequest(const uint8_t* data, size_t _size) noexcept
{
    if (state == ConnectionState::Closed)
        return;

    size_t size = PACKET_HEADER_BYTES + CryptoBox::MAC_BYTES + _size;

    WriteRequest* request = new WriteRequest{this, size};

    uint8_t* ptr = (uint8_t*)request->buf.base;
    // size
    *(uint16_t*)ptr = htons(size);
    ptr += sizeof(uint16_t);
    // flag
    *ptr++ = PacketFlag::data();
    // encrypted: data
    Blob _cipher{ptr, _size + CryptoBox::MAC_BYTES};
    const Blob _plain{data, _size};
    proxy.encrypt(_cipher, _plain, nonce);

    // log->trace("Connection {} send DATA({} bytes) to server {}.", id, size, proxy.serverEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&relay), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} send DATA to server {} failed({}): {}",
                    pc->id, pc->proxy.serverEndpoint(), status, uv_strerror(status));
            pc->close();
        }

        if (pc->upstreamPaused && uv_stream_get_write_queue_size((uv_stream_t*)&pc->relay) <= (MAX_RELAY_WRITE_QUEUE_SIZE >> 2)) {
            pc->upstreamPaused = false;
            pc->startReadUpstream();
            log->debug("Connection {} resume the upstream reading", pc->id);
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} send DATA to server {} failed({}): {}",
                id, proxy.serverEndpoint(), rc, uv_strerror(rc));
        delete request;
        close();
        return;
    }

    if (uv_stream_get_write_queue_size((uv_stream_t*)&relay) >= MAX_RELAY_WRITE_QUEUE_SIZE) {
        upstreamPaused = true;
        uv_read_stop((uv_stream_t*)&upstream);
        log->debug("Connection {} paused the upstream reading due to the server buffer limit.", id);
    }
}

inline void vectorAppend(std::vector<uint8_t>& v, const uint8_t* data, size_t size)
{
    size_t oldSize = v.size();
    v.resize(oldSize + size);
    std::memcpy(v.data() + oldSize, data, size);
}

/*
 * Challenge packet
 * - plain
 *   - Random challenge bytes
 */
void ProxyConnection::onChallenge(const uint8_t* data, size_t size) noexcept {
    if (size < 32 || size > 256) {
        log->error("Connection {} got challenge from the server {}, size is error!",
                id, proxy.serverEndpoint());
        return;
    }

    if (proxy.isAuthenticated()) {
        sendAttachRequest(data, size);
    } else {
        sendAuthenticateRequest(data, size);
    }
}

void ProxyConnection::onRelayRead(const uint8_t* data, size_t size) noexcept
{
    // update the keep-alive timestamp first when we get data from the server
    keepAliveTimestamp = uv_now(proxy.getLoop());

    const uint8_t* ptr = data;
    size_t remaining = size;

    if (stickyBuffer.size() > 0) {
        if (stickyBuffer.size() < PACKET_HEADER_BYTES) {
            int rs = PACKET_HEADER_BYTES - stickyBuffer.size();
            if (remaining < rs) {
                vectorAppend(stickyBuffer, ptr, remaining);
                return;
            }

            vectorAppend(stickyBuffer, ptr, rs);
            ptr += rs;
            remaining -= rs;
        }

        int packetSize = ntohs(*(uint16_t *)stickyBuffer.data());
        int rs = packetSize - stickyBuffer.size();
        if (remaining < rs) {
            vectorAppend(stickyBuffer, ptr, remaining);
            return;
        }

        vectorAppend(stickyBuffer, ptr, rs);
        ptr += rs;
        remaining -= rs;

        processRelayPacket(stickyBuffer.data(), stickyBuffer.size());
        stickyBuffer.resize(0);
    }

    while (remaining > 0) {
        if (remaining < PACKET_HEADER_BYTES) {
            vectorAppend(stickyBuffer, ptr, remaining);
            return;
        }

        int packetSize = ntohs(*(uint16_t *)ptr);
        if (remaining < packetSize) {
            vectorAppend(stickyBuffer, ptr, remaining);
            return;
        }

        processRelayPacket(ptr, packetSize);
        ptr += packetSize;
        remaining -= packetSize;
    }
}

void ProxyConnection::processRelayPacket(const uint8_t* packet, size_t size) noexcept
{
    uint8_t flag = *(packet + sizeof(uint16_t));
    bool ack = PacketFlag::isAck(flag);
    uint8_t type = PacketFlag::getType(flag);

    // log->trace("Connection {} got packet from server {}: type={}, ack={}, size={}",
    //        proxy.serverEndpoint(), id, type, ack, size);

    if (type == PacketFlag::ERR) {
        size_t len = size - PACKET_HEADER_BYTES - CryptoBox::MAC_BYTES;
        uint8_t* plain = (uint8_t*)alloca(len);
        Blob _plain{plain, len};
        const Blob _cipher{packet + PACKET_HEADER_BYTES, size - PACKET_HEADER_BYTES};
        proxy.decrypt(_plain, _cipher, nonce);
        int code = ntohs(*(uint16_t*)plain);
        char* msg = (char*)plain + sizeof(uint16_t);
        log->error("Connection {} got ERR response from the server {}, error({}): {}",
                id, proxy.serverEndpoint(), code, msg);
        close();
        return;
    }

    switch (state) {
    case ConnectionState::Initializing:
    case ConnectionState::Connecting:
    case ConnectionState::Closed:
        assert(!"should not receive any data when connecting or closed.");
        log->error("Connection {} got a packet when connecting or closed.", id);
        close();
        return;

    case ConnectionState::Authenticating:
        if (ack && type == PacketFlag::AUTH) {
            onAuthenticateResponse(packet, size);
            return;
        } else {
            log->error("Connection {} got a wrong packet({}), AUTH acknowledge expected.", id, flag);
            close();
            return;
        }
        break;

    case ConnectionState::Attaching:
        if (ack && type == PacketFlag::ATTACH) {
            onAttachResponse(packet, size);
            return;
        } else {
            log->error("Connection {} got a wrong packet({}), ATTACH acknowledge expected.", id, flag);
            close();
            return;
        }
        break;

    case ConnectionState::Idling:
        if (ack && type == PacketFlag::PING) {
            onPingResponse(packet, size);
            return;
        } else if (!ack && type == PacketFlag::CONNECT) {
            onConnectRequest(packet, size);
            return;
        } else {
            log->error("Connection {} got a wrong packet({}), PING acknowledge or CONNECT expected.", id, flag);
            close();
            return;
        }
        break;

    case ConnectionState::Relaying:
        if (type == PacketFlag::DATA) {
            onDataRequest(packet, size);
            return;
        } else if (!ack && type == PacketFlag::DISCONNECT) {
            onDisconnectRequest(packet, size);
            return;
        } else {
            log->error("Connection {} got a wrong packet({}), DATA or DISCONNECT expected.", id, flag);
            close();
            return;
        }
        break;
    }
}

/*
 * AUTHACK packet payload:
 * - encrypted
 *   - sessionPk[server]
 *   - port[uint16]
 *   - domainEnabled[uint8]
 */
const static size_t AUTH_ACK_SIZE = PACKET_HEADER_BYTES + CryptoBox::MAC_BYTES +
    CryptoBox::PublicKey::BYTES + sizeof(uint16_t) + 1;

void ProxyConnection::onAuthenticateResponse(const uint8_t* packet, size_t size) noexcept
{
    if (size < AUTH_ACK_SIZE) {
        log->error("Connection {} got an invalid AUTH ACK from server {}", id, proxy.serverEndpoint());
        close();
        return;
    }

    log->debug("Connection {} got AUTH ACK from server {}", id, proxy.serverEndpoint());

    std::array<uint8_t, AUTH_ACK_SIZE - PACKET_HEADER_BYTES - CryptoBox::MAC_BYTES> plain;

    Blob _plain{plain};
    const Blob _cipher{packet + PACKET_HEADER_BYTES, AUTH_ACK_SIZE - PACKET_HEADER_BYTES};
    proxy.decryptWithNode(_plain, _cipher);

    const uint8_t* ptr = plain.data();
    CryptoBox::PublicKey serverPk({ptr, CryptoBox::PublicKey::BYTES});
    ptr += CryptoBox::PublicKey::BYTES;
    uint16_t port = ntohs(*(uint16_t*)ptr);
    ptr += sizeof(port);
    bool domainEnabled = *ptr;

    onAuthorized(serverPk, port);

    std::string domain {};
    if (domainEnabled) {
        domain = proxy.getDomainName();
    }

    auto peer = PeerInfo::create(proxy.getPeerKeypair(), proxy.getServerId(), proxy.getNodeId(), port, domain);
    log->info("-**- announcePeer Id: {}, server: {}, port: {}, domain: {} -**- ",
            peer.getId().toBase58String(), proxy.getServerHost(), port, domain);

    proxy.getNode()->announcePeer(peer, true);

    state = ConnectionState::Idling;
    onOpened();

    log->info("Connection {} opened.", id);
}

/*
 * No payload
 */
const static size_t ATTACH_ACK_SIZE = PACKET_HEADER_BYTES;

void ProxyConnection::onAttachResponse(const uint8_t* packet, size_t size) noexcept
{
    log->debug("Connection {} got ATTACH ACK from server {}", id, proxy.serverEndpoint());
    state = ConnectionState::Idling;
    onOpened();
}

/*
 * No payload
 */
void ProxyConnection::onPingResponse(const uint8_t* packet, size_t size) const noexcept
{
    log->debug("Connection {} got PING ACK from server {}", id, proxy.serverEndpoint());
    // ignore the random padding payload.
    // keep-alive time stamp already update when we got the server data.
    // so nothing to do here.
}

/*
 * CONNECT packet payload:
 * - encrypted
 *   - addrlen[uint8]
 *   - addr[16 bytes both for IPv4 or IPv6]
 *   - port[uint16]
 */
const static size_t CONNECT_REQ_SIZE = PACKET_HEADER_BYTES + CryptoBox::MAC_BYTES + 1 + 16 + 2;

void ProxyConnection::onConnectRequest(const uint8_t* packet, size_t size) noexcept
{
    if (size < CONNECT_REQ_SIZE) {
        log->error("Connection {} got an invalid CONNECT from server {}.", id, proxy.serverEndpoint());
        close();
        return;
    }

    log->debug("Connection {} got CONNECT from server {}", id, proxy.serverEndpoint());

    state = ConnectionState::Relaying;
    onBusy();

    std::array<uint8_t, CONNECT_REQ_SIZE - PACKET_HEADER_BYTES - CryptoBox::MAC_BYTES> plain;
    Blob _plain{plain};
    const Blob _cipher{packet + PACKET_HEADER_BYTES, CONNECT_REQ_SIZE - PACKET_HEADER_BYTES};
    proxy.decrypt(_plain, _cipher, nonce);

    const uint8_t* ptr = plain.data();
    uint8_t addrLen = *ptr++;
    const uint8_t* addr = ptr;
    ptr += 16;
    uint16_t port = ntohs(*(uint16_t*)ptr);
    SocketAddress client{{addr, addrLen}, port};

    if (proxy.allow(client)) {
        state = ConnectionState::Relaying;
        openUpstream();
    } else {
        sendConnectResponse(false);
        state = ConnectionState::Idling;
        onIdle();
    }
}

/*
 * No payload
 */
void ProxyConnection::onDisconnectRequest(const uint8_t* packet, size_t size) noexcept
{
    log->debug("Connection {} got DISCONNECT from server {}", id, proxy.serverEndpoint());
    closeUpstream(true);
}

/*
 * DATA packet payload:
 * - encrypted
 *   - data
 */
void ProxyConnection::onDataRequest(const uint8_t* packet, size_t size) noexcept
{
    // log->trace("Connection {} got DATA({}) from server {}", id, size, proxy.serverEndpoint());

    WriteRequest* request = new WriteRequest{this, size - PACKET_HEADER_BYTES - CryptoBox::MAC_BYTES};

    Blob _plain{(uint8_t*)request->buf.base, request->buf.len};
    Blob _cipher{packet + PACKET_HEADER_BYTES, size - PACKET_HEADER_BYTES};
    proxy.decrypt(_plain, _cipher, nonce);

    // log->trace("Connection {} sending {} bytes data to upstream {}", id, request->buf.len, proxy.upstreamEndpoint());
    auto rc = uv_write((uv_write_t*)request, (uv_stream_t*)(&upstream), &request->buf, 1, [](uv_write_t* req, int status) {
        WriteRequest* request = (WriteRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} sent to upstream {} failed({}): {}",
                    pc->id, pc->proxy.upstreamEndpoint(), status, uv_strerror(status));
            pc->sendDisconnectRequest();
            pc->closeUpstream();
        }

        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} write to upstream {} failed({}): {}",
                id, proxy.upstreamEndpoint(), rc, uv_strerror(rc));
        sendDisconnectRequest();
        closeUpstream();
        delete request;
    }
}

void ProxyConnection::openUpstream() noexcept
{
    log->debug("Connection {} connecting to the upstream {}...", id, proxy.upstreamEndpoint());

    auto rc = uv_tcp_init(proxy.getLoop(), &upstream);
    if (rc < 0) {
        log->error("Connection {} failed to initialize the tcp conection({}): {}", id, rc, uv_strerror(rc));
        sendConnectResponse(false);
        state = ConnectionState::Idling;
        onIdle();
    }
    upstream.data = (void*)ref(); // release in closeUpstream() or close()

    ConnectRequest* request = new ConnectRequest{this};
    rc = uv_tcp_connect((uv_connect_t*)request, &upstream, proxy.upstreamAddress().addr(), [](uv_connect_t* req, int status) {
        ConnectRequest* request = (ConnectRequest*)req;
        ProxyConnection* pc = request->connection();

        if (status < 0) {
            log->error("Connection {} connect to upstream {} failed({}): {}",
                    pc->id, pc->proxy.upstreamEndpoint(), status, uv_strerror(status));

            uv_close((uv_handle_t*)&pc->upstream, [](uv_handle_t* handle) {
                ProxyConnection* pc = (ProxyConnection*)handle->data;

                // upstream.data
                handle->data = nullptr;
                pc->unref();
            });
        } else {
            log->info("Connection {} connected to upstream {}", pc->id, pc->proxy.upstreamEndpoint());
        }

        pc->sendConnectResponse(status == 0);
        delete request;
    });
    if (rc < 0) {
        log->error("Connection {} connect to upstream {} failed({}): {}",
                id, proxy.upstreamEndpoint(), rc, uv_strerror(rc));
        uv_close((uv_handle_t*)&upstream, [](uv_handle_t* handle) {
            ProxyConnection* pc = (ProxyConnection*)handle->data;

            // upstream.data
            handle->data = nullptr;
            pc->unref();
        });

        sendConnectResponse(false);
        delete request;
    }
}

void ProxyConnection::closeUpstream(bool force) noexcept
{
    if (state == ConnectionState::Closed)
        return;

    log->debug("Connection {} closing upstream {}", id, proxy.upstreamEndpoint());

    // Stop reading
    uv_read_stop((uv_stream_t*)&upstream);

    if (!upstream.data)
        return;

    int rc = 0;
    ShutdownRequest* request = nullptr;

    if (!force) {
        request = new ShutdownRequest{this};
        rc = uv_shutdown((uv_shutdown_t*)request, (uv_stream_t*)&upstream, [](uv_shutdown_t* req, int status){
            ShutdownRequest* request = (ShutdownRequest*)req;
            ProxyConnection* pc = request->connection();

            if (uv_is_closing((uv_handle_t*)&pc->upstream)) {
                delete request;
                return;
            }

            uv_close((uv_handle_t*)&pc->upstream, [](uv_handle_t* handle) {
                ProxyConnection* pc = (ProxyConnection*)handle->data;
                handle->data = nullptr;
                pc->unref(); // upstream.data
            });

            log->info("Connection {} closed upstream {}", pc->id, pc->proxy.upstreamEndpoint());

            pc->state = ConnectionState::Idling;
            pc->onIdle();
            delete request;
        });
    }
    if (rc < 0 || force) {
        if (uv_is_closing((uv_handle_t*)&upstream)) {
            if (request)
                delete request;
            return;
        }

        if (rc < 0)
            log->warn("Connection {} shutdown upstream failed({}): {}, force to close.", id, rc, uv_strerror(rc));

        uv_close((uv_handle_t*)&upstream, [](uv_handle_t* handle) {
            ProxyConnection* pc = (ProxyConnection*)handle->data;
            handle->data = nullptr;
            pc->unref(); // upstream.data
        });

        log->info("Connection {} closed upstream {}", id, proxy.upstreamEndpoint());

        state = ConnectionState::Idling;
        onIdle();
        delete request;
    }
}

void ProxyConnection::startReadUpstream() noexcept
{
    log->trace("Connection {} start reading from the upstream.", id);

    auto rc = uv_read_start((uv_stream_t*)&upstream, [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
        ProxyConnection* pc = (ProxyConnection*)handle->data;

       if (suggested_size > MAX_UPSTREAM_READ_BUFFER_SIZE)
            suggested_size = MAX_UPSTREAM_READ_BUFFER_SIZE;

        buf->base = new char[suggested_size];
        buf->len = buf->base ? suggested_size : 0;
   }, [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        ProxyConnection* pc = (ProxyConnection*)stream->data;
        if (nread > 0) {
            // log->trace("Connection {} got {} bytes data from upstream {}", pc->id, nread, pc->proxy.upstreamEndpoint());
            pc->sendDataRequest((const uint8_t*)buf->base, nread);
        } else if (nread < 0) {
            if (nread == UV_EOF) {
                log->info("Connection {} upstream closed.", pc->id);
            } else {
                log->error("Connection {} read upstream error({}): {}.", pc->id, nread, uv_strerror(nread));
            }
            pc->sendDisconnectRequest();
            pc->closeUpstream();
        }
        // Regarding to the libuv document: nread might be 0,
        // which does not indicate an error or EOF.
        // This is equivalent to EAGAIN or EWOULDBLOCK under read(2).

        delete[] buf->base;
   });
   if (rc < 0) {
        log->error("Connection {} start read from upstream {} failed({}): {}",
                id, proxy.upstreamEndpoint(), rc, uv_strerror(rc));
        sendDisconnectRequest();
        closeUpstream();
   }
}

} // namespace activeproxy
} // namespace carrier
} // namespace elastos
