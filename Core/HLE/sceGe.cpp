// Copyright (c) 2012- PPSSPP Project.

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

#include <map>
#include <vector>
#include <mutex>

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeList.h"
#include "Common/Serialize/SerializeMap.h"
#include "Common/Data/Collections/ThreadSafeList.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/MIPS/MIPS.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/MemMapHelpers.h"
#include "Core/Reporting.h"
#include "Core/System.h"
#include "Core/HLE/sceGe.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/KernelWaitHelpers.h"
#include "GPU/GPUState.h"
#include "GPU/GPUCommon.h"
#include <optional>

#include "Common/StvMedidor.h"
#include "GPU/Common/StvGeThread.h"  // STV_GE_THREAD_v1

static const int LIST_ID_MAGIC = 0x35000000;

// STV_GE_THREAD_v1: el punto unico por el que se continua la cola de listas.
// Reemplaza a los gpu->ProcessDLQueue() sueltos de las syscalls y del handler
// de interrupcion: nivel 0 ejecuta inline identico a upstream; nivel 1 postea
// la orden al worker y espera la terminacion aca mismo; nivel 2 postea y
// vuelve. El camino hleSplitSyscallOverGe es de debugger (ShouldSplitOverGe ==
// NeedsSlowInterpreter() || breakpoints): se conserva tal cual, con una
// barrera previa por si la palanca del debugger se prendio con una pasada del
// worker en vuelo — el split ejecuta ProcessDLQueue en el EmuThread via
// CORE_RUNNING_GE y no puede convivir con el worker.
static void StvGeDespacharCola(bool permitirSplit) {
	// SIN guarda fina A PROPOSITO: esta funcion no toca el estado protegido
	// (solo despacha), y sostener el fino aca seria mortal — Barrera() ESPERA a
	// que el worker termine, y el worker necesita el fino para terminar.
	// El inventario me la marco por error de atribucion; verificado con awk
	// sobre el cuerpo real: 0 accesos a ge_pending_cb.
	if (permitirSplit && gpu->ShouldSplitOverGe()) {
		stvge::Barrera();
		hleSplitSyscallOverGe();
		return;
	}
	stvge::DespacharProcessDLQueue();
}

static PspGeCallbackData ge_callback_data[16];
static bool ge_used_callbacks[16] = {0};

typedef std::vector<SceUID> WaitingThreadList;
static std::map<int, WaitingThreadList> listWaitingThreads;
static WaitingThreadList drawWaitingThreads;

struct GeInterruptData {
	int listid;
	u32 pc;
	u32 cmd;
};

static ThreadSafeList<GeInterruptData> ge_pending_cb;
static int geSyncEvent;
static int geInterruptEvent;
static int geCycleEvent;

class GeIntrHandler : public IntrHandler {
public:
	GeIntrHandler() : IntrHandler(PSP_GE_INTR) {}

	bool run(PendingInterrupt& pend) override {
		// STV_GE_THREAD_v1: este cuerpo lee y ESCRIBE dls[] (dl->state =
		// COMPLETED) mientras el worker puede estar corriendo otra lista:
		// candado hasta antes del despacho final, que se suelta explicitamente
		// porque el despacho en nivel 1 espera al worker (deadlock si lo
		// tuvieramos tomado). Los returns tempranos lo sueltan por RAII.
		// STV_MEDIDOR: esta toma NO pasa por CandadoGe, asi que el medidor no la
		// veia y su costo aparecia como "resto" — el campo que existe justo para
		// declarar lo que no se cronometra. La traza off-CPU la señalo como EL
		// sitio de bloqueo mas grande del EmuThread (21,9 % de su tiempo dormido,
		// contra 1,24 % de todas las tomas instrumentadas juntas). Se cronometra
		// con la misma ranura que el resto del camino de interrupciones.
		// LA GANANCIA: con el candado fino activo NO se toma el grueso, asi que
		// este hilo deja de esperar la pasada del worker (2,45 ms) y espera solo
		// las fronteras (microsegundos). Con la valvula apagada se toman los dos
		// y el comportamiento es el de hoy.
		std::optional<stvge::RastreoCandado> rastroVivo;
		std::unique_lock<std::recursive_mutex> candadoGe(stvge::g_mu, std::defer_lock);
		if (stvge::NivelActivo() != 0 && !stvge::DLFinoActivo()) {
			stvge::RastreoCandado rastro(stvge::kCandGe, "GeIntrHandler");
			stvmed::Cronometro c(stvmed::R_CAND_INTERRUPT);
			candadoGe.lock();
			rastroVivo.emplace(stvge::kCandGe, "GeIntrHandler-vivo");
		}
		// Zona de contabilidad de listas: el testigo cuenta entradas y, sobre
		// todo, ENTRADAS CONCURRENTES. Con el candado grueso debe dar 0.
		stvge::CandadoDL testigoDL("GeIntr::run");

		if (ge_pending_cb.empty()) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to run GE interrupt: no pending interrupt");
			return false;
		}

