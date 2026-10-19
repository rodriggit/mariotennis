"""libultra functions identified in main (VRAM -> name).

N64Recomp replaces the functions in its reimplemented/ignored lists by name,
so these must be named in syms.toml. Each was identified from its behaviour
(hardware registers, OSThread/OSMesgQueue field use, constants) and the call
graph; see docs/libultra.md for the reasoning behind the less obvious ones.
"""

LIBULTRA = {
    # threads, messages, interrupts
    0x80032C74: "osGetCount",
    0x80032DE0: "osRecvMesg",
    0x80032EA0: "osSendMesg",
    0x80032F60: "__osRestoreInt",
    0x80032F80: "__osDisableInt",
    0x8003A980: "osStartThread",
    0x80045070: "osCreateMesgQueue",
    0x800450A0: "osJamMesg",
    0x800451E0: "osSetEventMesg",
    0x80045590: "osCreateThread",
    0x80045660: "osDestroyThread",
    0x80045740: "osGetThreadId",
    0x80045760: "osGetThreadPri",
    0x80045780: "osSetThreadPri",
    0x80045850: "osStopThread",
    0x80045910: "__osDequeueThread",
    0x80045950: "osYieldThread",
    0x80042250: "osSetIntMask",
    # exceptasm
    0x800418E0: "__osException",
    0x80041E6C: "send_mesg",
    0x80041F5C: "__osEnqueueAndYield",
    0x80042064: "__osEnqueueThread",
    0x800420AC: "__osPopThread",
    0x800420BC: "__osNop",
    0x800420C4: "__osDispatchThread",
    0x80042240: "__osCleanupThread",
    # timers
    0x800459A0: "osGetTime",
    0x80045A30: "osSetTimer",
    0x80045B60: "__osTimerServicesInit",
    0x80045BB4: "__osTimerInterrupt",
    0x80045CF0: "__osSetTimerIntr",
    0x80045D70: "__osInsertTimer",
    # TLB / cop0 / cache
    0x80032BC0: "osMapTLB",
    # osVirtualToPhysical, renamed: the runtime's ignores the TLB; the port implements it
    # (ignored in gen_config.py, port/src/mt_hooks.cpp)
    0x80033160: "osVirtualToPhysical_tlb",
    0x80045E80: "osUnmapTLBAll",
    0x80048850: "osMapTLBRdb",
    0x80048650: "__osGetCause",
    0x80048660: "__osGetSR",
    0x80048670: "__osSetCompare",
    0x80048680: "__osSetFpcCsr",
    0x80048690: "__osSetSR",
    0x800486A0: "__osSetWatchLo",
    0x8003AC10: "osWritebackDCacheAll",   # game variant (TagLo walk), same effect
    0x8003AC64: "osWritebackDCache",      # game variant
    0x8003AD40: "osInvalDCache",          # game variant
    0x80042F20: "osInvalICache",
    # init
    0x80047908: "osInitialize",           # old single-function form, called by 0x80031000
    0x80047850: "__createSpeedParam",
    # PI
    0x800422F0: "osCreatePiManager",
    0x80042480: "__osEPiRawStartDma",
    0x80042670: "osEPiStartDma",
    0x80042710: "osCartRomInit",
    0x80042890: "__osDevMgrMain",
    0x80042C50: "__osPiCreateAccessQueue",
    0x80042CA4: "__osPiGetAccess",
    0x80042D10: "__osPiRelAccess",
    0x80048250: "osPiRawStartDma",
    0x80048320: "osPiGetCmdQueue",
    0x80048340: "__osEPiRawReadIo",
    0x800484B0: "__osEPiRawWriteIo",
    # AI
    0x80042D40: "osAiGetLength",
    0x80042D50: "osAiGetStatus",
    0x80042D60: "osAiSetFrequency",
    0x80042E80: "osAiSetNextBuffer",
    0x80048620: "__osAiDeviceBusy",
    # VI
    0x80045ED0: "osViGetCurrentFramebuffer",
    0x80045F10: "osViGetNextFramebuffer",
    0x80045F50: "osCreateViManager",
    0x800460F0: "viMgrMain",
    0x80046290: "osViSetEvent",
    0x800462F0: "osViSetMode",
    0x80046340: "osViSetSpecialFeatures",
    0x800464B0: "osViSetXScale",
    0x80046580: "osViSetYScale",
    0x800465D0: "osViSwapBuffer",
    0x80046620: "__osViSwapContext",
    0x80046930: "osViBlack",
    0x800488B0: "__osViInit",
    0x800489C0: "__osViGetCurrentContext",
    # SP
    0x8003AA70: "osSpTaskStartGo",
    0x8003AA9C: "osSpTaskLoad",
    0x80045290: "__osSpDeviceBusy",
    0x800452B0: "__osSpSetStatus",
    0x800452C0: "__osSpSetPc",
    0x800452F0: "__osSpRawStartDma",
    0x80045380: "osSpTaskYield",
    0x800453A0: "osSpTaskYielded",
    0x800486B0: "__osSpGetStatus",
    0x80032C68: "osDpSetStatus",          # sw a0, DPC_STATUS (0xA410000C)
    # SI, controller, paks
    0x800453F0: "__osSiRawStartDma",
    0x800454A0: "__osSiCreateAccessQueue",
    0x800454F4: "__osSiGetAccess",
    0x80045560: "__osSiRelAccess",
    0x800486C0: "__osSiRawReadIo",
    0x80048710: "__osSiRawWriteIo",
    0x80049A60: "__osSiDeviceBusy",
    0x80043220: "osContInit",             # 0x165A0BC = 500 ms wait
    0x800433BC: "__osContGetInitData",
    0x80043470: "__osPackRequestData",
    0x80042FA0: "osContStartQuery",
    0x80043020: "osContGetQuery",
    0x80043040: "osContStartReadData",
    0x800430C8: "osContGetReadData",
    0x80043164: "__osPackReadData",
    0x80046FD0: "__osMotorAccess",
    0x80047114: "osMotorInit",
    0x80047360: "__osPfsSelectBank",
    0x800473D0: "__osContRamRead",
    0x800475C0: "__osContRamWrite",
    0x80048760: "__osContAddressCrc",
    0x800487CC: "__osContDataCrc",
    # pak status (by call pattern: SI access, request, raw DMA write/read, unpack)
    0x800489D0: "osPfsIsPlug",
    0x80048B4C: "__osPfsRequestData",
    0x80048BF0: "__osPfsGetInitData",
    0x80048CB0: "__osPfsGetStatus",        # also called by the Transfer Pak code in file 71
    0x80048DA0: "__osPfsRequestOneChannel",
    0x80048E34: "__osPfsGetOneChannelData",
    # EEPROM
    0x80041700: "osEepromProbe",
    0x80041780: "osEepromLongWrite",      # write + osSetTimer delay
    0x80041870: "osEepromLongRead",
    0x80047BF0: "osEepromRead",
    0x80047D88: "__osPackEepReadData",
    0x80047E10: "osEepromWrite",
    0x80047F78: "__osPackEepWriteData",
    0x80048020: "__osEepStatus",
}
