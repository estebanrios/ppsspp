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
#include <unistd.h>
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
// Profundidad de CandadoGe VIVOS en este hilo. La cesion en frontera de lista
// solo es segura si el unico tenedor del candado es el lock de la pasada del
// worker; si hubiera un CandadoGe anidado, soltar una vez no liberaria nada
// (g_mu es recursivo) y ademas romperia su invariante. Con el contador en 0 la
// cesion es correcta por construccion, no por inspeccion.
inline thread_local int g_candadosVivos = 0;

// Cuantos hilos estan BLOQUEADOS esperando g_mu ahora mismo. Lo usa la cesion
// en frontera de lista para no pagar un sched_yield por lista cuando no hay
// nadie a quien cederle: con la cola vacia, ceder es puro costo.
inline std::atomic<int> g_esperandoGe{0};

class CandadoGe {
public:
	// STV_MEDIDOR_ESPERAS_v1: la ranura dice QUE sitio del cuadro esta
	// esperando el candado. Default R_CAND_OTRO para no tocar los ~30 sitios
	// que no interesan; los del camino del cuadro pasan la suya. Con el
	// medidor apagado esto es un load relaxed y una rama predecible mas.
	explicit CandadoGe(stvmed::Ranura ranura = stvmed::R_CAND_OTRO) : tomado_(NivelActivo() != 0) {
		if (tomado_) {
			stvmed::Cronometro c(ranura);
			g_esperandoGe.fetch_add(1, std::memory_order_relaxed);
			g_mu.lock();
			g_esperandoGe.fetch_sub(1, std::memory_order_relaxed);
			++g_candadosVivos;
		}
	}
	~CandadoGe() {
		if (tomado_) {
			--g_candadosVivos;
			g_mu.unlock();
		}
	}
	CandadoGe(const CandadoGe &) = delete;
	CandadoGe &operator=(const CandadoGe &) = delete;

private:
	bool tomado_;
};

// --- API del modulo (implementada en StvGeThread.cpp) ------------------------

// --- CANDADO FINO DE LA CONTABILIDAD DE LISTAS (valvula debug.stv.dlfino) ----
//
// EL PROBLEMA MEDIDO (Spiderman 3, ranura 4): el EmuThread pierde 3,2 ms POR
// CUADRO en 10 tomas, esperando g_mu dentro del manejador de interrupciones del
// GE. El worker retiene g_mu 2,45 ms seguidos por pasada (3 pasadas/cuadro) y
// el manejador queda detras. Quitar esa espera lleva el cuadro de 18,6 a 15,4
// ms = 65 VPS: el techo de 60 con margen.
//
// POR QUE SE PUEDE SEPARAR: el manejador NO toca currentList — que es lo que el
// lazo caliente (Execute_Jump/Ret/UpdatePC) usa sin parar. Toca los campos de
// UNA lista (state, signal, subIntrBase, subIntrToken), dlQueue y su propia
// ge_pending_cb. Ese conjunto se puede proteger aparte, y el worker solo
// necesita tomarlo en las FRONTERAS (elegir lista, cambiar estado, sacarla de
// la cola) y en los pocos comandos que cambian estado (Signal/End), no durante
// FastRunLoop.
//
// ORDEN DE CANDADOS: g_mu -> g_muDL. Quien tenga el grueso puede tomar el fino;
// nunca al reves. Es la misma regla que ya rige para g_muCola y g_muInval, y se
// respeta porque el fino solo se toma en hojas.
//
// VALVULA: debug.stv.dlfino. En 0 (DEFAULT) el manejador sigue tomando el
// candado GRUESO y todo queda byte-identico a hoy; el fino se toma igual en los
// dos casos, asi que el camino nuevo se ejercita sin depender de la valvula y
// una carrera aparece en el testigo aunque este apagado.
//
// EL TESTIGO ES LA RED: TestigoDL cuenta ENTRADAS CONCURRENTES a esta zona.
// Validado en las dos direcciones antes de escribir una linea de esto: con el
// candado grueso da 0 colisiones en 95.015 entradas, y con una colision
// inyectada a proposito grita y nombra al culpable. Si el refactor deja un
// sitio suelto, el aparato lo dice. NO SE HORNEA con una sola colision, por
// mas que el rendimiento mejore.
inline std::recursive_mutex g_muDL;

// Cuantas veces InterruptEnd necesita DE VERDAD el candado grueso: solo cuando
// la lista completo Y tenia contexto guardado (Restore + ReapplyGfxState). El
// resto es contabilidad pura. La proporcion decide si vale la pena partir la
// funcion en dos fases.
inline std::atomic<uint64_t> g_intrEndTotal{0};
inline std::atomic<uint64_t> g_intrEndConGpu{0};

