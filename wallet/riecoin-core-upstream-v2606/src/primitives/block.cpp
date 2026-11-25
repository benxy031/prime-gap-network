// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2013-present The Riecoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <memory>
#include <sstream>

uint256 CBlockHeader::GetHash() const
{
    // A counts-carrying header is new by construction and always hashes from
    // its serialized 218 bytes, so the section is committed to the block id
    // instead of riding along unhashed.
    if (!HasGoldCounts() && ((nNonce & 1) == 1 || (nNonce & 65535) == 0)) { // "Legacy" PoW: the hash is done after swapping nTime and nBits; the MainNet Genesis Block has all the offset bits to 0
        uint8_t blockData[112];
        memcpy(&blockData[0], &nVersion, 4);
        memcpy(&blockData[4], hashPrevBlock.begin(), 32);
        memcpy(&blockData[36], hashMerkleRoot.begin(), 32);
        memcpy(&blockData[68], &nBits, 4);
        memcpy(&blockData[72], &nTime, 8);
        memcpy(&blockData[80], ArithToUint256(nNonce).begin(), 32);
        // Hash the whole Block Header
        return Hash(blockData);
    }
    else
        return (HashWriter{} << *this).GetHash();
}

uint256 CBlockHeader::GetHashForPoW() const
{
    uint8_t blockData[80];
    memcpy(&blockData[0], &nVersion, 4);
    memcpy(&blockData[4], hashPrevBlock.begin(), 32);
    memcpy(&blockData[36], hashMerkleRoot.begin(), 32);
    if ((nNonce & 1) == 1 || (nNonce & 65535) == 0) { // "Legacy" PoW: the hash is done after swapping nTime and nBits; the MainNet Genesis Block has all the offset bits to 0
        memcpy(&blockData[68], &nBits, 4);
        memcpy(&blockData[72], &nTime, 8);
    }
    else {
        memcpy(&blockData[68], &nTime, 8);
        memcpy(&blockData[76], &nBits, 4);
    }
    if (HasGoldCounts()) {
        // The preimage protects the whole declaration, not just its prefix:
        // the counts are mined over, so they cannot be rewritten after the
        // work is found.
        uint8_t withCounts[80 + PGN_GOLD_COUNTS_SECTION_BYTES];
        memcpy(withCounts, blockData, 80);
        memcpy(withCounts + 80, gold_counts.data(), PGN_GOLD_COUNTS_SECTION_BYTES);
        return Hash(withCounts);
    }
    // Hash the Block Header without nNonce
    return Hash(blockData);
}

int32_t CBlockHeader::GetPoWVersion() const
{
    if ((nNonce & 1) == 1) // "Legacy" PoW
        return -1;
    else if ((nNonce & 31) == 2) // PoW after second fork
        return 1;
    else // Invalid PoW
        return 0;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%lu, nBits=0x%08x, gold_counts=%s, nNonce=%s, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, HasGoldCounts() ? HexStr(gold_counts).c_str() : "-", nNonce.ToString().c_str(),
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
