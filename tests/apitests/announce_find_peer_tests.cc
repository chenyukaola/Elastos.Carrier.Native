/*
 * Copyright (c) 2022 - 2023 trinity-tech.io
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

#include <list>
#include <iostream>
#include <array>
#include <carrier.h>

#include "utils.h"
#include "utils/misc.h"
#include "announce_find_peer_tests.h"

using namespace elastos::carrier;
using namespace std::chrono_literals;

namespace test {
CPPUNIT_TEST_SUITE_REGISTRATION(AnnounceFindPeerTests);

void
AnnounceFindPeerTests::setUp() {
    std::string path = getenv("PWD");
    std::string path1 = path + "/carrier1";
    std::string path2 = path + "/carrier2";
    std::string path3 = path + "/carrier3";

    Utils::removeStorage(path1);
    Utils::removeStorage(path2);
    Utils::removeStorage(path3);

    //create node1, node2 and node3
    auto b1 = DefaultConfiguration::Builder {};

    auto ipAddresses = Utils::getLocalIpAddresses();
    b1.setIPv4Address(ipAddresses);
    b1.setListeningPort(42222);
    b1.setStoragePath(path1);

    node1 = std::make_shared<Node>(b1.build());
    node1->start();

    auto b2 = DefaultConfiguration::Builder {};
    b2.setIPv4Address(ipAddresses);
    b2.setListeningPort(42223);
    b2.setStoragePath(path2);

    node2 = std::make_shared<Node>(b2.build());
    node2->start();

    auto b3 = DefaultConfiguration::Builder {};
    b3.setIPv4Address(ipAddresses);
    b3.setListeningPort(42224);
    b3.setStoragePath(path3);

    node3 = std::make_shared<Node>(b3.build());
    node3->start();

    //boostrap
    auto ni1 = NodeInfo {node1->getId(), ipAddresses, node1->getPort()};
    node2->bootstrap(ni1);
    node3->bootstrap(ni1);
}

void
AnnounceFindPeerTests::testPeer() {
    auto peerId1 = Id::random();
    auto peerId2 = Id::random();

    std::vector<int> ports1;
    for (int i = 0; i < 12; i++ )
        ports1.push_back(Utils::getRandom(40000, 45000));

    std::vector<int> ports2;
    for (int i = 0; i < 12; i++)
        ports2.push_back(Utils::getRandom(40000, 45000));

    std::vector<Id> nodes;
    nodes.push_back(node1->getId());
    nodes.push_back(node2->getId());
    nodes.push_back(node3->getId());

    for (int i = 0; i < 4; i++) {
        auto future1 = node1->announcePeer(peerId1, ports1[3*i]);
        auto result1 = future1.get();
        CPPUNIT_ASSERT(result1);

        auto future2 = node2->announcePeer(peerId1, ports1[3*i+1]);
        auto result2 = future2.get();
        CPPUNIT_ASSERT(result2);

        auto future3 = node3->announcePeer(peerId1, ports1[3*i+2]);
        auto result3 = future3.get();
        CPPUNIT_ASSERT(result3);

        auto future4 = node1->announcePeer(peerId2, ports2[3*i]);
        auto result4 = future4.get();
        CPPUNIT_ASSERT(result4);

        auto future5 = node2->announcePeer(peerId2, ports2[3*i+1]);
        auto result5 = future5.get();
        CPPUNIT_ASSERT(result5);

        auto future6 = node3->announcePeer(peerId2, ports2[3*i+2]);
        auto result6 = future6.get();
        CPPUNIT_ASSERT(result6);
    }

    sleep(2);

    auto future7 = node2->findPeer(peerId1, 3);
    auto peers7 = future7.get();
    CPPUNIT_ASSERT(3 == peers7.size());

    auto future8 = node3->findPeer(peerId1, 24);
    auto peers8 = future8.get();
    CPPUNIT_ASSERT(3 == peers8.size());

    for (auto &peer : peers8) {
        auto port = peer->getPort();
        auto node = peer->getNodeId();
        auto result7 = std::find(ports1.begin(), ports1.end(), port);
        CPPUNIT_ASSERT(result7 != ports1.end());

        auto result8 = std::find(nodes.begin(), nodes.end(), node);
        CPPUNIT_ASSERT(result8 != nodes.end());
    }

    //find peer2
    auto future9 = node1->findPeer(peerId2, 24);
    auto peers9 = future9.get();
    CPPUNIT_ASSERT(3 == peers9.size());

    auto future10 = node2->findPeer(peerId2, 4);
    auto peers10 = future10.get();
    CPPUNIT_ASSERT(3 == peers10.size());

    for (auto &peer : peers9) {
        auto port = peer->getPort();
        auto node = peer->getNodeId();
        auto result9 = std::find(ports2.begin(), ports2.end(), port);
        CPPUNIT_ASSERT(result9 != ports2.end());

        auto result10 = std::find(nodes.begin(), nodes.end(), node);
        CPPUNIT_ASSERT(result10 != nodes.end());
    }
}

void
AnnounceFindPeerTests::tearDown() {
    if (node1)
        node1->stop();
    if (node2)
        node2->stop();
    if (node3)
        node3->stop();

    std::string pwd = getenv("PWD");
    Utils::removeStorage(pwd + "/carrier1");
    Utils::removeStorage(pwd + "/carrier2");
    Utils::removeStorage(pwd + "/carrier3");
}

}  // namespace test