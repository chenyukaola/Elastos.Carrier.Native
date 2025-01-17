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

#pragma once

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

namespace test {

class DataStorageTests : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(DataStorageTests);
    CPPUNIT_TEST(testPutAndGetValue);
    CPPUNIT_TEST(testPutAndGetPersistentValue);
    CPPUNIT_TEST(testUpdateSignedValue);
    CPPUNIT_TEST(testUpdateEncryptedValue);
    CPPUNIT_TEST(testPutAndGetPeer);
    CPPUNIT_TEST(testPutAndGetPersistentPeer);
    CPPUNIT_TEST_SUITE_END();

 public:
    void setUp();
    void tearDown();

    void testPutAndGetValue();
    void testPutAndGetPersistentValue();
    void testUpdateSignedValue();
    void testUpdateEncryptedValue();
    void testPutAndGetPeer();
    void testPutAndGetPersistentPeer();

private:
    elastos::carrier::Scheduler scheduler {};

    std::shared_ptr<Node> node1 = nullptr;
    std::shared_ptr<Node> node2 = nullptr;

    std::string path;
    std::string path1;
    std::string path2;
    std::string path3;
};

}  // namespace test
