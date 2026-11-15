// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <cstddef>
class CRPCTable;
class ChainstateManager;
class ArgsManager;
class CMutableTransaction;
class uint256;
void RegisterPgnSocialIdentityRPCCommands(CRPCTable&);
namespace pgn::social::identity {
void StartNativeAssociations(ChainstateManager&,const ArgsManager&);
void StopNativeAssociations();
bool NativeAssociationsAvailable();
void OfferNativeAssociation(int64_t peer,const std::string& canonical_certificate);
std::optional<std::string> NextNativeAssociationOffer(uint64_t& cursor);
// Preverified exact-parent outputs only; no crypto/DB/wallet/chain scan.
bool AppendNativeAssociations(CMutableTransaction&,const uint256& parent,size_t bytes_available);
}