		GeInterruptData intrdata = ge_pending_cb.front();
		DisplayList* dl = gpu->getList(intrdata.listid);

		if (dl == NULL) {
			WARN_LOG(Log::sceGe, "Unable to run GE interrupt: list doesn't exist: %d", intrdata.listid);
			return false;
		}

		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to run GE interrupt: list has interrupts disabled, should not happen");
			return false;
		}

		gpu->InterruptStart(intrdata.listid);

		const u32 cmd = intrdata.cmd;
		int subintr = -1;
		if (dl->subIntrBase >= 0) {
			switch (dl->signal) {
			case PSP_GE_SIGNAL_SYNC:
			case PSP_GE_SIGNAL_JUMP:
			case PSP_GE_SIGNAL_CALL:
			case PSP_GE_SIGNAL_RET:
				// Do nothing.
				break;

			case PSP_GE_SIGNAL_HANDLER_PAUSE:
				if (cmd == GE_CMD_FINISH)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				break;

			default:
				if (cmd == GE_CMD_SIGNAL)
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
				else
					subintr = dl->subIntrBase | PSP_GE_SUBINTR_FINISH;
				break;
			}
		}

		// Set the list as complete once the interrupt starts.
		// In other words, not before another interrupt finishes.
		if (dl->signal != PSP_GE_SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH) {
			dl->state = PSP_GE_DL_STATE_COMPLETED;
		}

		SubIntrHandler* handler = get(subintr);
		if (handler != NULL) {
			DEBUG_LOG(Log::CPU, "Entering GE interrupt handler %08x", handler->handlerAddress);
			currentMIPS->pc = handler->handlerAddress;
			u32 data = dl->subIntrToken;
			currentMIPS->r[MIPS_REG_A0] = data & 0xFFFF;
			currentMIPS->r[MIPS_REG_A1] = handler->handlerArg;
			currentMIPS->r[MIPS_REG_A2] = sceKernelGetCompiledSdkVersion() <= 0x02000010 ? 0 : intrdata.pc + 4;
			// RA is already taken care of in __RunOnePendingInterrupt

			return true;
		}

		if (dl->signal == PSP_GE_SIGNAL_HANDLER_SUSPEND) {
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
		}

		ge_pending_cb.pop_front();
		// EL CUARTO CICLO, cazado por el rastreador de orden de candados:
		// InterruptEnd puede necesitar el candado GRUESO (su rama rara), y este
		// manejador ya tiene el FINO tomado -> g_muDL -> g_mu, contra el
		// g_mu -> g_muDL del worker. Se suelta el fino ANTES de llamarla: la
		// contabilidad de esta funcion ya esta hecha a esta altura, y
		// InterruptEnd toma por su cuenta lo que necesite, en el orden correcto.
		testigoDL.soltar();
		gpu->InterruptEnd(intrdata.listid);
		// Seen in GoW.
		if (subintr >= 0)
			DEBUG_LOG(Log::sceGe, "Ignoring interrupt for display list %d, already been released.", intrdata.listid);

		// Hm. This might be really tricky to get to behave the same in both modes. Here we are in __KernelReschedule, CoreTiming::Advance, ProcessEvents, GeExecuteInterrupt, ... .... __RunOnePendingInterrupt
		// But not sure how much it will matter. The test pause2 hits here.
		// STV_GE_THREAD_v1: upstream NO usa el split aca (rompe pause2, ver el
		// comentario de handleResult); permitirSplit=false lo preserva. El
		// candado se suelta ANTES: el despacho puede esperar al worker.
		if (candadoGe.owns_lock())
			testigoDL.soltar();   // el fino se suelta DONDE se suelta el grueso:
			candadoGe.unlock();   // el despacho final espera al worker
		StvGeDespacharCola(false);
		return false;
	}

	void handleResult(PendingInterrupt& pend) override {
		// STV_GE_THREAD_v1: mismo esquema que run() — candado sobre el estado,
		// soltado antes del despacho final.
		// LA GANANCIA: con el candado fino activo NO se toma el grueso, asi que
		// este hilo deja de esperar la pasada del worker (2,45 ms) y espera solo
		// las fronteras (microsegundos). Con la valvula apagada se toman los dos
		// y el comportamiento es el de hoy.
		std::optional<stvge::RastreoCandado> rastroVivo;
		std::unique_lock<std::recursive_mutex> candadoGe(stvge::g_mu, std::defer_lock);
		if (stvge::NivelActivo() != 0 && !stvge::DLFinoActivo()) {
			stvmed::Cronometro c(stvmed::R_CAND_INTERRUPT);   // STV_MEDIDOR: ver run()
			candadoGe.lock();
		}
		stvge::CandadoDL testigoDL("GeIntr::handleResult");   // ver run()

		GeInterruptData intrdata = ge_pending_cb.front();
		ge_pending_cb.pop_front();

		DisplayList* dl = gpu->getList(intrdata.listid);
		if (!dl->interruptsEnabled) {
			ERROR_LOG_REPORT(Log::sceGe, "Unable to finish GE interrupt: list has interrupts disabled, should not happen");
			return;
		}

		switch (dl->signal) {
		case PSP_GE_SIGNAL_HANDLER_SUSPEND:
			if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
				// uofw says dl->state = endCmd & 0xFF;
				DisplayListState newState = static_cast<DisplayListState>(Memory::ReadUnchecked_U32(intrdata.pc - 4) & 0xFF);
				//dl->status = static_cast<DisplayListStatus>(Memory::ReadUnchecked_U32(intrdata.pc) & 0xFF);
				//if(dl->status < 0 || dl->status > PSP_GE_LIST_PAUSED)
				//	ERROR_LOG(Log::sceGe, "Weird DL status after signal suspend %x", dl->status);
				if (newState != PSP_GE_DL_STATE_RUNNING) {
					DEBUG_LOG_REPORT(Log::sceGe, "GE Interrupt: newState might be %d", newState);
				}

				if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
					dl->state = PSP_GE_DL_STATE_QUEUED;
				}
			}
			break;
		default:
			break;
		}

		// EL CUARTO CICLO, cazado por el rastreador de orden de candados:
		// InterruptEnd puede necesitar el candado GRUESO (su rama rara), y este
		// manejador ya tiene el FINO tomado -> g_muDL -> g_mu, contra el
		// g_mu -> g_muDL del worker. Se suelta el fino ANTES de llamarla: la
		// contabilidad de esta funcion ya esta hecha a esta altura, y
		// InterruptEnd toma por su cuenta lo que necesite, en el orden correcto.
		testigoDL.soltar();
		gpu->InterruptEnd(intrdata.listid);

		// TODO: This is called from __KernelReturnFromInterrupt which does a bunch of stuff afterwards.
		// Using hleSplitSyscallOverGe here breaks the gpu/signals/suspend.prx test, for that reason.
		// However, it's still useful when debugging, and I believe the scheduling difference is quite insignificant -
		// we are already being extremely inaccurate by blasting the full display list here instead of running
		// it in the background in parallel with the CPU.
		// So, when debugging is active, we'll just use hleSplitSyscallOverGe.
		if (candadoGe.owns_lock())
			testigoDL.soltar();   // el fino se suelta DONDE se suelta el grueso:
			candadoGe.unlock();   // el despacho final espera al worker
		StvGeDespacharCola(true);  // STV_GE_THREAD_v1
	}
};

