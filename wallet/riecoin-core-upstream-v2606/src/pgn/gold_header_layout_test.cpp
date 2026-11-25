#undef NDEBUG
// The counts-carrying header. Two things must both hold: every header that
// exists today keeps its exact 112 bytes and its exact hash, and a header that
// selects the section is 218 bytes with the counts committed to the block id.
// A hash that ignored the section would let a miner rewrite the counts after
// finding the work, so the difference is measured, not assumed.
#include <pgn/gold_partition_counts.h>
#include <pgn/native_bootstrap_hash.h>
#include <consensus/merkle.h>
#include <primitives/block.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>

extern const TranslateFn G_TRANSLATION_FUN = [](const char* text) { return std::string(text); };
RecursiveMutex cs_main;

using namespace pgn::core;

static void need(bool yes, const char* reason)
{
    if (!yes) throw std::runtime_error(reason);
}

static CBlockHeader Plain()
{
    CBlockHeader header;
    header.nVersion = 4;
    header.hashPrevBlock = *uint256::FromHex(
        "00000000000000000000000000000000000000000000000000000000000000ff");
    header.hashMerkleRoot = *uint256::FromHex(
        "00000000000000000000000000000000000000000000000000000000000000ee");
    header.nTime = 1'700'000'000;
    header.nBits = 0x207fffff;
    header.nNonce = NativeBootstrapNonce(7);
    return header;
}

static size_t SerializedSize(const CBlockHeader& header)
{
    DataStream stream;
    stream << header;
    return stream.size();
}

static void CheckLegacyLayout()
{
    const auto header = Plain();
    need(!header.HasGoldCounts(), "A plain header claims the counts section");
    need(SerializedSize(header) == 112, "The plain header is no longer 112 bytes");
    need(header.gold_counts == std::array<uint8_t, PGN_GOLD_COUNTS_SECTION_BYTES>{},
         "A plain header carries counts");

    // Legacy hashing keeps the manual 112-byte path, so the field must not leak
    // into it even though it is part of the class.
    CBlockHeader legacy = header;
    legacy.nNonce = 1; // odd: the legacy branch
    const auto before = legacy.GetHash();
    legacy.gold_counts[0] = 0x7f;
    need(legacy.GetHash() == before, "An unselected section changed a legacy header hash");
    need(SerializedSize(legacy) == 112, "A legacy header serialized the section");
}

static void CheckGoldLayout()
{
    auto header = Plain();
    header.nVersion = 4 | PGN_GOLD_COUNTS_VERSION_BIT;
    need(header.HasGoldCounts(), "The version bit did not select the section");
    need(SerializedSize(header) == 218, "The counts header is not 218 bytes");
    need(218 == 112 + PGN_GOLD_COUNTS_SECTION_BYTES, "Section size changed");

    // The section sits between nBits and nNonce, so the counter family the block
    // generator patches is still the last ten header bytes.
    DataStream stream;
    stream << header;
    need(stream.size() == 218, "Serialized counts header has the wrong length");
    std::vector<uint8_t> bytes(stream.size());
    std::memcpy(bytes.data(), stream.data(), stream.size());
    const auto nonce_low = std::span<const uint8_t>{bytes}.subspan(218 - 32, 2);
    need(nonce_low[0] == 2 && nonce_low[1] == 0,
         "The section did not stay in front of nNonce");

    // Round trip preserves both the bit and every counts byte.
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>(i);
    const auto section = EncodeGoldCountsHeader(424242, counts);
    need(section.has_value(), "Canonical section rejected");
    header.gold_counts = *section;
    DataStream encoded;
    encoded << header;
    CBlockHeader decoded;
    encoded >> decoded;
    need(decoded.HasGoldCounts() && decoded.gold_counts == *section,
         "Counts header did not round-trip");
    need(decoded.GetHash() == header.GetHash(), "Round-trip changed the block id");
    need(CanonicalGoldCountsSection(header.gold_counts), "Encoded section is not canonical");

    // The counts are committed by the block id: changing one byte changes it.
    auto changed = header;
    changed.gold_counts[57] ^= 1;
    need(changed.GetHash() != header.GetHash(), "The block id ignores the counts");

    // And by the PoW preimage, so a found nonce cannot be re-used for other counts.
    need(changed.GetHashForPoW() != header.GetHashForPoW(), "The PoW preimage ignores the counts");
}

static void CheckSectionDecoding()
{
    // A section whose range number is in domain always decodes; the counts are
    // the only payload, so canonicality is structural rather than a version byte.
    const auto zero = EncodeGoldCountsHeader(0, GoldPartitionCounts{});
    need(zero.has_value() && CanonicalGoldCountsSection(*zero), "Zero section rejected");
    const auto top = EncodeGoldCountsHeader((uint64_t{1} << GoldRangeNumberBits) - 1,
                                            GoldPartitionCounts{});
    need(top.has_value() && CanonicalGoldCountsSection(*top), "Top range number section rejected");
    need(!EncodeGoldCountsHeader(uint64_t{1} << GoldRangeNumberBits, GoldPartitionCounts{}).has_value(),
         "Out-of-domain range number encoded");

    std::array<uint8_t, GoldCountsHeaderBytes> bytes{};
    const auto decoded = DecodeGoldCountsHeader(bytes);
    need(decoded.has_value() && decoded->first == 0, "All-zero section did not decode");
}

