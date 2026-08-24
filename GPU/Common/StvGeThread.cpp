// =============================================================================
// STV — EL HILO DEL GE (arco F6, entrega E1). Ver StvGeThread.h para el mapa.
//
// Este archivo contiene TODO el mecanismo: el worker, el doorbell de ordenes,
// la cola FIFO de terminaciones, la lectura de la prop y la conmutacion de
// nivel. Las costuras en archivos de upstream (sceGe.cpp, GPUCommon.cpp,
// sceDisplay.cpp, CoreTiming.cpp, GPU.cpp, NativeApp.cpp) solo llaman aca.
//
// EL ORDEN DE LOS CANDADOS, que es lo unico que puede matarnos:
//   g_mu (candado grueso del GE)  ->  g_muCola (doorbell/colas)
// El worker toma g_mu de punta a punta de cada pasada y ADENTRO toma g_muCola
// un instante para postear triggers. El EmuThread jamas toma g_mu teniendo
// g_muCola (Drenar saca el lote bajo g_muCola y lo ejecuta AFUERA), y jamas
// espera al worker teniendo g_mu (los dispatches postean DESPUES de que la
// syscall solto el candado). Sin ciclo, sin deadlock.
// =============================================================================

#include <condition_variable>
#include <sched.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/sceGe.h"
#include "GPU/Common/StvGeThread.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"

namespace stvge {

// --- Niveles y lectura de la prop (patron StvEpilogo.h) ----------------------
//
// A diferencia de debug.stv.epi, aca el DEFAULT ES 0 (inline): el worker es un
// cambio de arquitectura, no una optimizacion probada, y la regla del arco es
// que la prop ausente = upstream exacto. La regla del typo se conserva del
// patron epi: un caracter no-digito vale el default, no cero — que aca da lo
// mismo, pero mantiene identico el contrato de lectura entre las dos palancas.
inline constexpr int kNivelMax = 2;
inline constexpr int kNivelDefecto = 0;

static int NivelDeTexto(const char *s) {
	if (!s || !*s)
		return kNivelDefecto;
	if (*s < '0' || *s > '9')
		return kNivelDefecto;
	const int v = *s - '0';
	return v > kNivelMax ? kNivelMax : v;
}

static int ResolverNivel() {
	// El env manda sobre la prop, igual que STV_EPI_FUSION: es lo que usa el
	// banco cuando corre el binario fuera de Android.
	const char *e = getenv("STV_GE_NIVEL");
	if (e && *e)
		return NivelDeTexto(e);
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.ge", prop) > 0 && prop[0]) {
		return NivelDeTexto(prop);
	}
#endif
	return kNivelDefecto;
}

static void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

// --- Estado interno ----------------------------------------------------------

// Descriptor de trigger pendiente. Lleva los argumentos EXACTOS con los que el
// camino inline habria llamado a __GeTriggerInterrupt/__GeTriggerSync; el
// drenaje los reproduce tal cual, en orden FIFO.
enum class Clase : uint8_t {
	Interrupt,   // v1 = listid, v2 = pc
	Sync,        // v1 = GPUSyncType, v2 = id
};

struct Trigger {
	Clase clase;
	int v1;
	uint32_t v2;
	uint64_t atTicks;
};

// Doorbell + colas. g_muCola protege TODO lo de abajo. La orden RUN no lleva
// las listas (dlQueue ES la cola, compartida bajo g_mu): es un timbre con el
// tick simulado capturado al postear.
static std::mutex g_muCola;
static std::condition_variable g_cvOrden;   // EmuThread -> worker: hay orden
static std::condition_variable g_cvIdle;    // worker -> quien espere: quede idle
static bool g_ordenPendiente = false;
static uint64_t g_tickOrden = 0;
static bool g_corriendo = false;
static bool g_apagando = false;
static std::deque<Trigger> g_terminaciones;

static std::thread g_worker;
// atomic: lo escribe el EmuThread (crear/apagar) pero DeviceLost puede llamar
// EsperarIdle desde el hilo grafico del host.
static std::atomic<bool> g_workerCreado{false};

static thread_local bool tl_enWorker = false;
static thread_local uint64_t tl_tickOrden = 0;

// Contadores testigo. Atomicos relaxed porque los incrementa el worker y los
// lee el EmuThread al loguear: la regla del proyecto es que un testigo no
// puede mentir, y un int plano cruzando hilos es formalmente una carrera.
static std::atomic<uint32_t> g_pasadas{0};        // ordenes RUN atendidas por el worker
static std::atomic<uint32_t> g_triggers{0};       // triggers posteados por el worker
static std::atomic<uint32_t> g_drenajes{0};       // drenajes con trabajo real
static std::atomic<uint32_t> g_esperasSync{0};    // ListSync/DrawSync que esperaron al worker