static void __GeExecuteSync(u64 userdata, int cyclesLate) {
	int listid = userdata >> 32;
	GPUSyncType type = (GPUSyncType) (userdata & 0xFFFFFFFF);
	bool wokeThreads = __GeTriggerWait(type, listid);
	gpu->SyncEnd(type, listid, wokeThreads);
}

static void __GeExecuteInterrupt(u64 userdata, int cyclesLate) {
	__TriggerInterrupt(PSP_INTR_IMMEDIATE, PSP_GE_INTR, PSP_INTR_SUB_NONE);
}

static void __GeCheckCycles(u64 userdata, int cyclesLate) {
	// Deprecated
}

void __GeInit() {
	memset(&ge_used_callbacks, 0, sizeof(ge_used_callbacks));
	memset(&ge_callback_data, 0, sizeof(ge_callback_data));
	ge_pending_cb.clear();
	__RegisterIntrHandler(PSP_GE_INTR, new GeIntrHandler());

	geSyncEvent = CoreTiming::RegisterEvent("GeSyncEvent", &__GeExecuteSync);
	geInterruptEvent = CoreTiming::RegisterEvent("GeInterruptEvent", &__GeExecuteInterrupt);

	// Deprecated
	geCycleEvent = CoreTiming::RegisterEvent("GeCycleEvent", &__GeCheckCycles);

	listWaitingThreads.clear();
	drawWaitingThreads.clear();
}

