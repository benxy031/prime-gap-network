// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2013-present The Riecoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <arith_uint256.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
// Version bit selecting the counts-carrying header (the phase 1 gold block).
// Serialization and hashing are self-describing because the bit travels in
// nVersion: a 112-byte header always hashes exactly as it always did, and only
// a 218-byte header carries the section. The bit is never set on the published
// testnet, and validation refuses it on any chain whose genesis does not commit
// the rule.
//
// It sits in the top mask BIP9 reserves for version bits, which is deliberate:
// a header that carries consensus data is not a BIP9 signalling block, and this
// chain has no deployments.
static constexpr int32_t PGN_GOLD_COUNTS_VERSION_BIT = 0x40000000;
static constexpr size_t PGN_GOLD_COUNTS_SECTION_BYTES = 106;

class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    int64_t nTime;
    uint32_t nBits;
    // Six little-endian bytes of range number then 100 preimage counts.
    // Untouched and unhashed unless nVersion selects it, so every existing
    // header keeps its exact bytes and its exact hash.
    std::array<uint8_t, PGN_GOLD_COUNTS_SECTION_BYTES> gold_counts{};
    arith_uint256 nNonce;

    CBlockHeader()
    {
        SetNull();
    }

    bool HasGoldCounts() const { return (static_cast<uint32_t>(nVersion) & static_cast<uint32_t>(PGN_GOLD_COUNTS_VERSION_BIT)) != 0; }

    SERIALIZE_METHODS(CBlockHeader, obj) {
        READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits);
        if (obj.HasGoldCounts()) READWRITE(obj.gold_counts);
        READWRITE(obj.nNonce);
    }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        gold_counts = {};
        nNonce = 0;
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const;
    uint256 GetHashForPoW() const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return nTime;
    }

    int32_t GetPoWVersion() const;
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // Memory-only flags for caching expensive checks
    mutable bool fChecked;                            // CheckBlock()
    /** Non-serialized cache key for the explicitly validated amount domain. */
    mutable std::optional<CAmount> checked_amount_max;
    mutable bool m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable bool m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj), obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        checked_amount_max.reset();
        m_checked_witness_commitment = false;
        m_checked_merkle_root = false;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    /** Historically CBlockLocator's version field has been written to network
     * streams as the negotiated protocol version and to disk streams as the
     * client version, but the value has never been used.
     *
     * Hard-code to the highest protocol version ever written to a network stream.
     * SerParams can be used if the field requires any meaning in the future,
     **/
    static constexpr int DUMMY_VERSION = 260600;

    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = DUMMY_VERSION;
        READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