static bool g_hola = false;

bool EnWorker() {
	return tl_enWorker;
}

uint64_t TickDeLaOrden() {
	return tl_tickOrden;
}

// --- Afinidad ----------------------------------------------------------------
//
// El A523 tiene dos clusters de A55: p0-p3 a 1416 MHz y p4-p7 a 2232. Con el
// worker activo, EmuThread y worker ALTERNAN (util ~50 % cada uno) y EAS los
// ve livianos: los rebota por los littles y les mete latencia de despertar en
// cada handshake — MEDIDO en el banco (2026-08-24): nivel 1 daba 33 ms/cuadro
// (30 fps) con los tres hilos migrando por las 8 CPUs, contra 16,7 ms del
// inline, cuyo hilo unico al 95 % de util se planta solo en un big. Clavar
// worker y EmuThread al cluster grande mientras el nivel > 0 devuelve la
// colocacion que upstream consigue gratis por concentrar todo en un hilo.
// Best effort a proposito: si el kernel rechaza la mascara (otra topologia),
// se sigue sin afinidad, que es exactamente el comportamiento de hoy.

static void AfinidadCluster(bool soloGrandes) {
#if defined(__linux__)
	// cpu_set_t/sched_setaffinity son de Linux/bionic; en las demas
	// plataformas del fork esto es un no-op (alli no existe el A523).
	cpu_set_t set;
	CPU_ZERO(&set);
	const int desde = soloGrandes ? 4 : 0;
	for (int c = desde; c <= 7; c++)
		CPU_SET(c, &set);
	sched_setaffinity(0, sizeof(set), &set);  // 0 = el hilo que llama
#else
	(void)soloGrandes;
#endif
}

// --- El worker ---------------------------------------------------------------

static void WorkerMain() {
	SetCurrentThreadName("STVGeWorker");
	tl_enWorker = true;
	AfinidadCluster(true);

	std::unique_lock<std::mutex> lk(g_muCola);
	while (true) {
		g_cvOrden.wait(lk, [] { return g_ordenPendiente || g_apagando; });
		if (g_apagando)
			break;

		// El tick viajo con la orden; es el startingTicks de la pasada (misma
		// formula que inline: atTicks = tick del posteo + cyclesExecuted).
		tl_tickOrden = g_tickOrden;
		g_ordenPendiente = false;
		g_corriendo = true;
		lk.unlock();

		{
			// El candado grueso, de punta a punta de la pasada: mientras el
			// worker ejecuta, el EmuThread no entra a territorio GPU.
			std::lock_guard<std::recursive_mutex> ge(g_mu);
			DLResult r = gpu->ProcessDLQueue();
			// DebugBreak es imposible aca: el dispatch degrada a inline
			// cuando hay debugger/recorder activo (StvGeExigeInline).
			_dbg_assert_(r != DLResult::DebugBreak);
			(void)r;
		}
		g_pasadas.fetch_add(1, std::memory_order_relaxed);

		lk.lock();
		g_corriendo = false;
		if (!g_ordenPendiente)
			g_cvIdle.notify_all();
	}
}

static void CrearWorker() {
	g_apagando = false;
	g_worker = std::thread(&WorkerMain);
	g_workerCreado.store(true, std::memory_order_relaxed);
}

// --- Ordenes y triggers ------------------------------------------------------

static void PostearRun(uint64_t tick) {
	{
		std::lock_guard<std::mutex> lk(g_muCola);
		// Coalescencia: si ya hay una orden sin atender, la pasada que venga
		// cubre tambien este pedido (dlQueue es la cola real). Conservamos el
		// tick de la PRIMERA orden: atTicks nunca queda antes de su posteo, y
		// la microderiva (dos posteos en la ventana de despertar del worker)
		// solo existe en nivel 2, donde el timing ya no es ciclo-exacto.
		if (!g_ordenPendiente) {
			g_ordenPendiente = true;
			g_tickOrden = tick;
		}
	}
	g_cvOrden.notify_one();
}

void PostearInterrupt(int listid, uint32_t pc, uint64_t atTicks) {
	std::lock_guard<std::mutex> lk(g_muCola);
	g_terminaciones.push_back(Trigger{ Clase::Interrupt, listid, pc, atTicks });
	g_triggers.fetch_add(1, std::memory_order_relaxed);
	g_hayTerminaciones.store(true, std::memory_order_relaxed);
}