struct GeInterruptData_v1 {
	int listid;
	u32 pc;
};

void __GeDoState(PointerWrap &p) {
	auto s = p.Section("sceGe", 1, 2);
	if (!s)
		return;

	DoArray(p, ge_callback_data, ARRAY_SIZE(ge_callback_data));
	DoArray(p, ge_used_callbacks, ARRAY_SIZE(ge_used_callbacks));

	if (s >= 2) {
		Do(p, ge_pending_cb);
	} else {
		std::list<GeInterruptData_v1> old;
		Do(p, old);
		ge_pending_cb.clear();
		for (const auto &ge : old) {
			GeInterruptData intrdata = {ge.listid, ge.pc};
			intrdata.cmd = Memory::ReadUnchecked_U32(ge.pc - 4) >> 24;
			ge_pending_cb.push_back(intrdata);
		}
	}

	Do(p, geSyncEvent);
	CoreTiming::RestoreRegisterEvent(geSyncEvent, "GeSyncEvent", &__GeExecuteSync);
	Do(p, geInterruptEvent);
	CoreTiming::RestoreRegisterEvent(geInterruptEvent, "GeInterruptEvent", &__GeExecuteInterrupt);
	Do(p, geCycleEvent);
	CoreTiming::RestoreRegisterEvent(geCycleEvent, "GeCycleEvent", &__GeCheckCycles);

	Do(p, listWaitingThreads);
	Do(p, drawWaitingThreads);

	// Everything else is done in sceDisplay.
}

void __GeShutdown() {
}

bool __GeTriggerSync(GPUSyncType type, int id, u64 atTicks) {
	// STV_GE_THREAD_v1: en el worker no se toca CoreTiming (no es thread-safe
	// y es territorio del EmuThread). Se postea el descriptor con los MISMOS
	// argumentos y el drenaje ejecuta este mismo cuerpo en el EmuThread. El
	// true replica el retorno del camino inline (nadie lo consume aca).
	if (stvge::EnWorker()) {
		stvge::PostearSync(type, id, atTicks);
		return true;
	}
	u64 userdata = (u64)id << 32 | (u64)type;
	s64 future = atTicks - CoreTiming::GetTicks();
	if (type == GPU_SYNC_DRAW) {
		s64 left = CoreTiming::UnscheduleEvent(geSyncEvent, userdata);
		if (left > future)
			future = left;
	}
	CoreTiming::ScheduleEvent(future, geSyncEvent, userdata);
	return true;
}

bool __GeTriggerInterrupt(int listid, u32 pc, u64 atTicks) {
	stvge::CandadoDL zonaDL("__GeTriggerInterrupt");   // contabilidad de listas
	// STV_GE_THREAD_v1: idem __GeTriggerSync. En v1.20.4 esta funcion devuelve
	// true INCONDICIONALMENTE (verificado: no hay camino que devuelva false),
	// asi que Execute_End puede tomar su rama pendingInterrupt en el worker
	// sin esperar al drenaje: la decision no depende del kernel. Nota: el cmd
	// se lee de memoria emulada (pc - 4) recien al drenar; un juego que
	// reescriba su lista bajo una interrupcion pendiente veria la palabra
	// nueva, igual que en la PSP real donde la interrupcion tambien llega
	// despues de ejecutada la lista.
	if (stvge::EnWorker()) {
		stvge::PostearInterrupt(listid, pc, atTicks);
		return true;
	}
	GeInterruptData intrdata;
	intrdata.listid = listid;
	intrdata.pc = pc;
	intrdata.cmd = Memory::ReadUnchecked_U32(pc - 4) >> 24;

	ge_pending_cb.push_back(intrdata);

	u64 userdata = (u64)listid << 32 | (u64) pc;
	CoreTiming::ScheduleEvent(atTicks - CoreTiming::GetTicks(), geInterruptEvent, userdata);
	return true;
}

