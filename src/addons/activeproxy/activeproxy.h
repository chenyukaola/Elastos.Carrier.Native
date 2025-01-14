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

#pragma once

#include <uv.h>

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <thread>

#include "carrier.h"
#include "carrier/blob.h"

namespace elastos {
namespace carrier {
namespace activeproxy {

using SocketAddress = elastos::carrier::SocketAddress;
using CryptoBox = elastos::carrier::CryptoBox;

class ProxyConnection;

class ActiveProxy : public Addon {
public:
    std::future<void> initialize(Sp<Node> node, const std::map<std::string, std::any>& config) override;

    std::future<void> deinitialize() override;

    bool isInitialized() override {
        return isRunning();
    }

    const std::string& getServerHost() const noexcept {
        return serverHost;
    }

    const std::string& serverEndpoint() const noexcept {
        return serverName;
    }

    const std::string& upstreamEndpoint() const noexcept {
        return upstreamName;
    }

    const SocketAddress& serverAddress() const noexcept {
        return serverAddr;
    }

    const SocketAddress& upstreamAddress() const noexcept {
        return upstreamAddr;
    }

    bool isRunning() const noexcept {
        return running;
    }

    void start();
    void stop() noexcept;

    uv_loop_t* getLoop() noexcept {
        return &loop;
    }

    const Sp<Node>& getNode() {
        return node;
    }

    const Id& getNodeId() const noexcept {
        return node->getId();
    }

    const CryptoBox::PublicKey& getSessionKey() const noexcept {
        return sessionKey.publicKey();
    }

    bool isAuthenticated() const noexcept {
        return serverPk.has_value();
    }

    bool allow(SocketAddress& client) const noexcept {
        return true;
    }

    const Id& getServerId() const noexcept {
        return serverId;
    }

    uint16_t getRelayPort() const noexcept {
        return relayPort;
    }

    const Signature::KeyPair& getPeerKeypair() const noexcept {
        return peerKeypair;
    }

    const std::string& getDomainName() const noexcept {
        return domainName;
    }

    // encrypt/decrypt with the session context
    void encrypt(Blob& cipher, const Blob& plain, const CryptoBox::Nonce& nonce) const {
        box.encrypt(cipher, plain, nonce);
    }

    std::vector<uint8_t> encrypt(const Blob& plain, const CryptoBox::Nonce& nonce) const {
        return box.encrypt(plain, nonce);
    }

    void decrypt(Blob& plain, const Blob& cipher, const CryptoBox::Nonce& nonce) const {
        box.decrypt(plain, cipher, nonce);
    }

    std::vector<uint8_t> decrypt(const Blob& cipher, const CryptoBox::Nonce& nonce) const {
        return box.decrypt(cipher, nonce);
    }

    // encrypt/decrypt with the node context
    void encryptWithNode(Blob& cipher, const Blob& plain) const {
        node->encrypt(serverId, cipher, plain);
    }

    std::vector<uint8_t> encryptWithNode(const Blob& plain) const {
        return node->encrypt(serverId, plain);
    }

    void decryptWithNode(Blob& plain, const Blob& cipher) const {
        node->decrypt(serverId, plain, cipher);
    }

    std::vector<uint8_t> decryptWithNode(const Blob& cipher) const {
        return node->decrypt(serverId, cipher);
    }

protected:
    void onStop() noexcept;
    void onIteration() noexcept;
    void _connect() noexcept;
    void connect() noexcept;
    void idleCheck() noexcept;
    bool needsNewConnection() const noexcept;

private:
    Sp<Node> node;

    CryptoBox::KeyPair sessionKey;

    std::optional<CryptoBox::PublicKey> serverPk {};
    CryptoBox box;

    uint16_t relayPort;

    Id serverId {};
    std::string serverHost {};
    int serverPort {0};
    std::string serverName {};
    SocketAddress serverAddr {};
    uint32_t serverFails  {0};
    Id peerId {};
    std::string domainName {};

    std::string upstreamHost;
    int upstreamPort;
    std::string upstreamName;
    SocketAddress upstreamAddr;
    uint32_t upstreamFails { 0 };

    uv_loop_t loop { 0 };
    uv_async_t stopHandle { 0 };
    uv_idle_t idleHandle { 0 };

    uv_timer_t reconnectTimer { 0 };
    uint32_t reconnectInterval { 0 };

    uv_timer_t idleCheckTimer { 0 };
    uint64_t idleTimestamp;

    bool running { false };
    bool first { false };

    uint32_t maxConnections { 8 };
    uint32_t inFlights { 0 };

    std::vector<ProxyConnection*> connections;

    std::thread runner;

    std::promise<void> startPromise {};
    std::promise<void> stopPromise {};

    Signature::KeyPair peerKeypair {};
};

} // namespace activeproxy
} // namespace carrier
} // namespace elastos