void PostearSync(int type, int id, uint64_t atTicks) {
	std::lock_guard<std::mutex> lk(g_muCola);
	g_terminaciones.push_back(Trigger{ Clase::Sync, type, (uint32_t)id, atTicks });
	g_triggers.fetch_add(1, std::memory_order_relaxed);
	g_hayTerminaciones.store(true, std::memory_order_relaxed);
}

void Drenar() {
	// Solo EmuThread: los __GeTrigger* de abajo tocan CoreTiming y
	// ge_pending_cb, que son territorio exclusivo de ese hilo.
	_dbg_assert_(!EnWorker());
	if (!HayTerminaciones())
		return;

	std::deque<Trigger> lote;
	{
		std::lock_guard<std::mutex> lk(g_muCola);
		lote.swap(g_terminaciones);
		// Dentro del candado: un posteo del worker posterior al swap vuelve a
		// levantar la bandera despues de este store, nunca antes.
		g_hayTerminaciones.store(false, std::memory_order_relaxed);
	}
	if (lote.empty())
		return;
	g_drenajes.fetch_add(1, std::memory_order_relaxed);

	// FIFO estricto: el orden relativo entre interrupts y syncs es el mismo en
	// el que el camino inline los habria emitido. Se ejecutan FUERA de
	// g_muCola (regla de orden de candados del encabezado).
	for (const Trigger &t : lote) {
		if (t.clase == Clase::Interrupt) {
			__GeTriggerInterrupt(t.v1, t.v2, t.atTicks);
		} else {
			__GeTriggerSync((GPUSyncType)t.v1, (int)t.v2, t.atTicks);
		}
	}
}

// --- Esperas y barreras ------------------------------------------------------

// contarEspera: true cuando la espera viene de ListSync/DrawSync (testigo).
static void EsperarIdleInterno(bool contarEspera) {
	if (!g_workerCreado.load(std::memory_order_relaxed))
		return;
	std::unique_lock<std::mutex> lk(g_muCola);
	if (g_corriendo || g_ordenPendiente) {
		if (contarEspera)
			g_esperasSync.fetch_add(1, std::memory_order_relaxed);
		g_cvIdle.wait(lk, [] { return !g_corriendo && !g_ordenPendiente; });
	}
}

void EsperarIdle() {
	EsperarIdleInterno(false);
}

void Barrera() {
	EsperarIdleInterno(false);
	Drenar();
}

void EsperarGeParaSync() {
	// Si el worker no esta en juego, no hay nada que esperar ni drenar: cero
	// costo agregado en nivel 0 mas alla del branch.
	if (NivelActivo() == 0)
		return;
	// La espera es a WORKER IDLE, no a "lista completada": una lista stalleada
	// deja al worker idle sin completarse, y ahi la logica original de
	// upstream (waitUntilTicks == -1 => duerme al hilo EMULADO) es exactamente
	// lo que corresponde, igual que inline.
	EsperarIdleInterno(true);
	Drenar();
}

// --- El dispatch -------------------------------------------------------------

void DespacharProcessDLQueue() {
	const int nivel = NivelActivo();
	if (nivel == 0) {
		// Upstream exacto (el assert es el mismo que tenian los callers).
		DLResult r = gpu->ProcessDLQueue();
		_dbg_assert_(r != DLResult::DebugBreak);
		(void)r;
		return;
	}

	if (gpu->StvGeExigeInline()) {
		// Debugger, recorder o dump de cuadro activos: jamas conviven con el
		// worker (SlowRunLoop y sus notificaciones son de afinidad EmuThread).
		// Barrera primero por si la palanca del debugger se prendio con una
		// pasada en vuelo; despues inline bajo candado, que con nivel > 0 es
		// la unica forma legitima de entrar a territorio GPU.
		Barrera();
		std::lock_guard<std::recursive_mutex> ge(g_mu);
		DLResult r = gpu->ProcessDLQueue();
		_dbg_assert_(r != DLResult::DebugBreak);
		(void)r;
		return;
	}

	// El tick se captura ACA, en el mismo punto simulado en el que el camino
	// inline habria hecho CoreTiming::GetTicks() al entrar a ProcessDLQueue.
	const uint64_t tick = CoreTiming::GetTicks();
	PostearRun(tick);
	if (nivel == 1) {
		// Sincrono: mismo flujo que inline, solo cambia el nucleo. El drenaje
		// inmediato deja CoreTiming y ge_pending_cb IGUAL que si la pasada
		// hubiera corrido aca (el reloj simulado no avanzo mientras tanto).
		EsperarIdleInterno(false);
		Drenar();
	}
}