void __GeWaitCurrentThread(GPUSyncType type, SceUID waitId, const char *reason) {
	WaitType waitType;
	if (type == GPU_SYNC_DRAW) {
		drawWaitingThreads.push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GEDRAWSYNC;
	} else if (type == GPU_SYNC_LIST) {
		listWaitingThreads[waitId].push_back(__KernelGetCurThread());
		waitType = WAITTYPE_GELISTSYNC;
	} else {
		ERROR_LOG_REPORT(Log::sceGe, "__GeWaitCurrentThread: bad wait type");
		return;
	}

	__KernelWaitCurThread(waitType, waitId, 0, 0, false, reason);
}

static bool __GeTriggerWait(WaitType waitType, SceUID waitId, WaitingThreadList &waitingThreads) {
	// TODO: Do they ever get a result other than 0?
	bool wokeThreads = false;
	for (int threadID : waitingThreads)
		wokeThreads |= HLEKernel::ResumeFromWait(threadID, waitType, waitId, 0);
	waitingThreads.clear();
	return wokeThreads;
}

bool __GeTriggerWait(GPUSyncType type, SceUID waitId) {
	// We check for the old type for old savestate compatibility.
	if (type == GPU_SYNC_DRAW || (WaitType)type == WAITTYPE_GEDRAWSYNC)
		return __GeTriggerWait(WAITTYPE_GEDRAWSYNC, waitId, drawWaitingThreads);
	else if (type == GPU_SYNC_LIST || (WaitType)type == WAITTYPE_GELISTSYNC)
		return __GeTriggerWait(WAITTYPE_GELISTSYNC, waitId, listWaitingThreads[waitId]);
	else
		ERROR_LOG_REPORT(Log::sceGe, "__GeTriggerWait: bad wait type");
	return false;
}

// Some games spam this, like MediEvil.
static u32 sceGeEdramGetAddr() {
	u32 retVal = 0x04000000;
	hleEatCycles(150);
	return hleLogVerbose(Log::sceGe, retVal);
}

// TODO: Return a different value for the PS3 enhanced-emulator games?
static u32 sceGeEdramGetSize() {
	const u32 retVal = 0x00200000;
	return hleLogVerbose(Log::sceGe, retVal);
}

static int __GeSubIntrBase(int callbackId) {
	return callbackId * 2;
}

u32 sceGeListEnQueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	bool runList;
	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, false, &runList);
	if ((int)listID >= 0)
		listID = LIST_ID_MAGIC ^ listID;
	if (runList) {
		StvGeDespacharCola(true);  // STV_GE_THREAD_v1
	}
	hleEatCycles(490);
	hleCoreTimingForceCheck();
	DEBUG_LOG(Log::sceGe,
		"%08x=sceGeListEnQueue(addr=%08x, stall=%08x, cbid=%08x, param=%08x) ticks=%lld", listID,
		listAddress, stallAddress, callbackId, optParamAddr, (long long)CoreTiming::GetTicks());
	return hleNoLog(listID); // We already logged above, logs get confusing if we use hleLogSuccess.
}

u32 sceGeListEnQueueHead(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	DEBUG_LOG(Log::sceGe,
		"sceGeListEnQueueHead(addr=%08x, stall=%08x, cbid=%08x, param=%08x) ticks=%lld",
		listAddress, stallAddress, callbackId, optParamAddr, (long long)CoreTiming::GetTicks());
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);

	bool runList;
	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, true, &runList);
	if ((int)listID >= 0)
		listID = LIST_ID_MAGIC ^ listID;
	if (runList) {
		StvGeDespacharCola(true);  // STV_GE_THREAD_v1
	}
	hleEatCycles(480);
	hleCoreTimingForceCheck();
	return hleNoLog(listID); // We already logged above, logs get confusing if we use hleLogSuccess.
}

