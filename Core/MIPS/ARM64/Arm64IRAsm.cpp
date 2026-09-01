// Copyright (c) 2023- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"
// In other words, PPSSPP_ARCH(ARM64) || DISASM_ALL.
#if PPSSPP_ARCH(ARM64) || (PPSSPP_PLATFORM(WINDOWS) && !defined(__LIBRETRO__))

#include "Common/Log.h"
#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "Core/MIPS/ARM64/Arm64IRJit.h"
#include "Core/MIPS/StvDestinoSalto.h"
#include "Core/MIPS/ARM64/Arm64IRRegCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitState.h"
#include "Core/Core.h"

namespace MIPSComp {

using namespace Arm64Gen;
using namespace Arm64IRJitConstants;

static const bool enableDebug = false;
static const bool enableDisasm = false;

static void ShowPC(void *membase, void *jitbase) {
	static int count = 0;
	if (currentMIPS) {
		u32 downcount = currentMIPS->downcount;
		ERROR_LOG(Log::JIT, "[%08x] ShowPC  Downcount : %08x %d %p %p", currentMIPS->pc, downcount, count, membase, jitbase);
	} else {
		ERROR_LOG(Log::JIT, "Universe corrupt?");
	}
	//if (count > 2000)
	//	exit(0);
	count++;
}

void Arm64JitBackend::GenerateFixedCode(MIPSState *mipsState) {
	// This will be used as a writable scratch area, always 32-bit accessible.
	const u8 *start = AlignCodePage();
	if (DebugProfilerEnabled()) {
		ProtectMemoryPages(start, GetMemoryProtectPageSize(), MEM_PROT_READ | MEM_PROT_WRITE);
		hooks_.profilerPC = (uint32_t *)GetWritableCodePtr();
		Write32(0);
		hooks_.profilerStatus = (IRProfilerStatus *)GetWritableCodePtr();
		Write32(0);
	}

	const u8 *disasmStart = AlignCodePage();
	BeginWrite(GetMemoryProtectPageSize());

	if (jo.useStaticAlloc) {
		saveStaticRegisters_ = AlignCode16();
		STR(INDEX_UNSIGNED, DOWNCOUNTREG, CTXREG, offsetof(MIPSState, downcount));
		regs_.EmitSaveStaticRegisters();
		RET();

		loadStaticRegisters_ = AlignCode16();
		regs_.EmitLoadStaticRegisters();
		LDR(INDEX_UNSIGNED, DOWNCOUNTREG, CTXREG, offsetof(MIPSState, downcount));
		RET();
	} else {
		saveStaticRegisters_ = nullptr;
		loadStaticRegisters_ = nullptr;
	}

	restoreRoundingMode_ = AlignCode16();
	{
		MRS(SCRATCH2_64, FIELD_FPCR);
		// We are not in flush-to-zero mode outside the JIT, so let's turn it off.
		uint32_t mask = ~(4 << 22);
		// Assume we're always in round-to-nearest mode beforehand.
		mask &= ~(3 << 22);
		ANDI2R(SCRATCH2, SCRATCH2, mask);
		_MSR(FIELD_FPCR, SCRATCH2_64);
		RET();
	}

	applyRoundingMode_ = AlignCode16();
	{
		LDR(INDEX_UNSIGNED, SCRATCH1, CTXREG, offsetof(MIPSState, fcr31));
		ANDI2R(SCRATCH2, SCRATCH1, 3);
		FixupBranch skip1 = TBZ(SCRATCH1, 24);
		ADDI2R(SCRATCH2, SCRATCH2, 4);
		SetJumpTarget(skip1);

		// We can skip if the rounding mode is nearest (0) and flush is not set.
		// (as restoreRoundingMode cleared it out anyway)
		FixupBranch skip = CBZ(SCRATCH2);

		// MIPS Rounding Mode:       ARM Rounding Mode
		//   0: Round nearest        0
		//   1: Round to zero        3
		//   2: Round up (ceil)      1
		//   3: Round down (floor)   2
		ANDI2R(SCRATCH1, SCRATCH2, 3);
		CMPI2R(SCRATCH1, 1);

		FixupBranch skipadd = B(CC_NEQ);
		ADDI2R(SCRATCH2, SCRATCH2, 2);
		SetJumpTarget(skipadd);
		FixupBranch skipsub = B(CC_LE);
		SUBI2R(SCRATCH2, SCRATCH2, 1);
		SetJumpTarget(skipsub);

		// Actually change the system FPCR register
		MRS(SCRATCH1_64, FIELD_FPCR);
		// Clear both flush-to-zero and rounding before re-setting them.
		ANDI2R(SCRATCH1, SCRATCH1, ~((4 | 3) << 22));
		ORR(SCRATCH1, SCRATCH1, SCRATCH2, ArithOption(SCRATCH2, ST_LSL, 22));
		_MSR(FIELD_FPCR, SCRATCH1_64);

		SetJumpTarget(skip);
		RET();
	}

	updateRoundingMode_ = AlignCode16();
	{
		LDR(INDEX_UNSIGNED, SCRATCH1, CTXREG, offsetof(MIPSState, fcr31));

		// Set SCRATCH2 to FZ:RM (FZ is bit 24, and RM are lowest 2 bits.)
		ANDI2R(SCRATCH2, SCRATCH1, 3);
		FixupBranch skip = TBZ(SCRATCH1, 24);
		ADDI2R(SCRATCH2, SCRATCH2, 4);
		SetJumpTarget(skip);

		// Update currentRoundingFunc_ with the right convertS0ToSCRATCH1_ func.
		MOVP2R(SCRATCH1_64, convertS0ToSCRATCH1_);
		LSL(SCRATCH2, SCRATCH2, 3);
		LDR(SCRATCH2_64, SCRATCH1_64, SCRATCH2);
		MOVP2R(SCRATCH1_64, &currentRoundingFunc_);
		STR(INDEX_UNSIGNED, SCRATCH2_64, SCRATCH1_64, 0);
		RET();
	}

	hooks_.enterDispatcher = (IRNativeFuncNoArg)AlignCode16();

	uint32_t regs_to_save = Arm64Gen::ALL_CALLEE_SAVED;
	uint32_t regs_to_save_fp = Arm64Gen::ALL_CALLEE_SAVED_FP;
	fp_.ABI_PushRegisters(regs_to_save, regs_to_save_fp);

	// Fixed registers, these are always kept when in Jit context.
	MOVP2R(MEMBASEREG, Memory::base);
	MOVP2R(CTXREG, mipsState);
	// Pre-subtract this to save time later.
	MOVI2R(JITBASEREG, (intptr_t)GetBasePtr() - MIPS_EMUHACK_OPCODE);

	LoadStaticRegisters();
	WriteDebugProfilerStatus(IRProfilerStatus::IN_JIT);
	MovFromPC(SCRATCH1);
	WriteDebugPC(SCRATCH1);
	outerLoopPCInSCRATCH1_ = GetCodePtr();
	MovToPC(SCRATCH1);
	outerLoop_ = GetCodePtr();
		SaveStaticRegisters();  // Advance can change the downcount, so must save/restore
		RestoreRoundingMode(true);
		WriteDebugProfilerStatus(IRProfilerStatus::TIMER_ADVANCE);
		QuickCallFunction(SCRATCH1_64, &CoreTiming::Advance);
		WriteDebugProfilerStatus(IRProfilerStatus::IN_JIT);
		ApplyRoundingMode(true);
		LoadStaticRegisters();

		dispatcherCheckCoreState_ = GetCodePtr();

		MOVP2R(SCRATCH1_64, &coreState);
		LDR(INDEX_UNSIGNED, SCRATCH1, SCRATCH1_64, 0);
		FixupBranch badCoreState = CBNZ(SCRATCH1);

		// Check downcount.
		TBNZ(DOWNCOUNTREG, 31, outerLoop_);
		FixupBranch skipToRealDispatch = B();

		dispatcherPCInSCRATCH1_ = GetCodePtr();
		MovToPC(SCRATCH1);

		hooks_.dispatcher = GetCodePtr();

			FixupBranch bail = TBNZ(DOWNCOUNTREG, 31);
			SetJumpTarget(skipToRealDispatch);

			dispatcherNoCheck_ = GetCodePtr();

			// Debug
			if (enableDebug) {
				MOV(W0, DOWNCOUNTREG);
				MOV(X1, MEMBASEREG);
				MOV(X2, JITBASEREG);
				QuickCallFunction(SCRATCH1_64, &ShowPC);
			}

			MovFromPC(SCRATCH1);
			WriteDebugPC(SCRATCH1);
#ifdef MASKED_PSP_MEMORY
			ANDI2R(SCRATCH1, SCRATCH1, Memory::MEMVIEW32_MASK);
#endif
			// --- via rapida: cache de destinos de salto indirecto (STV) ---
			// La lectura de abajo trae una linea de la RAM EMULADA por cada salto
			// indirecto y no la reusa: medida como el mayor generador de fallos de
			// cache del proceso (3,16 %) y 5,08 % de los ciclos. Aca se consulta
			// primero una tabla compacta pc -> palabra, que si entra en L1. Cuando
			// falla, sigue exactamente el camino de antes. Ver StvDestinoSalto.h.
			const int stvModo = stvjit::ModoCache();
			const int stvBits = stvjit::BitsCache();
			const int stvMezcla = stvjit::MezclaCache();
			if (stvModo > 0) {
				MOV(W10, SCRATCH1);                                  // etiqueta = pc
				if (stvMezcla) {
					// Sin mezclar, el indice son los bits bajos del pc y dos
					// direcciones separadas por (4 << bits) bytes chocan. El
					// codigo del juego ocupa megabytes: eso es una colision cada
					// pocos KB. El XOR con los bits de arriba cuesta una
					// instruccion y reparte por todo el rango.
					LSR(W11, SCRATCH1, 2);
					EOR(W11, W11, SCRATCH1, ArithOption(SCRATCH1, ST_LSR, 2 + stvBits));
					ANDI2R(W11, W11, (1u << stvBits) - 1u);
				} else {
					UBFX(W11, SCRATCH1, 2, stvBits);                 // indice
				}
				MOVP2R(X9, stvjit::g_destinos);
				ADD(X9, X9, X11, ArithOption(X11, ST_LSL, 3));       // &entrada
				LDR(INDEX_UNSIGNED, X12, X9, 0);                     // (palabra<<32)|pc
				CMP(W12, W10);
				FixupBranch stvFallo = B(CC_NEQ);
					if (stvModo >= 2) {
						MOVP2R(X14, &stvjit::g_aciertos);
						LDR(INDEX_UNSIGNED, X15, X14, 0);
						ADD(X15, X15, 1);
						STR(INDEX_UNSIGNED, X15, X14, 0);
					}
					LSR(X13, X12, 32);                               // palabra
					ADD(SCRATCH1_64, JITBASEREG, X13);
					BR(SCRATCH1_64);
				SetJumpTarget(stvFallo);
				if (stvModo >= 2) {
					MOVP2R(X14, &stvjit::g_fallos);
					LDR(INDEX_UNSIGNED, X15, X14, 0);
					ADD(X15, X15, 1);
					STR(INDEX_UNSIGNED, X15, X14, 0);
				}
			}

			hooks_.dispatchFetch = GetCodePtr();
			LDR(SCRATCH1, MEMBASEREG, SCRATCH1_64);
			LSR(SCRATCH2, SCRATCH1, 24);   // or UBFX(SCRATCH2, SCRATCH1, 24, 8)
			// We don't mask SCRATCH1 as that's already baked into JITBASEREG.
			CMP(SCRATCH2, MIPS_EMUHACK_OPCODE >> 24);
			FixupBranch skipJump = B(CC_NEQ);
				// La marca era valida: se recuerda para el proximo salto a este pc.
				// X9 y W10 siguen vivos desde la via rapida (entre medio solo hubo
				// una carga y una comparacion).
				if (stvModo > 0) {
					ORR(X14, X10, SCRATCH1_64, ArithOption(SCRATCH1_64, ST_LSL, 32));
					STR(INDEX_UNSIGNED, X14, X9, 0);
				}
				ADD(SCRATCH1_64, JITBASEREG, SCRATCH1_64);
				BR(SCRATCH1_64);
			SetJumpTarget(skipJump);

			// No block found, let's jit.  We don't need to save static regs, they're all callee saved.
			RestoreRoundingMode(true);
			WriteDebugProfilerStatus(IRProfilerStatus::COMPILING);
			QuickCallFunction(SCRATCH1_64, &MIPSComp::JitAt);
			WriteDebugProfilerStatus(IRProfilerStatus::IN_JIT);
			ApplyRoundingMode(true);

			// Let's just dispatch again, we'll enter the block since we know it's there.
			B(dispatcherNoCheck_);

		SetJumpTarget(bail);

		MOVP2R(SCRATCH1_64, &coreState);
		LDR(INDEX_UNSIGNED, SCRATCH1, SCRATCH1_64, 0);
		CBZ(SCRATCH1, outerLoop_);

	const uint8_t *quitLoop = GetCodePtr();
	SetJumpTarget(badCoreState);

	WriteDebugProfilerStatus(IRProfilerStatus::NOT_RUNNING);
	SaveStaticRegisters();
	RestoreRoundingMode(true);

	fp_.ABI_PopRegisters(regs_to_save, regs_to_save_fp);

	RET();

	// STV: publicar las direcciones del despachador para poder ATRIBUIR el
	// perfil en vez de inferirlo. Aislarlo por concentracion de direcciones es
	// adivinar: el JIT ubica su codigo distinto en cada arranque y las
	// direcciones de un perfil viejo no valen en el proceso nuevo. Con esto,
	// una muestra cae dentro del despachador o no cae, sin interpretacion.
	{
		const uint8_t *finDisp = GetCodePtr();
		char b[240];
		snprintf(b, sizeof(b),
			"STVJIT: base=%p enter=%p disp=%p fetch=%p fin=%p  (despachador %d bytes)",
			(const void *)GetBasePtr(), (const void *)hooks_.enterDispatcher,
			(const void *)hooks_.dispatcher, (const void *)hooks_.dispatchFetch,
			(const void *)finDisp, (int)(finDisp - (const uint8_t *)hooks_.dispatcher));
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "STV", "%s", b);
#else
		printf("%s\n", b);
#endif
	}

