// Plaintext stand-ins for the protocol calls a program makes itself (see
// mpc/mpc.h). Linked into the native reference build, which runs without
// the passes and so makes no other ABI calls.
#include "mpc/smaug_abi.h"

static int32_t party_ = 1;

extern "C" {
void smaug_setup(int32_t party, int32_t) { party_ = party; }
void smaug_setup_stdin(void) {}
void smaug_finish(void) {}
int64_t smaug_and_gates(void) { return 0; }
void smaug_reset_and_gates(void) {}
int32_t smaug_party(void) { return party_; }
}