static int sceGeListDeQueue(u32 listID) {
	WARN_LOG(Log::sceGe, "sceGeListDeQueue(%08x)", listID);
	int result = gpu->DequeueList(LIST_ID_MAGIC ^ listID);
	hleReSchedule("dlist dequeued");
	return hleNoLog(result);
}

static int sceGeListUpdateStallAddr(u32 displayListID, u32 stallAddress) {
	// Advance() might cause an interrupt, so defer the Advance but do it ASAP.
	// Final Fantasy Type-0 has a graphical artifact without this (timing issue.)
	hleEatCycles(190);
	hleCoreTimingForceCheck();

	DEBUG_LOG(Log::sceGe, "sceGeListUpdateStallAddr(dlid=%i, stalladdr=%08x)", displayListID, stallAddress);
	bool runList;
	int retval = gpu->UpdateStall(LIST_ID_MAGIC ^ displayListID, stallAddress, &runList);
	if (runList) {
		StvGeDespacharCola(true);  // STV_GE_THREAD_v1
	}
	return hleNoLog(retval);
}

// 0 : wait for completion. 1:check and return
int sceGeListSync(u32 displayListID, u32 mode) {
	hleEatCycles(220);  // Fudged without measuring, copying sceGeContinue.
	return hleLogDebug(Log::sceGe, gpu->ListSync(LIST_ID_MAGIC ^ displayListID, mode));
}

static u32 sceGeDrawSync(u32 mode) {
	//wait/check entire drawing state
	if (PSP_CoreParameter().compat.flags().DrawSyncEatCycles)
		hleEatCycles(500000); //HACK(?) : Potential fix for Crash Tag Team Racing and a few Gundam games
	else if (!PSP_CoreParameter().compat.flags().DrawSyncInstant)
		hleEatCycles(1240);
	return hleLogDebug(Log::sceGe, gpu->DrawSync(mode));
}

static int sceGeContinue() {
	bool runList;
	int ret = gpu->Continue(&runList);
	if (runList) {
		StvGeDespacharCola(true);  // STV_GE_THREAD_v1
	}
	hleEatCycles(220);
	hleReSchedule("ge continue");
	return hleLogDebug(Log::sceGe, ret);
}

static int sceGeBreak(u32 mode, u32 unknownPtr) {
	if (mode > 1) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_MODE, "invalid mode");
	}
	// Not sure what this is supposed to be for...
	if ((int)unknownPtr < 0 || (int)(unknownPtr + 16) < 0) {
		WARN_LOG_REPORT(Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x): invalid ptr", mode, unknownPtr);
		return SCE_KERNEL_ERROR_PRIV_REQUIRED;
	} else if (unknownPtr != 0) {
		WARN_LOG_REPORT_ONCE(gebreak_unkptr, Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x): unknown ptr (%s)", mode, unknownPtr, Memory::IsValidAddress(unknownPtr) ? "valid" : "invalid");
	}

	//mode => 0 : current dlist 1: all drawing
	DEBUG_LOG(Log::sceGe, "sceGeBreak(mode=%d, unknown=%08x)", mode, unknownPtr);
	int result = gpu->Break(mode);
	if (result >= 0 && mode == 0) {
		return hleNoLog(LIST_ID_MAGIC ^ result);
	}
	return hleNoLog(result);
}