static void CheckMerkleAndBlock()
{
    // A block's merkle root does not cover the header, so adding the section must
    // leave the transaction commitment untouched and still produce a valid CBlock.
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript{} << OP_0 << int64_t{1};
    coinbase.vout.emplace_back(0, CScript{} << OP_RETURN << Bytes{'P', 'G', 'N'});
    CBlock block;
    block.nVersion = 4 | PGN_GOLD_COUNTS_VERSION_BIT;
    block.nTime = 1'700'000'000;
    block.nBits = 0x207fffff;
    block.nNonce = NativeBootstrapNonce(0);
    block.vtx = {MakeTransactionRef(coinbase)};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const auto root = block.hashMerkleRoot;

    const auto section = EncodeGoldCountsHeader(9, GoldPartitionCounts{});
    need(section.has_value(), "Fixture section rejected");
    block.gold_counts = *section;
    DataStream stream;
    stream << TX_WITH_WITNESS(block);
    const auto serialized = stream.size();
    CBlock round;
    stream >> TX_WITH_WITNESS(round);
    need(round.HasGoldCounts() && round.gold_counts == *section, "Block did not round-trip the section");
    need(round.hashMerkleRoot == root && round.GetHash() == block.GetHash(),
         "Section changed the merkle root or the block id round trip");
    need(serialized > 218, "Serialized block is shorter than its header");
}

// The block index keeps the header, including this section, because a served or
// reconstructed header must hash to the same id as the block it describes. The
// disk codec has to round-trip it or a restart turns every gold block into a
// hash mismatch.
static void CheckIndexRoundTrip()
{
    auto header = Plain();
    // The index reconstructs the previous-block hash from its parent pointer, so
    // a root-less fixture must be a root.
    header.hashPrevBlock.SetNull();
    header.nVersion = 4 | PGN_GOLD_COUNTS_VERSION_BIT;
    const auto section = EncodeGoldCountsHeader(31337, [] {
        GoldPartitionCounts counts{};
        for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>(i % 9);
        return counts;
    }());
    need(section.has_value(), "Fixture section rejected");
    header.gold_counts = *section;

    const CBlockIndex index{header};
    need(index.HasGoldCounts(), "The index lost the version bit");
    need(index.gold_counts.size() == PGN_GOLD_COUNTS_SECTION_BYTES, "The index did not keep the section");
    need(index.GetBlockHeader().gold_counts == *section, "The index served a header without its section");
    need(index.GetBlockHeader().GetHash() == header.GetHash(),
         "The header rebuilt from the index has a different block id");

    // Disk codec, both directions, with the section present.
    const CDiskBlockIndex disk{&index};
    DataStream stream;
    stream << disk;
    const auto counts_bytes = stream.size();
    CDiskBlockIndex loaded;
    stream >> loaded;
    need(loaded.HasGoldCounts(), "The disk codec dropped the version bit");
    need(loaded.gold_counts == index.gold_counts, "The disk codec dropped the section");
    need(loaded.ConstructBlockHash() == index.GetBlockHeader().GetHash(),
         "The disk codec reconstructs a different block id");

    // And a plain entry still encodes exactly as before: 88 bytes of body, no
    // section, and the same reconstructed hash.
    auto plain_header = Plain();
    plain_header.hashPrevBlock.SetNull();
    const CBlockIndex plain_index{plain_header};
    need(!plain_index.HasGoldCounts() && plain_index.gold_counts.empty(),
         "A plain index entry carries a section");
    const CDiskBlockIndex plain_disk{&plain_index};
    DataStream plain_stream;
    plain_stream << plain_disk;
    const auto plain_bytes = plain_stream.size();
    CDiskBlockIndex plain_loaded;
    plain_stream >> plain_loaded;
    need(plain_loaded.gold_counts.empty(), "A plain entry grew a section on disk");
    need(plain_loaded.ConstructBlockHash() == plain_header.GetHash(),
         "A plain entry reconstructs a different block id");
    need(plain_bytes + PGN_GOLD_COUNTS_SECTION_BYTES + 1 == counts_bytes,
         "The section costs more than itself plus its length prefix on disk");
}

int main()
{
    try {
        CheckLegacyLayout();
        CheckGoldLayout();
        CheckSectionDecoding();
        CheckMerkleAndBlock();
        CheckIndexRoundTrip();
        std::cout << "gold header layout ok" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold header layout failed: " << failure.what() << std::endl;
        return 1;
    }
}
