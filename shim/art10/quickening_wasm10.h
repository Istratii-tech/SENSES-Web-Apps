// The original indices of the instructions arc-wasm quickens at run time
// (ClassVerifier::VerifyClass, patches/aosp10/art10.patch, topic art10). On a
// device dex2oat quickens and keeps them in the vdex (ArtMethod::
// GetQuickenedInfo), which the verifier, NullPointerException messages and
// FindLocksAtDexPc read back through ArtMethod::GetIndexFromQuickening;
// ART 8.1 inferred the field from register types instead. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_QUICKENING_WASM10_H_
#define ARC_WASM_SHIM_ART10_QUICKENING_WASM10_H_

#include <stddef.h>
#include <stdint.h>

namespace art {

// The quickened instruction at |insn| had index |index| (a field index).
void ArtWasmRecordQuickening(const uint16_t* insn, uint16_t index);

// The index recorded for |insn|, or 0xffff (DexFile::kDexNoIndex16).
uint16_t ArtWasmQuickeningIndex(const uint16_t* insn);

}  // namespace art

#endif  // ARC_WASM_SHIM_ART10_QUICKENING_WASM10_H_