// (definida en StvGeThread.cpp: NO va inline, o el simbolo no se emite)
bool DLFinoActivo();

// --- TESTIGO DE EXCLUSION SOBRE LA CONTABILIDAD DE LISTAS --------------------
//
// PARA QUE: el arreglo del candado de las interrupciones cambia QUIEN serializa
// el acceso a dls[]/dlQueue/ge_pending_cb. Si un sitio se escapa, el modo de
// fallo no es ruidoso: es una carrera sobre estado compartido, o sea corrupcion
// silenciosa. Este testigo la convierte en un NUMERO.
//
// COMO: cada hilo que entra a la zona protegida sube un contador atomico y lo
// baja al salir. Ese contador DEBE valer 0 o 1. Si alguna vez vale 2, hubo dos
// hilos adentro a la vez y queda contado — aunque esa carrera puntual no llegue
// a romper nada visible.
//
// COMO SE VALIDA (y por que se construye ANTES del refactor):
//   1. con el candado GRUESO actual tiene que dar CERO siempre  -> sin falsos
//      positivos;
//   2. inyectandole una colision a proposito tiene que GRITAR   -> sensible.
//   Recien despues sirve para vigilar el cambio de verdad.
//
// COSTO: dos atomicas relaxed por entrada. A frecuencia de syscall es ruido.
//
// NO es un detector de carreras completo (dos hilos podrian intercalarse sin
// solaparse dentro de la guarda). Es un detector de ENTRADA CONCURRENTE, que es
// el fallo que nos importa: dos duenos del mismo estado al mismo tiempo.
inline std::atomic<int> g_dentroDL{0};
inline std::atomic<uint64_t> g_colisionesDL{0};
inline std::atomic<uint64_t> g_entradasDL{0};
inline std::atomic<int> g_picoDL{0};
// IDENTIDAD de la colision: quien estaba adentro, quien entro encima, y en que
// sitio cada uno. Sin esto el contador dice QUE pasa pero no QUIENES, y habria
// que deducirlo — que es justo lo que no queremos hacer con una carrera.
inline std::atomic<int> g_dueñoDL{0};          // tid del que entro primero
inline std::atomic<const char *> g_sitioDL{nullptr};
inline std::atomic<int> g_choqueTid{0};
inline std::atomic<const char *> g_choqueSitio{nullptr};
inline std::atomic<const char *> g_choqueDueñoSitio{nullptr};

class TestigoDL {
public:
	explicit TestigoDL(const char *sitio) {
		const int antes = g_dentroDL.fetch_add(1, std::memory_order_acq_rel);
		g_entradasDL.fetch_add(1, std::memory_order_relaxed);
		const int yo = (int)gettid();
		// RECURSION NO ES COLISION. g_muDL es recursivo a proposito: las
		// funciones de esta zona se llaman entre si (handleResult -> InterruptEnd
		// es el caso tipico). Contar la entrada anidada del MISMO hilo daba
		// 55.668 "colisiones" que no eran carreras: eran el mismo hilo entrando
		// dos veces. Solo cuenta si el que ya estaba adentro es OTRO hilo.
		if (antes != 0 && g_dueñoDL.load(std::memory_order_relaxed) != yo) {
			g_colisionesDL.fetch_add(1, std::memory_order_relaxed);
			g_choqueTid.store(yo, std::memory_order_relaxed);
			g_choqueSitio.store(sitio, std::memory_order_relaxed);
			g_choqueDueñoSitio.store(g_sitioDL.load(std::memory_order_relaxed), std::memory_order_relaxed);
			int pico = g_picoDL.load(std::memory_order_relaxed);
			while (antes + 1 > pico &&
			       !g_picoDL.compare_exchange_weak(pico, antes + 1, std::memory_order_relaxed)) {}
		} else {
			g_dueñoDL.store(yo, std::memory_order_relaxed);
			g_sitioDL.store(sitio, std::memory_order_relaxed);
		}
	}
	// El testigo tiene que vivir EXACTAMENTE lo que vive el candado que vigila.
	// GeIntrHandler suelta g_mu ANTES de terminar la funcion (a proposito: el
	// despacho final esperaria al worker y se colgaria si lo tuviera tomado).
	// Sin esta liberacion explicita, la guarda seguia "adentro" en ese hueco y
	// contaba como colision al worker entrando legitimamente: un falso positivo
	// del instrumento, no una carrera del emulador. Medido: 5 colisiones en
	// 51.835 entradas, todas con esa firma.
	void soltar() {
		if (!soltado_) { soltado_ = true; g_dentroDL.fetch_sub(1, std::memory_order_acq_rel); }
	}
	~TestigoDL() { soltar(); }
	TestigoDL(const TestigoDL &) = delete;
	TestigoDL &operator=(const TestigoDL &) = delete;

private:
	bool soltado_ = false;
};

