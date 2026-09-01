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
#include <vector>
#include <chrono>
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
	// STV parche 10: sin env ni prop manda la config (checkbox "Hilo del GE
	// (STV)" en Graficos, PER_GAME). El usuario solo elige on/off: on = nivel
	// 2 (asincrono, el que rinde); el nivel 1 es de validacion y queda para
	// la prop del banco. PorVblank ya relee esto cada cuadro y conmuta en
	// frontera segura: el checkbox actua en caliente.
	return g_Config.bStvWorkerGE ? 2 : kNivelDefecto;
}

static void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

// --- Gate de gracia del arranque ---------------------------------------------
//
// MEDIDO en el aparato: 20+ tombstones, TODOS con process-uptime 4-13 s y
// NINGUNO despues; con nivel 0 jamas crashea. La ventana coincide con la
// compilacion inicial de pipelines: las pasadas del arranque son largas y
// cruzan la frontera de cuadro (la carrera real y su fix viven en
// EmuScreen.cpp, cadena EndHostFrame — Barrera antes de Finish/Present).
// Este gate es la mitigacion que el daemon externo ya probo, ahora horneada:
// el worker NO se activa hasta que el juego lleva kGraciaSegundosDefecto
// corriendo.
//
// Override sin recompilar: prop debug.stv.ge.gracia (env STV_GE_GRACIA, que
// manda sobre la prop, mismo contrato que ResolverNivel) en SEGUNDOS. "0"
// explicito = sin gracia (para aislar el fix real en el banco); texto no
// numerico o ausente = default. El log dice el objetivo en VBLANKS para que
// nadie confunda las unidades.
// DEFAULT 0 DESDE 2026-08-31 (antes 20). El gate era el "cinturon" que entro
// en el MISMO commit que la barrera (cc286c85a): la barrera arreglaba la
// carrera real y el gate quedaba de seguro. Nunca se reviso si seguia
// haciendo falta, y costaba caro: se cuenta en VBLANKS, asi que con el juego
// por debajo de 60 VPS son ~29 SEGUNDOS de reloj en los que el worker no
// existe (medido en Spiderman 3: mediana 43,7 durante la gracia contra 57,5
// despues).
//
// POR QUE 0 Y NO UN VALOR CHICO: las caidas historicas ocurrian con
// process-uptime 4-13 s. Una gracia de 3 o 5 segundos no habria evitado
// NINGUNA — seria costo sin cobertura. O cubre la ventana entera (20 s) o no
// cubre nada; y si la barrera funciona, no hay ventana que cubrir.
//
// EVIDENCIA (2026-08-31): 24 arranques EN FRIO con gracia=0 (8 por juego) en
// GhostOfSparta, ChainsOfOlympus y Spiderman 3, cero caidas. Los dos primeros
// son justamente los del reporte original (CoO: 7 de 7 intentos del usuario
// terminaban en tombstone; GoS: "casi determinista"). GhostOfSparta ademas NO
// tiene .vkshadercache sembrada en la imagen, o sea que sus pipelines nacen
// FRIOS — que es la condicion exacta que disparaba la carrera. El detector de
// caidas se valido inyectando un cierre real antes de creerle a los ceros.
//
// EL MECANISMO SE CONSERVA ENTERO. Si alguna vez reaparece algo, se restaura
// en caliente y sin recompilar:  setprop debug.stv.ge.gracia 20
inline constexpr int kGraciaSegundosDefecto = 0;
inline constexpr int kVblanksPorSegundo = 60;  // vblank PSP: 59,94 ~ 60/s

// Vblanks que lleva el juego ACTUAL. Lo incrementa PorVblank (EmuThread) y lo
// resetea Apagar — que GPU_Shutdown llama en CADA teardown de juego
// (PSP_Shutdown -> GPU_Shutdown, Core/System.cpp:772), tambien al cambiar de
// juego o reiniciar en el mismo proceso. Atomico relaxed: el teardown de un
// boot fallido puede llegar desde el loader thread.
static std::atomic<uint32_t> g_vblanksDeJuego{0};
static std::atomic<bool> g_graciaAnunciada{false};

// strtol + clamp 0..3600 s: atoi con texto enorme es UB con signo y un
// overflow silencioso podria dejar el gate apagado sin aviso.
static int GraciaSegundosDeTexto(const char *t) {
	if (!t || *t < '0' || *t > '9')
		return kGraciaSegundosDefecto;
	long s = strtol(t, nullptr, 10);
	if (s < 0) s = 0;
	if (s > 3600) s = 3600;
	return (int)s;
}

