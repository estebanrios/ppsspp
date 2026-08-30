// =============================================================================
// STV — EL HILO DEL GE (arco F6, entrega E1).
//
// Parche del proyecto STV-TSPS-Android. NO es codigo de upstream.
// El plano maestro vive en docs/f6-plano.md del proyecto; este modulo se
// implemento CONTRA ese documento.
//
// QUE ES: un worker ("STVGeWorker") que ejecuta ProcessDLQueue() —el 38,5 % del
// EmuThread— en OTRO nucleo A55. El worker no inventa un mecanismo de
// terminacion: la terminacion de una lista YA es un evento diferido de
// CoreTiming (__GeTriggerInterrupt/__GeTriggerSync con tick futuro), asi que el
// worker solo POSTEA esos mismos triggers a una cola FIFO y el EmuThread los
// materializa (drenaje) donde CoreTiming puede verlos.
//
// EL REPARTO: las syscalls sceGe*, CoreTiming, el kernel HLE, el flip y los
// savestates quedan en el EmuThread. El worker corre SOLO el loop de listas
// (FastRunLoop + DrawEngine + texturas + framebuffers), de punta a punta bajo
// el candado grueso g_mu. Sin ese candado, el worker no toca nada.
//
// NIVELES (prop Android `debug.stv.ge`, patron de lectura de StvEpilogo.h):
//   0 = inline: upstream exacto, worker dormido o inexistente.  <- DEFAULT
//   1 = worker sincrono: la syscall postea la orden RUN y espera la
//       terminacion ahi mismo. Mismo flujo y mismos ticks simulados que
//       inline — solo cambia el nucleo. Valida el marshalling con riesgo cero.
//   2 = worker asincrono: la syscall postea y retorna; el juego corre en
//       paralelo con el GE (LA ganancia).
// La prop se relee una vez por vblank; la conmutacion de nivel solo la ejecuta
// el EmuThread en frontera segura (worker idle + colas drenadas). 0<->1<->2 en
// caliente sin reiniciar el juego = A/B con un solo APK.
//
// INVARIANTES QUE ESTE MODULO SOSTIENE (plano §2):
//   - CoreTiming y el kernel HLE solo se tocan desde el EmuThread. Los
//     __GeTrigger* que el camino de ejecucion dispara EN el worker se postean
//     (choke point en sceGe.cpp) y se ejecutan en el drenaje.
//   - startingTicks en modo worker = el tick capturado AL POSTEAR la orden
//     (TickDeLaOrden), para que atTicks = tickPosteo + cyclesExecuted de
//     exactamente la misma formula que inline.
//   - Con la prop en 0 no hay NI UN branch nuevo en el camino caliente por-op;
//     lo unico tolerado es un load relaxed por vuelta de CoreTiming::Advance
//     (HayTerminaciones) y los branches a frecuencia de syscall.
//   - Un solo emisor de draws a la vez: el worker de punta a punta de cada
//     pasada, y el EmuThread en toda entrada a territorio GPU, ambos bajo g_mu.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "Common/StvMedidor.h"  // STV_MEDIDOR_ESPERAS_v1

