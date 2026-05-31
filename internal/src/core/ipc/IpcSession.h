#pragma once

#include <cstdint>

namespace Handshake { struct AuthState; }

namespace IpcSession {

bool IsAsciiIdSafe(const char* s);
bool ParseUint64Dec(const char* s, uint64_t* out);
bool ConstantTimeHexEq64(const char* a, const char* b);
bool DeriveSessionKey(const char* serverChallenge, const char* clientChallenge, const char* userId, const char* clientPid, uint8_t outKey[32], const char* pipeName);
bool ComputeSessionMacHex(const uint8_t key[32], uint64_t seq, const char* type, const char* payload, char outHex[65]);
bool VerifyClientSeqAndMac(Handshake::AuthState* auth, const char* seqStr, const char* macHex, const char* type, const char* payload);

} // namespace IpcSession