static int GraciaEnVblanks() {
	const char *e = getenv("STV_GE_GRACIA");
	if (e && *e)
		return GraciaSegundosDeTexto(e) * kVblanksPorSegundo;
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.ge.gracia", prop) > 0 && prop[0])
		return GraciaSegundosDeTexto(prop) * kVblanksPorSegundo;
#endif
	return kGraciaSegundosDefecto * kVblanksPorSegundo;
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

// Declarada aca porque la pasada del worker (mas abajo) la usa antes de su
// definicion, que vive junto al resto del mecanismo de diferido.
// Puntero al lock de la pasada del worker: lo publica el propio worker antes
// de entrar a ProcessDLQueue y lo retira al salir. Solo se usa bajo EnWorker().
static std::unique_lock<std::recursive_mutex> *g_lockPasada = nullptr;

static void DrenarInvalidaciones();
static void DrenarLimpiezas();

static void WorkerMain() {
	SetCurrentThreadName("STVGeWorker");
	tl_enWorker = true;
	stvmed::MarcarRol(stvmed::ROL_WORKER);  // STV_MEDIDOR_ESPERAS_v1
	AfinidadCluster(true);

	RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::unique_lock<std::mutex> lk(g_muCola);
	while (true) {
		{
			// STV: worker OCIOSO. Espera LEGITIMA (no hay lista que correr).
			// Es el complemento de w_pasada: si w_orden es grande, el worker no
			// es el cuello; si es chico, el EmuThread lo esta persiguiendo.
			stvmed::Cronometro c(stvmed::R_W_ORDEN);
			g_cvOrden.wait(lk, [] { return g_ordenPendiente || g_apagando; });
		}
		if (g_apagando)
			break;

		// El tick viajo con la orden; es el startingTicks de la pasada (misma
		// formula que inline: atTicks = tick del posteo + cyclesExecuted).
		tl_tickOrden = g_tickOrden;
		g_ordenPendiente = false;
		g_corriendo = true;
		rastroCola.soltar();   // el rastro sigue al candado REAL
		lk.unlock();

		{
			// El candado grueso, de punta a punta de la pasada: mientras el
			// worker ejecuta, el EmuThread no entra a territorio GPU.
			// STV_MEDIDOR_ESPERAS_v1: se parte en dos — lo que cuesta CONSEGUIR
			// el candado (espera al EmuThread) y lo que dura la pasada (trabajo).
			std::unique_lock<std::recursive_mutex> ge(g_mu, std::defer_lock);
			{
				RastreoCandado rastro(kCandGe, "worker::pasada");
				stvmed::Cronometro c(stvmed::R_W_CANDADO);
				ge.lock();
			}
			RastreoCandado rastroPasada(kCandGe, "worker::pasada-vivo");
			stvmed::Cronometro cPasada(stvmed::R_W_PASADA);
			g_lockPasada = &ge;
			DLResult r = gpu->ProcessDLQueue();
			g_lockPasada = nullptr;
			// DebugBreak es imposible aca: el dispatch degrada a inline
			// cuando hay debugger/recorder activo (StvGeExigeInline).
			_dbg_assert_(r != DLResult::DebugBreak);
			(void)r;
			// Con g_mu TODAVIA tomado: aplicar lo que el EmuThread encolo
			// mientras esta pasada corria. El orden resultante es el mismo que
			// tenia el camino bloqueante (la invalidacion caia despues de la
			// pasada), sin la parada del EmuThread.
			DrenarInvalidaciones();
			// Misma ventana y misma razon: con g_mu todavia tomado, aplicar lo
			// que el EmuThread encolo para no quedarse esperando esta pasada.
			DrenarLimpiezas();
		}
		g_pasadas.fetch_add(1, std::memory_order_relaxed);

		lk.lock();
		rastroCola.retomar("g_muCola");
		g_corriendo = false;
		if (!g_ordenPendiente)
			g_cvIdle.notify_all();
	}
}


// --- Invalidaciones diferidas ------------------------------------------------
//
// Ver el porque en StvGeThread.h. Aca solo el mecanismo.
//
// ORDEN DE CANDADOS: g_muInval es SIEMPRE el mas interno. El worker lo toma
// teniendo g_mu (para drenar); el EmuThread lo toma justo DESPUES de fallar en
// conseguir g_mu, o sea sin tenerlo. No hay ciclo posible.
struct InvalPend { uint32_t addr; int size; int type; };
static std::mutex g_muInval;
static std::vector<InvalPend> g_invalPend;
// Tope: si la cola se llena (el worker no llega a drenar), NO se pierde una
// invalidacion — se cae al camino bloqueante de siempre. Perder una es
// corrupcion de texturas; esperar es solo lento.
inline constexpr size_t kMaxInval = 512;
static std::atomic<uint64_t> g_invalDiferidas{0};
static std::atomic<size_t> g_invalCuenta{0};   // espejo barato de g_invalPend.size()
static std::atomic<uint64_t> g_invalDirectas{0};

// Valvula: 1 = diferir (DEFAULT), 0 = upstream exacto. Misma regla del typo que
// el resto: un caracter no-digito vale el default, no cero.
//
// ARRANCA ENCENDIDA porque esta medido y validado en las tres dimensiones:
//   rendimiento  Spiderman 3 escena del incendio  44,7 -> 59,8 VPS de mediana
//                (p95 42,4 -> 44,7 ; p99 42,0 -> 42,8), y el testigo confirma
//                que cand_invalidate cae de 8,9 ms/cuadro a 0,015.
//   no-regresion God of War escena de la explosion 59,9 -> 60,0 (p99 51,6 ->
//                53,9). El worker ya sumaba ahi; el diferido no le quita nada.
//   imagen       capturas de la misma escena: la diferencia contra el control
//                queda DENTRO del ruido de dos controles identicos entre si
//                (0,54 contra 0,49 de diferencia media; el fuego se anima, asi
//                que ni dos capturas iguales coinciden).
// Se apaga con debug.stv.inval=0 sin recompilar.
inline constexpr int kInvalDefecto = 1;
static int InvalDeTexto(const char *s) {
	if (!s || !*s) return kInvalDefecto;
	if (*s < '0' || *s > '9') return kInvalDefecto;
	return (*s - '0') ? 1 : 0;
}
static int g_invalNivel = kInvalDefecto;
static uint32_t g_invalRevision = 0;
static uint32_t g_invalAnuncio = 0;
inline constexpr uint32_t kInvalAnuncioCada = 10;   // 10 * 32 cuadros ~ 5 s
inline constexpr uint32_t kInvalFramesRevision = 32;

static int ResolverInval() {
	const char *e = getenv("STV_GE_INVAL");
	if (e && *e)
		return InvalDeTexto(e);
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.inval", prop) > 0 && prop[0])
		return InvalDeTexto(prop);
#endif
	return kInvalDefecto;
}

bool DiferirInvalidacion(uint32_t addr, int size, int type) {
	if (g_invalNivel == 0 || NivelActivo() == 0)
		return false;
	// Solo vale la pena diferir si el que retiene el candado es una PASADA del
	// worker: es la unica que lo tiene milisegundos. Cualquier otro tenedor lo
	// suelta enseguida, y ahi bloquear sale mas barato que encolar.
	{
		RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::lock_guard<std::mutex> lk(g_muCola);
		if (!g_corriendo)
			return false;
	}
	// try_lock sobre el recursivo: si este mismo hilo YA lo tiene (el worker
	// drenando, o una entrada anidada), devuelve exito y hacemos el camino
	// normal — que es lo correcto, no hay a quien esperar.
	if (g_mu.try_lock()) {
		g_mu.unlock();
		g_invalDirectas.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	{
		RastreoCandado rastroInv(kCandInval, "g_muInval");
		std::lock_guard<std::mutex> lk(g_muInval);
		if (g_invalPend.size() >= kMaxInval)
			return false;
		g_invalPend.push_back({ addr, size, type });
		g_invalCuenta.store(g_invalPend.size(), std::memory_order_relaxed);
	}
	g_invalDiferidas.fetch_add(1, std::memory_order_relaxed);
	return true;
}

// La llama el worker al TERMINAR la pasada, TODAVIA con g_mu tomado. Reentra
// por gpu->InvalidateCache, y ahi DiferirInvalidacion devuelve false (try_lock
// tiene exito: es el mismo hilo), asi que se aplican por el camino normal.
static void DrenarInvalidaciones() {
	std::vector<InvalPend> lote;
	{
		RastreoCandado rastroInv(kCandInval, "g_muInval");
		std::lock_guard<std::mutex> lk(g_muInval);
		if (g_invalPend.empty())
			return;
		lote.swap(g_invalPend);
		g_invalCuenta.store(0, std::memory_order_relaxed);
	}
	if (!gpu)
		return;
	for (const InvalPend &i : lote)
		gpu->InvalidateCache(i.addr, i.size, (GPUInvalidationType)i.type);
}


// --- Limpieza de lista diferida (ver el porque en StvGeThread.h) -------------
static std::mutex g_muLimpieza;
static std::vector<int> g_limpiezaPend;
FnLimpiarLista g_alLimpiarLista = nullptr;
inline constexpr size_t kMaxLimpieza = 64;

bool DiferirLimpiezaLista(int listid) {
	if (NivelActivo() == 0 || !g_alLimpiarLista)
		return false;
	// Solo vale diferir si el que retiene el candado es una PASADA del worker:
	// es la unica que lo tiene milisegundos. Cualquier otro lo suelta enseguida
	// y ahi bloquear sale mas barato que encolar.
	{
		RastreoCandado rastroCola(kCandCola, "g_muCola");
		std::lock_guard<std::mutex> lk(g_muCola);
		if (!g_corriendo)
			return false;
	}
	// try_lock sobre el recursivo: si este hilo ya lo tiene, o esta libre, el
	// camino normal es lo correcto — no hay a quien esperar.
	if (g_mu.try_lock()) {
		g_mu.unlock();
		g_limpiezasDirectas.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(g_muLimpieza);
		if (g_limpiezaPend.size() >= kMaxLimpieza)
			return false;   // desbordada: mejor bloquear que perder la limpieza
		g_limpiezaPend.push_back(listid);
	}
	g_limpiezasDiferidas.fetch_add(1, std::memory_order_relaxed);
	return true;
}

static void DrenarLimpiezas() {
	std::vector<int> lote;
	{
		std::lock_guard<std::mutex> lk(g_muLimpieza);
		if (g_limpiezaPend.empty())
			return;
		lote.swap(g_limpiezaPend);
	}
	if (!g_alLimpiarLista)
		return;
	for (int id : lote)
		g_alLimpiarLista(id);
}

// --- Cesion en frontera de lista ---------------------------------------------
//
// Ver el porque en StvGeThread.h. El puntero al lock de la pasada lo publica el
// worker antes de entrar a ProcessDLQueue y lo retira al salir; nadie mas lo
// toca (solo se usa bajo EnWorker()).
static std::atomic<uint64_t> g_cesiones{0};
static std::atomic<uint64_t> g_cesionesSaltadas{0};

inline constexpr int kCederDefecto = 0;
static int CederDeTexto(const char *s) {
	if (!s || !*s) return kCederDefecto;
	if (*s < '0' || *s > '9') return kCederDefecto;
	return (*s - '0') ? 1 : 0;
}
static int g_cederNivel = kCederDefecto;
static uint32_t g_cederRevision = 0;
static uint32_t g_dlRevision = 0;
static uint32_t g_dlAnuncio = 0;
static uint32_t g_cederAnuncio = 0;

static int ResolverCeder() {
	const char *e = getenv("STV_GE_CEDER");
	if (e && *e)
		return CederDeTexto(e);
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.ceder", prop) > 0 && prop[0])
		return CederDeTexto(prop);
#endif
	return kCederDefecto;
}

void CederEnFronteraDeLista() {
	if (g_cederNivel == 0 || !g_lockPasada)
		return;
	if (!EnWorker())
		return;
	// Con un CandadoGe anidado vivo, soltar UNA vez no libera el recursivo y
	// ademas romperia su invariante. Se cuenta, no se supone.
	if (g_candadosVivos != 0) {
		g_cesionesSaltadas.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// Nadie esperando: ceder seria pagar un sched_yield por lista a cambio de
	// nada. Con carga real esto es lo normal entre rafagas.
	if (g_esperandoGe.load(std::memory_order_relaxed) == 0)
		return;
	g_lockPasada->unlock();
	std::this_thread::yield();
	g_lockPasada->lock();
	g_cesiones.fetch_add(1, std::memory_order_relaxed);
}


static std::atomic<uint64_t> g_cesionesCmd{0};
// Un cero sin explicacion no es un dato: se cuenta CUAL guarda freno.
static std::atomic<uint64_t> g_ccMiro{0}, g_ccNoWorker{0}, g_ccAnidado{0}, g_ccNadieEspera{0};
static int g_cederCmd = 0;
static int g_cederCmdRevision = 0;

static int ResolverCederCmd() {
	const char *e = getenv("STV_GE_CEDER_CMD");
	if (e && *e) return (*e >= '1' && *e <= '9') ? 1 : 0;
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.ceder.cmd", prop) > 0 && prop[0])
		return (prop[0] >= '1' && prop[0] <= '9') ? 1 : 0;
#endif
	// ENCENDIDA POR DEFECTO desde f25. Ranura 5, 4 corridas por lado:
	//   apagada:  51,6 · 57,7 · 47,5 · 58,2   mediana 54,1  p95 43,5  %bajo50 47,3
	//   encendida: 59,9 · 59,9 · 59,9 · 59,9  mediana 59,9  p95 50,0  %bajo50  4,6
	// Las cuatro encendidas dan el TECHO, planas. Y la espera que ataca cae de
	// 5,52 ms a ~1 ms por cuadro.
	// Regresion en la ranura 4: mediana igual (58,7 -> 58,3) y la COLA mejora
	// (p95 52,0 -> 55,2, minimo 50,8 -> 54,6). Cero caidas, cero colisiones.
	// Se apaga con: setprop debug.stv.ceder.cmd 0
	return 1;
}

void CederEnComando() {
	if (g_cederCmd == 0 || !g_lockPasada)
		return;
	g_ccMiro.fetch_add(1, std::memory_order_relaxed);
	if (!EnWorker()) {
		g_ccNoWorker.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// Con un CandadoGe anidado vivo, soltar UNA vez no libera el recursivo.
	if (g_candadosVivos != 0) {
		g_ccAnidado.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// SOLO por EnqueueList. Cualquier otro que espere el candado grueso puede
	// tocar estado de GPU, y dejarlo entrar a mitad de lista es justo el ciclo
	// que colgo la consola en los intentos anteriores del candado fino.
	if (g_esperandoEncola.load(std::memory_order_relaxed) == 0) {
		g_ccNadieEspera.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	g_lockPasada->unlock();
	std::this_thread::yield();
	g_lockPasada->lock();
	g_cesionesCmd.fetch_add(1, std::memory_order_relaxed);
}

// --- Testigo de exclusion: prueba de sensibilidad ----------------------------
//
// Un contador en cero no prueba nada si nunca comprobamos que sabe subir. Esto
// lanza un hilo que entra a la zona protegida mientras el hilo que llama esta
// adentro: si el testigo esta sano, la colision queda contada. Se dispara UNA
// vez, con debug.stv.dl.probar=1, y se apaga sola.
static std::atomic<bool> g_probandoDL{false};
void ProbarTestigoDL() {
	bool esperado = false;
	if (!g_probandoDL.compare_exchange_strong(esperado, true))
		return;
	Emitir("STV: dl PROBANDO el testigo (se espera 1 colision)");
	std::thread([]{
		SetCurrentThreadName("StvProbaDL");
		TestigoDL dentro("PRUEBA-hilo");        // entra el hilo de prueba
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	}).detach();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	{
		TestigoDL dentro("PRUEBA-encima");      // y entra este, encima
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	char b[160];
	snprintf(b, sizeof(b), "STV: dl prueba hecha: colisiones=%llu pico=%d",
		(unsigned long long)g_colisionesDL.load(std::memory_order_relaxed),
		g_picoDL.load(std::memory_order_relaxed));
	Emitir(b);
}


// --- Valvula del candado fino ------------------------------------------------
//
// En 0 (DEFAULT) el manejador de interrupciones sigue tomando el candado GRUESO
// ademas del fino: comportamiento identico a hoy. En 1 toma SOLO el fino y deja
// de esperar la pasada del worker — que es la ganancia.
//
// El candado fino se toma en los 24 sitios en LOS DOS casos, a proposito: asi
// el camino nuevo se ejercita siempre y una carrera aparece en el testigo
// aunque la valvula este apagada.
static int g_dlFino = 0;
static uint32_t g_dlFinoRevision = 0;

// DESACTIVADA A LA FUERZA (2026-08-31). El diseño esta REFUTADO y encenderla
// CUELGA la consola: los tres hilos quedan en futex y el proceso vivo pero
// congelado. La razon no es un descuido de candados sino la estructura del
// codigo: GeIntrHandler llama a InterruptEnd, y esa funcion hace
// gstate.Restore(dl.context) + ReapplyGfxState() — o sea ESTADO DE GPU, no
// contabilidad de listas. Por eso necesita el candado grueso de verdad, y si el
// manejador ya tiene el fino tomado se arma el ciclo fino->grueso contra el
// grueso->fino del worker.
//
// La premisa era cierta para run() y FALSA para el camino completo. Se deja el
// codigo (los 24 sitios protegidos + el testigo) porque no cuesta nada y sirve
// de base cuando se saque el Restore del camino de la interrupcion, que es el
// arreglo de fondo. Pero la valvula NO puede quedar como gatillo en una imagen.
// CLAVADA EN 0 otra vez (2026-08-31, segundo intento). El diseño de dos fases
// arreglo tres ciclos reales (DrawSync/ListSync invertidos, InterruptEnd sin
// necesitar el grueso, StvGeDespacharCola sosteniendo el fino sobre Barrera) y
// AUN ASI cuelga. Queda al menos un cuarto camino sin identificar.
//
// Lo que SI quedo probado y sirve: los 24 sitios protegidos, el testigo (0
// colisiones en 276.468 entradas con el candado grueso), y que el camino comun
// de InterruptEnd es contabilidad pura (16.638 llamadas: pop=0, conGpu=0).
// Retomar desde aca con un rastreador de orden de candados, no a ojo.
// CLAVADA EN 0. Estado al 2026-08-31: el ORDEN de candados quedo limpio (el
// rastreador reporta 0 ciclos con la valvula encendida), pero el camino de dos
// fases lanza std::system_error desde GeIntrHandler::handleResult — un unlock
// sobre un candado que no se posee, o un lock sobre uno ya tomado. Es un error
// de LOGICA mio, no de orden, y hay que corregirlo antes de volver a encenderla.
// DESCLAVADA (2026-09-01) — con dos motivos, y ninguno es optimismo:
//
// 1. El motivo del ultimo clavado YA NO EXISTE. Decia: "el orden quedo limpio
//    (0 ciclos) pero el camino de dos fases lanza std::system_error desde
//    handleResult — un unlock sobre un candado que no se posee". Ese error se
//    encontro y se corrigio despues: eran LLAVES FALTANTES en sceGe.cpp, que
//    hacian correr el unlock siempre aunque el grueso nunca se hubiera tomado.
//    El sintoma descrito y el bug corregido son el mismo.
//
// 2. El veredicto de "cero ganancia" que le siguio se midio ANTES de descubrir
//    que dos de las tres escenas de Spiderman no sirven para comparar: la
//    ranura 3 oscila 37..60 por fase y la ranura 2 ya esta en el techo. Ese
//    veredicto no distingue "no sirve" de "se midio donde no se ve".
//
// Y lo que hoy dice el medidor en la escena que SI sirve (ranura 4):
//    wall 19,4 ms = cpuEmu 16,4 + esperaEmu 3,1 + resto -0,2
//    cand_interrupt = 2,98 ms/cuadro, 6 veces, pico 3,7 ms
// O sea: el 95 % de toda la espera del hilo de emulacion es exactamente lo que
// este candado fino existe para quitar. Son 3 ms de 19,4 — recuperarlos lleva
// el cuadro a 16,4 ms, que es 60 VPS.
//
// Sigue siendo VALVULA, apagada por defecto: en 0 el comportamiento es
// identico al de hoy. Historial completo del clavado, arriba.
bool DLFinoActivo() { return g_dlFino != 0; }

static int ResolverDLFino() {
	const char *e = getenv("STV_GE_DLFINO");
	if (e && *e) return (*e >= '1' && *e <= '9') ? 1 : 0;
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.dlfino", prop) > 0 && prop[0])
		return (prop[0] >= '1' && prop[0] <= '9') ? 1 : 0;
#endif
	// ENCENDIDO POR DEFECTO desde f24. Medido en la ranura 4, 4 corridas por
	// lado: mediana 53,3 -> 58,1, p95 49,8 -> 55,1, %bajo50 6,7 -> 0,0. Hacen
	// falta LOS DOS (esto y el diferimiento de InterruptEnd): el diferimiento
	// solo da 53,3, o sea nada, porque con esto apagado los otros tres sitios
	// del camino de interrupciones siguen tomando el candado grueso y
	// esperando la pasada del worker igual.
	// Validado por el usuario: sesion larga de juego, 5 h de uptime, CERO
	// caidas, cero errores del GE, 33,4 M de entradas al candado fino con CERO
	// colisiones, y sin artefactos graficos.
	// Se apaga con: setprop debug.stv.dlfino 0
	return 1;
}


// Aviso INMEDIATO del ciclo. Va por kmsg apenas se detecta, no en el testigo
// periodico: si el ciclo termina en cuelgue, un reporte que llega "cada 5 s"
// no llega nunca. Se dice una sola vez por par para no inundar.
void AvisarCiclo(int a, int b, const char *sitioNuevo, const char *sitioViejo) {
	char t[256];
	snprintf(t, sizeof(t),
		"STV: *** CICLO DE CANDADOS *** %s -> %s en '%s'  CONTRA  %s -> %s visto en '%s'",
		NombreCandado(a), NombreCandado(b), sitioNuevo ? sitioNuevo : "?",
		NombreCandado(b), NombreCandado(a), sitioViejo ? sitioViejo : "?");
	Emitir(t);
}

static void CrearWorker() {
	g_apagando = false;
	g_worker = std::thread(&WorkerMain);
	g_workerCreado.store(true, std::memory_order_relaxed);
}

// --- Ordenes y triggers ------------------------------------------------------

static void PostearRun(uint64_t tick) {
	{
		RastreoCandado rastroCola(kCandCola, "g_muCola");
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
	RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::lock_guard<std::mutex> lk(g_muCola);
	g_terminaciones.push_back(Trigger{ Clase::Interrupt, listid, pc, atTicks });
	g_triggers.fetch_add(1, std::memory_order_relaxed);
	g_hayTerminaciones.store(true, std::memory_order_relaxed);
}

void PostearSync(int type, int id, uint64_t atTicks) {
	RastreoCandado rastroCola(kCandCola, "g_muCola");
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
		RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::lock_guard<std::mutex> lk(g_muCola);
		lote.swap(g_terminaciones);
		// Dentro del candado: un posteo del worker posterior al swap vuelve a
		// levantar la bandera despues de este store, nunca antes.
		g_hayTerminaciones.store(false, std::memory_order_relaxed);
	}
	if (lote.empty())
		return;
	g_drenajes.fetch_add(1, std::memory_order_relaxed);
	// STV: el drenaje es TRABAJO del EmuThread, no espera. Se cronometra para
	// poder descontarlo del residuo y que resto= signifique lo que dice.
	stvmed::Cronometro cDren(stvmed::R_DRENAJE);

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
static void EsperarIdleInterno(bool contarEspera, stvmed::Ranura ranura = stvmed::R_ESPERA_IDLE) {
	if (!g_workerCreado.load(std::memory_order_relaxed))
		return;
	RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::unique_lock<std::mutex> lk(g_muCola);
	if (g_corriendo || g_ordenPendiente) {
		if (contarEspera)
			g_esperasSync.fetch_add(1, std::memory_order_relaxed);
		// STV_MEDIDOR_ESPERAS_v1: el hilo que llama se para hasta que el worker
		// termina la pasada Y no quedan ordenes. La ranura la pone el caller.
		stvmed::Cronometro c(ranura);
		g_cvIdle.wait(lk, [] { return !g_corriendo && !g_ordenPendiente; });
	}
}

void EsperarIdle() {
	EsperarIdleInterno(false);
}

void Barrera(stvmed::Ranura ranura) {
	EsperarIdleInterno(false, ranura);
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
	EsperarIdleInterno(true, stvmed::R_GE_SYNC);
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
		"STV: ge %s nivel=%d (%s) gracia=%ds pasadas=%u triggers=%u drenajes=%u esperas=%u",
		encabezado, nivel, kMarca, GraciaEnVblanks() / kVblanksPorSegundo,
		g_pasadas.load(std::memory_order_relaxed),
		g_triggers.load(std::memory_order_relaxed),
		g_drenajes.load(std::memory_order_relaxed),
		g_esperasSync.load(std::memory_order_relaxed));
	Emitir(b);
}


// RED DE SEGURIDAD. El worker drena al final de CADA pasada, asi que solo puede
// quedar algo encolado si el EmuThread encolo despues del ultimo drenaje y el
// worker ya no volvio a correr (por ejemplo porque se apago la palanca). En ese
// caso el worker esta idle, g_mu esta libre, y este drenaje es instantaneo.
// Sin esto, apagar el worker con la cola cargada dejaria texturas rancias.
static void DrenarInvalidacionesSiQuedaron() {
	if (g_invalCuenta.load(std::memory_order_relaxed) == 0)
		return;
	{
		RastreoCandado rastroCola(kCandCola, "g_muCola");
	std::lock_guard<std::mutex> lk(g_muCola);
		if (g_corriendo || g_ordenPendiente)
			return;   // el worker las va a drenar el solo al terminar
	}
	std::lock_guard<std::recursive_mutex> ge(g_mu);
	DrenarInvalidaciones();
}

void PorVblank() {
	DrenarInvalidacionesSiQuedaron();
	// Drenaje por cuadro: el respaldo de 60/s aunque el mainloop no haya
	// pisado el hook de Advance (no deberia pasar, pero un silencio no es un
	// dato: este drenaje es incondicional si hay trabajo).
	if (HayTerminaciones())
		Drenar();

	// Releida cada vblank (a diferencia del contador de epi: aca la llamada ya
	// es 1/cuadro, la prop cuesta ~nada y el A/B conmuta al toque).
	const uint32_t vb = g_vblanksDeJuego.fetch_add(1, std::memory_order_relaxed) + 1;
	if (++g_cederCmdRevision >= kInvalFramesRevision) {
		g_cederCmdRevision = 0;
		const int antes = g_cederCmd;
		g_cederCmd = ResolverCederCmd();
		if (g_cederCmd != antes) {
			char b[128];
			snprintf(b, sizeof(b), "STV: ceder en comando %s (debug.stv.ceder.cmd)", g_cederCmd ? "ENCENDIDO" : "apagado");
			Emitir(b);
		}
	}
	if (++g_dlFinoRevision >= kInvalFramesRevision) {
		g_dlFinoRevision = 0;
		const int antes = g_dlFino;
		g_dlFino = ResolverDLFino();
		if (g_dlFino != antes) {
			char b[144];
			snprintf(b, sizeof(b), "STV: dlfino %s (debug.stv.dlfino)", g_dlFino ? "ENCENDIDO" : "apagado");
			Emitir(b);
		}
	}
	if (++g_dlRevision >= kInvalFramesRevision) {
		g_dlRevision = 0;
#if defined(__ANDROID__)
		char prop[PROP_VALUE_MAX] = { 0 };
		if (__system_property_get("debug.stv.dl.probar", prop) > 0 && prop[0] == '1')
			ProbarTestigoDL();
#endif
		if (++g_dlAnuncio >= kInvalAnuncioCada) {
			g_dlAnuncio = 0;
			char b[320];
			const char *sChoque = g_choqueSitio.load(std::memory_order_relaxed);
			const char *sDuenio = g_choqueDueñoSitio.load(std::memory_order_relaxed);
			snprintf(b, sizeof(b),
				"STV: dl ciclos=%llu intrEnd=%llu compl=%llu pop=%llu conGpu=%llu fino=%d limpDif=%llu limpDir=%llu cesCmd=%llu/miro=%llu/noW=%llu/anid=%llu/nadie=%llu entradas=%llu COLISIONES=%llu pico=%d dentro=%d ultima: tid=%d en %s CHOCO contra %s",
				(unsigned long long)g_ciclos.load(std::memory_order_relaxed),
				(unsigned long long)g_intrEndTotal.load(std::memory_order_relaxed),
				(unsigned long long)g_intrEndCompletada.load(std::memory_order_relaxed),
				(unsigned long long)g_intrEndPop.load(std::memory_order_relaxed),
				(unsigned long long)g_intrEndConGpu.load(std::memory_order_relaxed),
				g_dlFino,
				(unsigned long long)g_limpiezasDiferidas.load(std::memory_order_relaxed),
				(unsigned long long)g_limpiezasDirectas.load(std::memory_order_relaxed),
				(unsigned long long)g_cesionesCmd.load(std::memory_order_relaxed),
				(unsigned long long)g_ccMiro.load(std::memory_order_relaxed),
				(unsigned long long)g_ccNoWorker.load(std::memory_order_relaxed),
				(unsigned long long)g_ccAnidado.load(std::memory_order_relaxed),
				(unsigned long long)g_ccNadieEspera.load(std::memory_order_relaxed),
				(unsigned long long)g_entradasDL.load(std::memory_order_relaxed),
				(unsigned long long)g_colisionesDL.load(std::memory_order_relaxed),
				g_picoDL.load(std::memory_order_relaxed),
				g_dentroDL.load(std::memory_order_relaxed),
				g_choqueTid.load(std::memory_order_relaxed),
				sChoque ? sChoque : "-", sDuenio ? sDuenio : "-");
			Emitir(b);
		}
	}
	if (++g_cederRevision >= kInvalFramesRevision) {
		g_cederRevision = 0;
		const int antesCeder = g_cederNivel;
		g_cederNivel = ResolverCeder();
		if (g_cederNivel != antesCeder) {
			char b[160];
			snprintf(b, sizeof(b), "STV: ceder en frontera %s (debug.stv.ceder)",
				g_cederNivel ? "ENCENDIDO" : "apagado");
			Emitir(b);
		}
		if (++g_cederAnuncio >= kInvalAnuncioCada) {
			g_cederAnuncio = 0;
			char b[160];
			snprintf(b, sizeof(b), "STV: ceder nivel=%d cesiones=%llu saltadas=%llu",
				g_cederNivel,
				(unsigned long long)g_cesiones.load(std::memory_order_relaxed),
				(unsigned long long)g_cesionesSaltadas.load(std::memory_order_relaxed));
			Emitir(b);
		}
	}
	if (++g_invalRevision >= kInvalFramesRevision) {
		g_invalRevision = 0;
		const int antesInval = g_invalNivel;
		g_invalNivel = ResolverInval();
		// TESTIGO. Regla del instrumento mudo: la valvula habla en los DOS
		// estados. Si no aparece ninguna de estas lineas, la lib NO lleva el
		// parche — que es distinto de "la valvula esta en 0".
		if (g_invalNivel != antesInval) {
			char b[160];
			snprintf(b, sizeof(b), "STV: inval diferido %s (debug.stv.inval)",
				g_invalNivel ? "ENCENDIDO" : "apagado");
			Emitir(b);
		}
		if (++g_invalAnuncio >= kInvalAnuncioCada) {
			g_invalAnuncio = 0;
			size_t pend;
			{
				// .size() sobre el vector mientras otro hilo puede estar
				// insertando es una carrera de verdad, no un dato aproximado.
				RastreoCandado rastroInv(kCandInval, "g_muInval");
		std::lock_guard<std::mutex> lk(g_muInval);
				pend = g_invalPend.size();
			}
			char b[192];
			snprintf(b, sizeof(b),
				"STV: inval nivel=%d diferidas=%llu directas=%llu pendientes=%zu",
				g_invalNivel,
				(unsigned long long)g_invalDiferidas.load(std::memory_order_relaxed),
				(unsigned long long)g_invalDirectas.load(std::memory_order_relaxed),
				pend);
			Emitir(b);
		}
	}
	int deseado = ResolverNivel();
	// E1 solo cubre los backends HW (los candados viven en GPUCommon/HW y en
	// EmuScreen); con el renderer por software el worker queda deshabilitado
	// en vez de correr sin candados en la mitad de las entradas.
	if (g_Config.bSoftwareRendering)
		deseado = 0;
	// Gate de gracia: mientras el juego no cumplio la ventana de arranque, el
	// nivel deseado se fuerza a 0 (ver el bloque de kGraciaSegundosDefecto).
	// Se anuncia UNA vez al frenar un nivel pedido > 0 y una vez al cumplirla,
	// para que el testigo cuente la historia completa.
	if (deseado > 0) {
		const int gracia = GraciaEnVblanks();
		if ((int64_t)vb < (int64_t)gracia) {
			if (!g_graciaAnunciada) {
				g_graciaAnunciada = true;
				char b[160];
				snprintf(b, sizeof(b),
					"STV: ge en gracia (vblank %u de %d): nivel 0 forzado hasta cumplirla",
					vb, gracia);
				Emitir(b);
			}
			deseado = 0;
		} else if (g_graciaAnunciada) {
			g_graciaAnunciada = false;
			char b[96];
			snprintf(b, sizeof(b), "STV: ge gracia cumplida en el vblank %u", vb);
			Emitir(b);
		}
	}
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
		RastreoCandado rastroCola(kCandCola, "g_muCola");
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
			RastreoCandado rastroCola(kCandCola, "g_muCola");
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
		RastreoCandado rastroCola(kCandCola, "g_muCola");
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
	// Gate de gracia: GPU_Shutdown pasa por aca en CADA teardown de juego
	// (PSP_Shutdown -> GPU_Shutdown, Core/System.cpp:772), tambien al cambiar
	// de juego o reiniciar en el mismo proceso — el proximo boot arranca su
	// ventana de gracia desde cero.
	g_vblanksDeJuego.store(0, std::memory_order_relaxed);
	g_graciaAnunciada = false;
	g_pasadas.store(0, std::memory_order_relaxed);
	g_triggers.store(0, std::memory_order_relaxed);
	g_drenajes.store(0, std::memory_order_relaxed);
	g_esperasSync.store(0, std::memory_order_relaxed);
}

}  // namespace stvge
