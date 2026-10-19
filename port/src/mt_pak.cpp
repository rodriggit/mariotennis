// Low-level pak access used by the Transfer Pak code in file 71 (0x80122xxx). The runtime
// emulates no Controller/Transfer Pak (its osPfs* return PFS_ERR_NOPACK), so these report
// an empty slot too and the game sees no Transfer Pak. Rumble goes through the runtime's
// osMotor* / __osMotorAccess instead.
#include "recomp.h"
#include "mt_hooks.h"

constexpr gpr PFS_ERR_NOPACK = 1;

extern "C" void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}

extern "C" void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}

extern "C" void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = PFS_ERR_NOPACK;
}