static u32 sceGeSetCallback(u32 structAddr) {
	int cbID = -1;
	for (size_t i = 0; i < ARRAY_SIZE(ge_used_callbacks); ++i) {
		if (!ge_used_callbacks[i]) {
			cbID = (int) i;
			break;
		}
	}

	if (cbID == -1) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_OUT_OF_MEMORY, "out of callback ids");
	}

	ge_used_callbacks[cbID] = true;
	auto callbackData = PSPPointer<PspGeCallbackData>::Create(structAddr);
	ge_callback_data[cbID] = *callbackData;
	callbackData.NotifyRead("GeSetCallback");

	int subIntrBase = __GeSubIntrBase(cbID);

	// TODO: Maybe don't ignore return values of the hleCalls?

	if (ge_callback_data[cbID].finish_func != 0) {
		hleCall(InterruptManager, u32, sceKernelRegisterSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH,
				ge_callback_data[cbID].finish_func, ge_callback_data[cbID].finish_arg);
		hleCall(InterruptManager, u32, sceKernelEnableSubIntr, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
	}
	if (ge_callback_data[cbID].signal_func != 0) {
		hleCall(InterruptManager, u32, sceKernelRegisterSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL,
				ge_callback_data[cbID].signal_func, ge_callback_data[cbID].signal_arg);
		hleCall(InterruptManager, u32, sceKernelEnableSubIntr, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	}

	return hleLogDebug(Log::sceGe, cbID);
}

static int sceGeUnsetCallback(u32 cbID) {
	if (cbID >= ARRAY_SIZE(ge_used_callbacks)) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_ID, "invalid callback id");
	}

	if (ge_used_callbacks[cbID]) {
		int subIntrBase = __GeSubIntrBase(cbID);

		// TODO: Maybe don't ignore return values?
		hleCall(InterruptManager, u32, sceKernelReleaseSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_FINISH);
		hleCall(InterruptManager, u32, sceKernelReleaseSubIntrHandler, PSP_GE_INTR, subIntrBase | PSP_GE_SUBINTR_SIGNAL);
	} else {
		WARN_LOG(Log::sceGe, "sceGeUnsetCallback(cbid=%08x): ignoring unregistered callback id", cbID);
	}

	ge_used_callbacks[cbID] = false;
	return hleLogDebug(Log::sceGe, 0);
}

// Points to 512 32-bit words, where we can probably layout the context however we want
// unless some insane game pokes it and relies on it...
u32 sceGeSaveContext(u32 ctxAddr) {
	// STV_GE_THREAD_v1: BusyDrawing toma el candado del GE adentro. Si vuelve
	// false, la cola quedo vacia CON el candado tomado, o sea el worker
	// termino su pasada y no puede arrancar otra sin que ESTE hilo lo
	// despierte: el gstate.Save de abajo no necesita candado propio.
	if (gpu->BusyDrawing()) {
		// Real error code.
		return hleLogWarning(Log::sceGe, -1, "lists in process, aborting");
	}

	// Let's just dump gstate.
	if (Memory::IsValidAddress(ctxAddr)) {
		gstate.Save((u32_le *)Memory::GetPointer(ctxAddr));
	}

	// This action should probably be pushed to the end of the queue of the display thread -
	// when we have one.
	return hleLogDebug(Log::sceGe, 0);
}

u32 sceGeRestoreContext(u32 ctxAddr) {
	// STV_GE_THREAD_v1: mismo razonamiento que sceGeSaveContext — worker
	// probadamente idle tras BusyDrawing()==false; ReapplyGfxState ademas
	// toma el candado por su cuenta.
	if (gpu->BusyDrawing()) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_BUSY, "lists in process, aborting");
	}

	if (Memory::IsValidAddress(ctxAddr)) {
		gstate.Restore((u32_le *)Memory::GetPointer(ctxAddr));
	}

	gpu->ReapplyGfxState();
	return hleLogDebug(Log::sceGe, 0);
}

static int sceGeGetMtx(int type, u32 matrixPtr) {
	int size = type == GE_MTX_PROJECTION ? 16 : 12;
	if (!Memory::IsValidRange(matrixPtr, size * sizeof(float))) {
		return hleLogError(Log::sceGe, -1, "bad matrix ptr");
	}

	u32_le *dest = (u32_le *)Memory::GetPointerWriteUnchecked(matrixPtr);
	// Note: this reads the CPU-visible matrix values, which may differ from the actual used values.
	// They only differ when more DATA commands are sent than are valid for a matrix.
	if (!gpu || !gpu->GetMatrix24(GEMatrixType(type), dest, 0))
		return hleLogError(Log::sceGe, SCE_KERNEL_ERROR_INVALID_INDEX, "invalid matrix");

	return hleLogInfo(Log::sceGe, 0);
}