	hooks_.crashHandler = GetCodePtr();
	MOVP2R(SCRATCH1_64, &coreState);
	MOVI2R(SCRATCH2, CORE_RUNTIME_ERROR);
	STR(INDEX_UNSIGNED, SCRATCH2, SCRATCH1_64, 0);
	B(quitLoop);

	// Generate some integer conversion funcs.
	// MIPS order!
	static const RoundingMode roundModes[8] = { ROUND_N, ROUND_Z, ROUND_P, ROUND_M, ROUND_N, ROUND_Z, ROUND_P, ROUND_M };
	for (size_t i = 0; i < ARRAY_SIZE(roundModes); ++i) {
		convertS0ToSCRATCH1_[i] = AlignCode16();

		// Invert 0x80000000 -> 0x7FFFFFFF for the NAN result.
		fp_.MVNI(32, EncodeRegToDouble(SCRATCHF2), 0x80, 24);
		fp_.FCMP(S0, S0);  // Detect NaN
		fp_.FCVTS(S0, S0, roundModes[i]);
		fp_.FCSEL(S0, S0, SCRATCHF2, CC_VC);

		RET();
	}

	// Leave this at the end, add more stuff above.
	if (enableDisasm) {
		std::vector<std::string> lines = DisassembleArm64(disasmStart, (int)(GetCodePtr() - disasmStart));
		for (auto s : lines) {
			INFO_LOG(Log::JIT, "%s", s.c_str());
		}
	}

	// Let's spare the pre-generated code from unprotect-reprotect.
	AlignCodePage();
	jitStartOffset_ = (int)(GetCodePtr() - start);
	// Don't forget to zap the instruction cache! This must stay at the end of this function.
	FlushIcache();
	EndWrite();

	// Update our current cached rounding mode func, too.
	UpdateFCR31(mipsState);
}

} // namespace MIPSComp

#endif
