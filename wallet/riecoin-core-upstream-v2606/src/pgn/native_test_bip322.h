// BIP322 fixture signatures from public, deterministic test keys.
#pragma once

#include <pgn/range_commitment.h>
#include <common/signmessage.h>
#include <key.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <streams.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pgn::core::test {
inline void need(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline CKey known(unsigned scalar)
{
    std::array<unsigned char, 32> bytes{};
    bytes.back() = scalar;
    CKey key;
    key.Set(bytes.begin(), bytes.end(), true);
    return key;
}

inline Bytes sign(const std::string& message, const CKey& key)
{
    auto status = MessageVerificationResult::ERR_INVALID;
    const auto challenge = BIP322Txs::Create(WitnessV0KeyHash(key.GetPubKey()), message, status);
    need(challenge.has_value(), "Native BIP322 challenge could not be prepared");
    CMutableTransaction transaction{challenge->m_to_sign};
    FillableSigningProvider provider;
    provider.AddKey(key);
    const auto& previous = challenge->m_to_spend.vout[0];
    SignatureData data;
    need(ProduceSignature(provider,
                          MutableTransactionSignatureCreator(transaction, 0, previous.nValue, SignOptions{SIGHASH_ALL}),
                          previous.scriptPubKey, data), "Actual fixture key did not sign");
    UpdateInput(transaction.vin[0], data);
    DataStream wire;
    wire << transaction.vin[0].scriptWitness.stack;
    Bytes signature(wire.size());
    std::memcpy(signature.data(), wire.data(), wire.size());
    return signature;
}
} // namespace pgn::core::test
