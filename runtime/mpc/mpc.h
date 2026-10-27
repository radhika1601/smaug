#ifndef MPC_MPC_H
#define MPC_MPC_H

// What a program calls itself, as C++ names for the smaug_* ABI functions
// (mpc/smaug_abi.h). Everything else it computes goes through the ABI calls
// that mpc-lower emits.
#include "mpc/smaug_abi.h"

namespace MPC {
// Connects the two parties: party 1 listens on port, party 2 connects to it.
inline void setup(int party, int port) { smaug_setup(party, port); }
inline void finish() { smaug_finish(); }
// 1 or 2, after setup.
inline int party() { return smaug_party(); }

// AND gates since the last setNumGates.
inline int getNumGates() { return (int)smaug_and_gates(); }
inline void setNumGates() { smaug_reset_and_gates(); }
} // namespace MPC

#endif // MPC_MPC_H
