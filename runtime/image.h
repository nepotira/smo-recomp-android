#ifndef IMAGE_H
#define IMAGE_H

#define GAME_VERSION "1.0.0"
#define SDK_VERSION 0x00030501u
#define GAME_ELF_DEFAULT "main.elf"
#define GAME_BUILD_ID "3947b198fa0d947f"

/* Android virtual address space layout (extracted from libsmo.so & target.py) */
#define SMO_LAYOUT_GUEST_BASE  0x4000000000ULL
#define SMO_LAYOUT_IMPORT_BASE 0x4100000000ULL
#define SMO_LAYOUT_HOSTFN_BASE 0x4200000000ULL
#define SMO_LAYOUT_SDK_BASE    0x4300000000ULL

/* Exact ELF layout of main.elf (verified via readelf -l -d main.elf & libsmo.so runtime_init) */
#define GUEST_FILE_IMAGE 0x1e61400ULL
#define GUEST_IMAGE_SIZE 0x25a8000ULL
#define GUEST_RO_END     0x1c4e000ULL
#define GUEST_RELA       0xbf2080ULL
#define GUEST_RELASZ     0x456c78ULL
#define GUEST_JMPREL     0x1048cf8ULL
#define GUEST_PLTRELSZ   0x3be8ULL
#define GUEST_DYNSYM     0x118e108ULL
#define GUEST_DYNSTR     0x13a49f0ULL
#define GUEST_INIT_ARRAY (GUEST_BASE + 0x1e60e48ULL)
#define GUEST_INIT_ARRAY_SIZE 0x5b8ULL
#define GUEST_TEXT_SIZE  0xbf1838ULL

/* Exact symbol offsets in main.elf (verified via readelf -s main.elf & libsmo.so) */
#define GAME_SYM_malloc        (GUEST_BASE + 0x724b94ULL)
#define GAME_SYM_free          (GUEST_BASE + 0x724becULL)
#define GAME_SYM_calloc        (GUEST_BASE + 0x724c08ULL)
#define GAME_SYM_realloc       (GUEST_BASE + 0x724c80ULL)
#define GAME_SYM_aligned_alloc (GUEST_BASE + 0x724ce0ULL)
#define GAME_SYM_nninitStartup (GUEST_BASE + 0x536f34ULL)
#define GAME_SYM_nnMain        (GUEST_BASE + 0x536f90ULL)
#define GAME_SYM__ZdlPv        (GUEST_BASE + 0x72d198ULL)

#define GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E                        (GUEST_BASE + 0x72e61cULL)
#define GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj      (GUEST_BASE + 0x7764a4ULL)
#define GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_ (GUEST_BASE + 0x7765c4ULL)

#define GAME_VSYM_StageScene_init        (GUEST_BASE + 0x4c861cULL)
#define GAME_FN_ChangeStageInfo_ctor     (GUEST_BASE + 0x248cc0ULL)
#define GAME_FN_tryChangeNextStage       (GUEST_BASE + 0x527a44ULL)
#define GAME_FN_updatePoseTrans          (GUEST_BASE + 0x8ee6acULL)
#define GAME_FN_setVelocityZero          (GUEST_BASE + 0x8e2c68ULL)

/* Exact ELF layout of sdk.elf (verified via readelf -l sdk.elf & libsmo.so register_sdklift_imports) */
#define SDK_IMAGE_SIZE_GEN 0xa11000ULL
#define SDK_TEXT_SIZE_GEN  0x3de8b8ULL

#endif /* IMAGE_H */