// --- Conmutacion por vblank --------------------------------------------------

static void EmitirEstado(const char *encabezado, int nivel) {
	char b[256];
	snprintf(b, sizeof(b),
		"STV: ge %s nivel=%d (%s) pasadas=%u triggers=%u drenajes=%u esperas=%u",
		encabezado, nivel, kMarca,
		g_pasadas.load(std::memory_order_relaxed),
		g_triggers.load(std::memory_order_relaxed),
		g_drenajes.load(std::memory_order_relaxed),
		g_esperasSync.load(std::memory_order_relaxed));
	Emitir(b);
}

void PorVblank() {
	// Drenaje por cuadro: el respaldo de 60/s aunque el mainloop no haya
	// pisado el hook de Advance (no deberia pasar, pero un silencio no es un
	// dato: este drenaje es incondicional si hay trabajo).
	if (HayTerminaciones())
		Drenar();

	// Releida cada vblank (a diferencia del contador de epi: aca la llamada ya
	// es 1/cuadro, la prop cuesta ~nada y el A/B conmuta al toque).
	int deseado = ResolverNivel();
	// E1 solo cubre los backends HW (los candados viven en GPUCommon/HW y en
	// EmuScreen); con el renderer por software el worker queda deshabilitado
	// en vez de correr sin candados en la mitad de las entradas.
	if (g_Config.bSoftwareRendering)
		deseado = 0;
	const int activo = NivelActivo();
	if (deseado == activo)
		return;

	if (deseado > 0 && !g_workerCreado.load(std::memory_order_relaxed)) {
		CrearWorker();
	}

	// Frontera segura: si el worker esta ocupado o con orden pendiente, no se
	// conmuta; se reintenta el proximo vblank (el worker queda idle entre
	// pasadas, asi que la ventana aparece enseguida).
	{
		std::lock_guard<std::mutex> lk(g_muCola);
		if (g_corriendo || g_ordenPendiente)
			return;
	}
	// Worker idle y solo este hilo puede despertarlo: drenar deja las colas
	// vacias de verdad antes de mover la palanca.
	Drenar();
	g_nivelActivo.store(deseado, std::memory_order_relaxed);

	// PorVblank corre EN el EmuThread: fijarlo al cluster grande junto con el
	// worker (ver "Afinidad" arriba), y devolverle las 8 CPUs al apagar.
	AfinidadCluster(deseado > 0);

	if (!g_hola && deseado > 0) {
		g_hola = true;
		EmitirEstado("ACTIVO", deseado);
	} else {
		char b[64];
		snprintf(b, sizeof(b), "NIVEL %d->%d ahora", activo, deseado);
		EmitirEstado(b, deseado);
	}
}

// --- Apagado -----------------------------------------------------------------

void Apagar() {
	if (g_workerCreado.load(std::memory_order_relaxed)) {
		{
			std::lock_guard<std::mutex> lk(g_muCola);
			g_apagando = true;
		}
		g_cvOrden.notify_one();
		g_worker.join();
		g_workerCreado.store(false, std::memory_order_relaxed);
		g_apagando = false;
		EmitirEstado("APAGADO", NivelActivo());
	}

	// La PSP se esta apagando: una terminacion sin materializar ya no tiene a
	// quien despertar, pero descartarla en silencio esconderia un bug de
	// drenaje. Se cuenta y se dice.
	size_t descartadas = 0;
	{
		std::lock_guard<std::mutex> lk(g_muCola);
		descartadas = g_terminaciones.size();
		g_terminaciones.clear();
		g_ordenPendiente = false;
		g_hayTerminaciones.store(false, std::memory_order_relaxed);
	}
	if (descartadas) {
		char b[128];
		snprintf(b, sizeof(b), "STV: ge apagado con %u terminaciones sin drenar (revisar)",
			(unsigned)descartadas);
		Emitir(b);
	}

	g_nivelActivo.store(0, std::memory_order_relaxed);
	g_hola = false;
	g_pasadas.store(0, std::memory_order_relaxed);
	g_triggers.store(0, std::memory_order_relaxed);
	g_drenajes.store(0, std::memory_order_relaxed);
	g_esperasSync.store(0, std::memory_order_relaxed);
}

}  // namespace stvge