static u32 sceGeGetCmd(int cmd) {
	if (cmd >= 0 && cmd < (int)ARRAY_SIZE(gstate.cmdmem)) {
		// Does not mask away the high bits.  But matrix regs don't read back.
		u32 val = gstate.cmdmem[cmd];
		switch (cmd) {
		case GE_CMD_BONEMATRIXDATA:
		case GE_CMD_WORLDMATRIXDATA:
		case GE_CMD_VIEWMATRIXDATA:
		case GE_CMD_PROJMATRIXDATA:
		case GE_CMD_TGENMATRIXDATA:
			val &= 0xFF000000;
			break;

		case GE_CMD_BONEMATRIXNUMBER:
			val &= 0xFF00007F;
			break;

		case GE_CMD_WORLDMATRIXNUMBER:
		case GE_CMD_VIEWMATRIXNUMBER:
		case GE_CMD_PROJMATRIXNUMBER:
		case GE_CMD_TGENMATRIXNUMBER:
			val &= 0xFF00000F;
			break;

		default:
			break;
		}
		return hleLogInfo(Log::sceGe, val);
	}
	return hleLogError(Log::sceGe, SCE_KERNEL_ERROR_INVALID_INDEX);
}

static int sceGeGetStack(int index, u32 stackPtr) {
	return hleReportWarning(Log::sceGe, gpu->GetStack(index, stackPtr));
}

static u32 sceGeEdramSetAddrTranslation(u32 new_size) {
	bool outsideRange = new_size != 0 && (new_size < 0x200 || new_size > 0x1000);
	bool notPowerOfTwo = (new_size & (new_size - 1)) != 0;
	if (outsideRange || notPowerOfTwo) {
		return hleLogWarning(Log::sceGe, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid value");
	}
	if (!gpu) {
		return hleLogError(Log::sceGe, -1, "GPUInterface not available");
	}

	return hleLogDebug(Log::sceGe, gpu->SetAddrTranslation(new_size));
}

const HLEFunction sceGe_user[] = {
	{0XE47E40E4, &WrapU_V<sceGeEdramGetAddr>,            "sceGeEdramGetAddr",            'x', ""    },
	{0XAB49E76A, &WrapU_UUIU<sceGeListEnQueue>,          "sceGeListEnQueue",             'x', "xxip"},
	{0X1C0D95A6, &WrapU_UUIU<sceGeListEnQueueHead>,      "sceGeListEnQueueHead",         'x', "xxip"},
	{0XE0D68148, &WrapI_UU<sceGeListUpdateStallAddr>,    "sceGeListUpdateStallAddr",     'i', "xx"  },
	{0X03444EB4, &WrapI_UU<sceGeListSync>,               "sceGeListSync",                'x', "xx"  },
	{0XB287BD61, &WrapU_U<sceGeDrawSync>,                "sceGeDrawSync",                'x', "x"   },
	{0XB448EC0D, &WrapI_UU<sceGeBreak>,                  "sceGeBreak",                   'i', "xx"  },
	{0X4C06E472, &WrapI_V<sceGeContinue>,                "sceGeContinue",                'i', ""    },
	{0XA4FC06A4, &WrapU_U<sceGeSetCallback>,             "sceGeSetCallback",             'i', "p"   },
	{0X05DB22CE, &WrapI_U<sceGeUnsetCallback>,           "sceGeUnsetCallback",           'i', "x"   },
	{0X1F6752AD, &WrapU_V<sceGeEdramGetSize>,            "sceGeEdramGetSize",            'x', ""    },
	{0XB77905EA, &WrapU_U<sceGeEdramSetAddrTranslation>, "sceGeEdramSetAddrTranslation", 'x', "x"   },
	{0XDC93CFEF, &WrapU_I<sceGeGetCmd>,                  "sceGeGetCmd",                  'x', "i"   },
	{0X57C8945B, &WrapI_IU<sceGeGetMtx>,                 "sceGeGetMtx",                  'i', "ip"  },
	{0X438A385A, &WrapU_U<sceGeSaveContext>,             "sceGeSaveContext",             'x', "x"   },
	{0X0BF608FB, &WrapU_U<sceGeRestoreContext>,          "sceGeRestoreContext",          'x', "x"   },
	{0X5FB86AB0, &WrapI_U<sceGeListDeQueue>,             "sceGeListDeQueue",             'i', "x"   },
	{0XE66CB92E, &WrapI_IU<sceGeGetStack>,               "sceGeGetStack",                'i', "ix"  },
};

void Register_sceGe_user() {
	RegisterHLEModule("sceGe_user", ARRAY_SIZE(sceGe_user), sceGe_user);
}
