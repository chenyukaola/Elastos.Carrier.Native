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

#include <iostream>
#include <climits>
#include <string>
#include <algorithm>

#include "carrier/id.h"
#include "utils/random_generator.h"
#include "crypto/base58.h"
#include "crypto/shasum.h"
#include "crypto/hex.h"

namespace elastos {
namespace carrier {

Id Id::MIN_ID = Id::zero();
Id Id::MAX_ID = Id::ofHex("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");

std::ostream& operator<< (std::ostream& os, const Id& id) {
    os << static_cast<std::string>(id);
    return os;
}

Id Id::ofHex(const std::string& hexId) {
    Id id;
    id.fromHexString(hexId);
    return id;
}

Id Id::ofBase58(const std::string& base58Id) {
    Id id;
    id.fromBase58String(base58Id);
    return id;
}

Id Id::ofName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    int len = strlen(name.c_str());
    std::vector<uint8_t> data(len);
    std::memcpy((void*)data.data(), name.c_str(), len);
    auto d = SHA256::digest(data);
    auto id = Id(d);
    return id;
}

Id Id::random() {
    Id id;
    auto a = reinterpret_cast<uint32_t*>(id.bytes.data());
    auto b = reinterpret_cast<uint32_t*>(id.bytes.data()+ ID_BYTES);

    RandomGenerator<uint32_t> generator;
    std::generate(a, b, generator);
    return id;
}

Id Id::distance(const Id& to) const {
    Id id;
    for (int i = 0; i < ID_BYTES; i++)
        id.bytes[i] = bytes[i] ^ to.bytes[i];

    return id;
}

Id Id::distance(const Id& id1, const Id& id2) {
    return id1.distance(id2);
}

int Id::threeWayCompare(const Id &id1, const Id &id2) const {
    int mmi = -1;
    for (int i = 0; i < ID_BYTES; i++) {
        if (id1.bytes[i] != id2.bytes[i]) {
            mmi = i;
            break;
        }
    }
    if (mmi == -1) return 0;

    uint8_t a = id1.bytes[mmi] ^ bytes[mmi];
    uint8_t b = id2.bytes[mmi] ^ bytes[mmi];

    if (a < b)
        return -1;
    else if (a > b)
        return 1;
    else
        return 0;
}

bool Id::bitsEqual(const Id& id1, const Id& id2, int n) {
    if (n < 0)
        return true;

    int mmi = INT_MAX;
    for (int i = 0; i < ID_BYTES; i++) {
        if (id1.bytes[i] != id2.bytes[i]) {
            mmi = i;
            break;
        }
    }

    int indexToCheck = n >> 3;

    uint8_t diff = (id1.bytes[indexToCheck] ^ id2.bytes[indexToCheck]);
    bool bitsDiff = (diff & (0xff80 >> (n & 0x07))) == 0;

    if (mmi == indexToCheck)
        return bitsDiff;
    else
        return mmi > indexToCheck;
}

void Id::bitsCopy(const Id& src, Id& dest, int depth) {
    if (depth < 0)
        return;

    // copy over all complete bytes
    int idx = depth >> 3;
    if (idx > 0)
        std::memcpy(dest.bytes.data(), src.bytes.data(), idx);

    int mask = 0xff80 >> (depth & 0x07);

    // mask out the part we have to copy over from the last prefix byte
    dest.bytes[idx] &= (~mask);
    // copy the bits from the last byte
    dest.bytes[idx] |= (src.bytes[idx] & mask);
}

int Id::getLeadingZeros() {
    int msb = 0;
    int i = 0;

    // find the index of first non-zero byte in byte.
    for (; i < ID_BYTES && bytes[i] == 0; i++);

    // the number of leading zero bytes to the most significant bit
    msb += i << 3;

    if (i < ID_BYTES) {
        uint8_t b = bytes[i]; // first non-zero byte
        int n = 7;

        // skip four bits if they are zero
        if (b >= 1 << 4) {
            n -= 4;
            b >>= 4;
        }
        // skip two bits if they are zero
        if (b >= 1 << 2) {
            n -= 2;
            b >>= 2;
        }
        // add the index of most significant bit of first non-zero byte
        msb += (n - (b >> 1));
    }
    return msb;
}

bool Id::operator<(const Id& other) const {
    return std::lexicographical_compare(bytes.begin(), bytes.end(), other.bytes.begin(), other.bytes.end());
}

const std::string Id::toHexString() const {
    return Hex::encode(bytes, true);
}

const std::string Id::toBase58String() const {
    return base58_encode((uint8_t*)bytes.data(), bytes.size());
}

const std::string Id::toBinaryString() const {
    std::string str {};

    for(int i = 0; i < ID_BYTES * 8; i++) {
        str.append(1, (bytes[i / 3] & (0x80 >> (i & 0x07))) != 0 ? '1' : '0');
        if ((i & 0x03) == 0x03) str.append(1, ' ');
    }
    return str;
}

void Id::fromBase58String(const std::string& str) {
    std::vector<uint8_t> data = base58_decode(str.c_str());
    if(data.size() != ID_BYTES)
        throw std::out_of_range("Invalid id string, the size of data from decoding string is 32 bytes.");

    std::memcpy(bytes.data(), data.data(), ID_BYTES);
}

void Id::fromHexString(const std::string& str) {
    const std::string substr = "0x";
    auto prefix = str.substr(0, substr.length()) == substr;
    auto pos = prefix ? substr.length() : 0;

    if (str.length() != ID_BYTES * 2 + pos)
        throw std::out_of_range("Hex ID string should be 64 characters long.");

    Hex::decode(str.c_str() + pos, ID_BYTES * 2, bytes.data(), ID_BYTES);
}

}
}