// La primitiva de la zona: toma el candado fino Y enciende el testigo, en ese
// orden. Asi es IMPOSIBLE proteger un sitio y olvidarse de vigilarlo — que es
// como se cuelan las carreras en un refactor de candados.
// El testigo se construye DESPUES del lock (orden de miembros) y se destruye
// ANTES, de modo que solo cuenta mientras el candado esta tomado.
class CandadoDL {
public:
	explicit CandadoDL(const char *sitio) : lk_(g_muDL), testigo_(sitio) {}
	// Liberacion explicita: hace falta en GeIntrHandler, que suelta ANTES del
	// despacho final porque ese despacho espera al worker (y el worker necesita
	// este mismo candado: tenerlo tomado seria un abrazo mortal).
	void soltar() {
		if (lk_.owns_lock()) { testigo_.soltar(); lk_.unlock(); }
	}
	CandadoDL(const CandadoDL &) = delete;
	CandadoDL &operator=(const CandadoDL &) = delete;
private:
	std::unique_lock<std::recursive_mutex> lk_;
	TestigoDL testigo_;
};

// Colision a proposito, para comprobar que el testigo SABE gritar. La enciende
// debug.stv.dl.probar=1 y hace que un hilo aparte entre a la zona mientras el
// EmuThread esta adentro. Sin esto, un contador en cero no prueba nada.
void ProbarTestigoDL();

// --- Invalidaciones diferidas (valvula debug.stv.inval) ----------------------
//
// EL PROBLEMA MEDIDO (Spiderman 3, escena del incendio, medidor de esperas):
// con el worker encendido el EmuThread pasa 8,0-8,9 ms POR CUADRO parado en
// cand_invalidate, con 56-61 tomas por cuadro. Es InvalidateCache pidiendo
// g_mu mientras el worker lo retiene de punta a punta de su pasada (los 8,62 ms
// de "DL processing time" que reporta el propio emulador). El worker saca 8,62
// ms de trabajo del EmuThread y le devuelve 8,9 ms de espera: por eso en ese
// juego el worker RESTA (35 VPS con worker vs 44,7 sin el).
//
// LA OBSERVACION QUE LO DESTRABA: hoy, cuando el EmuThread se topa con el
// candado tomado por una pasada, se queda esperando y su invalidacion se
// aplica IGUAL despues de la pasada. O sea que encolarla para que el worker la
// aplique al terminar produce EXACTAMENTE EL MISMO ORDEN — lo unico que cambia
// es que el EmuThread no se queda parado. No es una relajacion de la
// semantica: es la misma secuencia sin la parada.
//
// Devuelve true si quedo encolada (el llamador no hace nada mas) y false si hay
// que hacerla ahi mismo (worker apagado, candado libre, cola llena o valvula
// en 0). Con la valvula en 0 devuelve false SIEMPRE: upstream exacto.
bool DiferirInvalidacion(uint32_t addr, int size, int type);

// --- Cesion en frontera de lista (valvula debug.stv.ceder) -------------------
//
// EL PROBLEMA MEDIDO (Spiderman 3, escena del incendio grande, ranura 3): en
// las ventanas LENTAS el EmuThread espera 4,07 ms por cuadro en cand_encola
// contra 0,76 en las rapidas. Es sceGeListEnQueue pidiendo g_mu mientras el
// worker lo retiene de punta a punta de su pasada. EnqueueList no toca nada de
// GPU — solo dls[], dlQueue y currentList— pero igual queda detras del candado
// grueso hasta que la pasada entera termina.
//
// POR QUE LA FRONTERA ES LEGITIMA: se cede al TERMINAR cada lista, despues de
// FinishDeferred() y de sacarla de la cola, justo antes de elegir la siguiente.
// Ese es exactamente un punto en el que upstream YA puede retornar al EmuThread
// (DLResult::Done sale de ahi), asi que el estado que queda expuesto no es
// nuevo: es el mismo que ve el camino inline entre dos ProcessDLQueue.
//
// No cambia el orden de ejecucion de las listas ni el trabajo que se hace:
// solo abre una ventana para que el que espera entre antes.
void CederEnFronteraDeLista();

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