namespace stvge {

// Cadena que prueba que el parche entro en el .so:
//   strings libppsspp_jni.so | grep STV_GE_THREAD_v1
inline constexpr const char *kMarca = "STV_GE_THREAD_v1";

// --- Camino caliente (todo inline, costo minimo) -----------------------------

// Testigo de terminaciones pendientes (worker -> EmuThread). Es EL chequeo
// barato del drenaje de respaldo en CoreTiming::Advance: un load relaxed por
// vuelta del mainloop, cero contencion (solo el worker lo pone en true).
inline std::atomic<bool> g_hayTerminaciones{false};
inline bool HayTerminaciones() {
	return g_hayTerminaciones.load(std::memory_order_relaxed);
}

// Nivel ACTIVO (no el deseado de la prop): 0/1/2. Lo escribe SOLO el EmuThread
// y solo en frontera segura (PorVblank / Apagar). Es atomico porque el worker
// lo lee via CandadoGe en llamadas anidadas (ReapplyGfxState desde
// Execute_End); relaxed alcanza porque cuando cambia, el worker esta idle por
// construccion.
inline std::atomic<int> g_nivelActivo{0};
inline int NivelActivo() {
	return g_nivelActivo.load(std::memory_order_relaxed);
}

// El candado grueso del GE. RECURSIVO a proposito: upstream anida entradas de
// territorio GPU entre si (BusyDrawing -> DrawSync; Execute_End ->
// ReapplyGfxState; PerformMemoryCopy -> InvalidateCache) y un mutex plano nos
// obligaria a refactorizar el grafo de llamadas de upstream — mas diff, mas
// riesgo. El costo extra del recursive_mutex es irrelevante a frecuencia de
// syscall.
inline std::recursive_mutex g_mu;

// Guardia de las entradas del EmuThread a territorio GPU. Con nivel 0 no toma
// nada: un load relaxed y un branch predecible a frecuencia de syscall — el
// comportamiento queda byte-identico a upstream. Con nivel > 0 serializa
// contra la pasada del worker.
class CandadoGe {
public:
	// STV_MEDIDOR_ESPERAS_v1: la ranura dice QUE sitio del cuadro esta
	// esperando el candado. Default R_CAND_OTRO para no tocar los ~30 sitios
	// que no interesan; los del camino del cuadro pasan la suya. Con el
	// medidor apagado esto es un load relaxed y una rama predecible mas.
	explicit CandadoGe(stvmed::Ranura ranura = stvmed::R_CAND_OTRO) : tomado_(NivelActivo() != 0) {
		if (tomado_) {
			stvmed::Cronometro c(ranura);
			g_mu.lock();
		}
	}
	~CandadoGe() {
		if (tomado_)
			g_mu.unlock();
	}
	CandadoGe(const CandadoGe &) = delete;
	CandadoGe &operator=(const CandadoGe &) = delete;

private:
	bool tomado_;
};

// --- API del modulo (implementada en StvGeThread.cpp) ------------------------

// Identidad de hilo: true SOLO en el worker. Es lo que consulta el choke point
// de __GeTriggerSync/__GeTriggerInterrupt en sceGe.cpp para decidir si postear.
bool EnWorker();

// El tick simulado que viajo con la orden RUN que el worker esta atendiendo.
// Solo tiene sentido con EnWorker() == true; es lo que ProcessDLQueue usa como
// startingTicks en vez de CoreTiming::GetTicks() (que en el worker seria una
// carrera y un valor sin sentido).
uint64_t TickDeLaOrden();

// Posteos del worker (los llama el choke point de sceGe.cpp; nunca upstream).
void PostearInterrupt(int listid, uint32_t pc, uint64_t atTicks);
void PostearSync(int type, int id, uint64_t atTicks);

// Ejecuta EN el EmuThread, en orden FIFO, los triggers pendientes llamando a
// las funciones originales (__GeTriggerInterrupt/__GeTriggerSync). Las
// transiciones de estado de las listas ya las hizo el worker bajo candado; el
// drenaje solo materializa los eventos donde CoreTiming puede verlos.
void Drenar();

// Espera (bloqueando el hilo que llama, sin tomar g_mu) a que el worker quede
// idle y sin ordenes pendientes. Segura desde cualquier hilo del host.
void EsperarIdle();

// EsperarIdle + Drenar. La frontera segura completa: despues de esto no hay
// pasada en vuelo ni terminacion sin materializar. Solo desde el EmuThread
// (el drenaje toca CoreTiming).
void Barrera(stvmed::Ranura ranura = stvmed::R_BARRERA_OTRA);

// El paso previo de ListSync/DrawSync mode=0 en modo worker: si la lista
// consultada puede seguir en el worker, esperar la pasada y drenar para que la
// logica original de upstream (waitUntilTicks / drawCompleteTicks) decida con
// el estado final. Contabiliza las esperas reales en el testigo.
void EsperarGeParaSync();

// El dispatch unico que reemplaza a los gpu->ProcessDLQueue() de sceGe.cpp:
// nivel 0 -> inline exacto; nivel 1 -> postear + esperar + drenar; nivel 2 ->
// postear y volver. Si el GPU exige inline (debugger/recorder/dump), degrada a
// inline CON barrera aunque el nivel activo sea > 0.
void DespacharProcessDLQueue();

// Una vez por vblank (hleEnterVblank): relee la prop, drena pendientes y
// conmuta de nivel si la frontera es segura. Es el unico escritor de
// g_nivelActivo (junto con Apagar).
void PorVblank();

// Join del worker + descarte de colas. Se llama desde GPU_Shutdown() ANTES de
// borrar el objeto gpu (el worker corre sobre ese objeto).
void Apagar();

}  // namespace stvge
