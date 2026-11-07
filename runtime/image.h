#ifndef IMAGE_H
#define IMAGE_H

#define GAME_VERSION "1.4.1"
#define SDK_VERSION 0x00040300u
#define GAME_ELF_DEFAULT "main.elf"
#define GAME_BUILD_ID "3947b198fa0d947f"
#define GUEST_FILE_IMAGE 0x1f00000ULL
#define GUEST_IMAGE_SIZE 0x2400000ULL
#define GUEST_RO_END 0x1800000ULL
#define GUEST_RELA 0x1810000ULL
#define GUEST_RELASZ 0x1000ULL
#define GUEST_JMPREL 0x1811000ULL
#define GUEST_PLTRELSZ 0x1000ULL
#define GUEST_DYNSYM 0x1812000ULL
#define GUEST_DYNSTR 0x1814000ULL
#define GUEST_INIT_ARRAY (GUEST_BASE + 0x1816000ULL)
#define GUEST_INIT_ARRAY_SIZE 0x100ULL
#define GUEST_TEXT_SIZE 0x1400000ULL

#define GAME_SYM_malloc (GUEST_BASE + 0x1000ULL)
#define GAME_SYM_free (GUEST_BASE + 0x1100ULL)
#define GAME_SYM_calloc (GUEST_BASE + 0x1200ULL)
#define GAME_SYM_realloc (GUEST_BASE + 0x1300ULL)
#define GAME_SYM_aligned_alloc (GUEST_BASE + 0x1400ULL)
#define GAME_SYM_nninitStartup (GUEST_BASE + 0x1500ULL)
#define GAME_SYM_nnMain (GUEST_BASE + 0x1600ULL)
#define GAME_SYM__ZdlPv (GUEST_BASE + 0x1700ULL)

#define GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_ 0ULL

#define GAME_VSYM_StageScene_init 0ULL

#define GAME_FN_ChangeStageInfo_ctor 0ULL
#define GAME_FN_tryChangeNextStage 0ULL
#define GAME_FN_updatePoseTrans 0ULL
#define GAME_FN_setVelocityZero 0ULL

#define SDK_IMAGE_SIZE_GEN 0xcfb000ULL
#define SDK_TEXT_SIZE_GEN 0x800000ULL

#endif /* IMAGE_H */
