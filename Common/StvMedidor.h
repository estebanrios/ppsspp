// =============================================================================
// STV — MEDIDOR DE ESPERAS DEL CUADRO (instrumento, arco F17).
//
// Parche del proyecto STV-TSPS-Android (tools/ppsspp/parches/diag-esperas.sh).
// NO es codigo de upstream.
//
// QUE MIDE, EN UNA FRASE: cuanto de cada cuadro se pasa CADA hilo del pipeline
// PARADO ESPERANDO A OTRO, y a quien espera.
//
// POR QUE: los contadores de HW de la Mali dicen que la GPU procesa el 55,5 %
// del tiempo y que NINGUNA unidad del shader core pasa de 0,33 de utilizacion.
// O sea: no hay un recurso saturado. El cuadro medido (18,7 ms) es EXACTAMENTE
// GPU (10,4) + resto (8,3): el modelo EN SERIE lo reproduce. La hipotesis es
// que CPU y GPU se TURNAN. Este instrumento la confirma o la refuta poniendo
// numero a cada turno.
//
// EL REPARTO DE HILOS QUE SE INSTRUMENTA (leido del fuente, no supuesto):
//   EmuThread       = emulacion + UI + Finish/Present   (app-android.cpp:1702)
//   STVGeWorker     = ProcessDLQueue bajo g_mu          (StvGeThread.cpp)
//   VulkanRenderMan = graba cmdbufs y submitea          (VulkanRenderManager)
//
// COSTO. Todo el instrumento vive detras de un load relaxed de g_activo:
//   * APAGADO: dos ramas perfectamente predecibles por sitio de espera y CERO
//     lecturas de reloj. El A/B encendido-vs-apagado se corre CON EL MISMO APK.
//   * ENCENDIDO: 2 clock_gettime(CLOCK_MONOTONIC) por espera cronometrada.
//     En aarch64 eso es vDSO (CNTVCT_EL0), ~30 ns. Con un tope generoso de
//     1000 esperas por cuadro son ~60 us sobre 18,7 ms = 0,3 %. Mas 1
//     clock_gettime(CLOCK_THREAD_CPUTIME_ID) por cuadro (ese si es syscall,
//     ~1 us, o sea 0,005 %) y un __android_log_print cada 120 cuadros.
//   * Cero asignaciones, cero strings, cero binder.
//
// LA AUTOCOMPROBACION (regla del proyecto: un testigo no puede mentir).
// El volcado imprime, ademas de las esperas, el CPU REAL del EmuThread en la
// ventana (CLOCK_THREAD_CPUTIME_ID). Tiene que cumplirse:
//     wall  ~=  cpuEmu + esperaEmu  (+ resto pequeno de preempcion)
// El campo resto= es esa diferencia. Si resto= sale grande y positivo, HAY UNA
// ESPERA QUE NO ESTAMOS CRONOMETRANDO — y el instrumento lo dice en vez de
// repartirla entre las ranuras conocidas. Si sale negativo, el EmuThread
// estuvo corriendo dentro de una espera cronometrada (imposible: seria un bug
// del instrumento).
//
// COMO SE ENCIENDE, sin recompilar y sin reiniciar la app (se relee 1 vez por
// segundo, asi que prender/apagar tiene efecto en <= 1 s):
//   1. variable de entorno   STV_MED=1        (banco fuera de Android)
//   2. propiedad de Android  debug.stv.med=1  (adb shell setprop debug.stv.med 1)
// El valor tambien fija la VENTANA: debug.stv.med=N vuelca cada N*60 cuadros
// (1 = 60 cuadros ~ 1,1 s; 2 = 120; ...). 0 = apagado.
//
// Y —regla del instrumento mudo— cuando esta APAGADO tambien habla: emite una
// linea cada 10 s diciendo que esta apagado y como se prende. Un silencio
// total en logcat significa "la lib no lleva el parche", NO "no hubo esperas".
// Las dos cosas quedan distinguibles sin tocar nada.
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#if defined(__linux__)
#include <time.h>

// STV F10b: para leer la velocidad REAL de emulacion (vblanks emulados por
// segundo de pared), que es distinta del ritmo del bucle de presentacion.
#include "Core/HW/Display.h"
#endif

namespace stvmed {

// Cadena que prueba que el parche entro en el .so:
//   strings libppsspp_jni.so | grep STV_MEDIDOR_ESPERAS_v1
inline constexpr const char *kMarca = "STV_MEDIDOR_ESPERAS_v1";

// --- Roles ------------------------------------------------------------------
// Cada hilo se marca UNA vez al arrancar. El rol decide en que cubo cae la
// espera, asi una misma funcion (FlushSync, que la llaman el EmuThread Y el
// worker) queda separada por hilo sin preguntarle a nadie mas.
enum Rol : int { ROL_OTRO = 0, ROL_EMU = 1, ROL_WORKER = 2, ROL_RENDER = 3, NUM_ROL = 4 };

inline const char *const kNombreRol[NUM_ROL] = { "otro", "emu", "worker", "render" };

// --- Ranuras ----------------------------------------------------------------
// Una por punto de bloqueo REAL del pipeline. El orden es el del cuadro.
enum Ranura : int {
	// --- EmuThread: cadena de NativeFrame ---
	R_FENCE_CV = 0,     // BeginFrame: espera a que el render thread haya submiteado (readyForFence)
	R_FENCE_VK,         // BeginFrame: vkWaitForFences del cuadro de hace iInflightFrames
	R_CAND_BEGINHOST,   // g_mu en gpu->BeginHostFrame            (EmuScreen.cpp)
	// --- EmuThread: dentro del run loop (frecuencia de syscall) ---
	R_CAND_ENCOLA,      // g_mu en GPUCommon::EnqueueList         (sceGeListEnQueue)
	R_CAND_STALL,       // g_mu en GPUCommon::UpdateStall         (sceGeListUpdateStallAddr)
	R_GE_SYNC,          // EsperarGeParaSync: DrawSync/ListSync esperando al worker
	R_CAND_FBDIRTY,     // g_mu en FramebufferDirty/ReallyDirty   (__DisplayFlip)
	R_CAND_FLIP,        // g_mu del veredicto de frameskip        (sceDisplay.cpp)
	R_CAND_OTRO,        // cualquier otro CandadoGe sin etiquetar
	R_DRENAJE,          // stvge::Drenar (trabajo real, no espera; sirve de control)
	// --- EmuThread: frontera de cuadro ---
	R_BARRERA_CUADRO,   // stvge::Barrera de EmuScreen.cpp:1835  <- LA GRANDE
	R_BARRERA_OTRA,     // Barrera de savestate/DoState/Reinitialize/split
	R_ESPERA_IDLE,      // EsperarIdle suelto (DeviceLost/DeviceRestore)
	R_CAND_ENDHOST,     // g_mu en PrepareCopyDisplayToOutput/EndHostFrame
	// --- Cualquier hilo: la parada dura ---
	R_FLUSHSYNC,        // VulkanRenderManager::FlushSync: readback BLOQUEANTE
	R_POSTSUBMIT,       // FrameTiming::PostSubmit (throttle deliberado)
	// --- STVGeWorker ---
	R_W_ORDEN,          // worker dormido esperando orden RUN (espera LEGITIMA)
	R_W_CANDADO,        // worker esperando g_mu que tiene el EmuThread
	R_W_PASADA,         // pasada entera con g_mu tomado (trabajo, no espera)
	// --- VulkanRenderMan ---
	R_RD_COLA,          // dormido esperando tarea (espera LEGITIMA)
	R_RD_ACQUIRE,       // vkAcquireNextImageKHR
	R_RD_PRESENT,       // vkQueuePresentKHR
	R_RD_READBACK,      // vkWaitForFences del readbackFence (la parada dura de verdad)
	// --- EmuThread: audio ---
	R_SAS_DRAIN,        // __SasDrain: el EmuThread esperando al hilo SAS
	// --- desglose de cand_otro: los sitios que se chocan el candado del worker.
	// Se etiquetan porque cand_otro es el UNICO campo que se mueve de verdad
	// entre un cuadro bueno (0,039 ms) y uno malo (0,588 ms), 15 veces por
	// cuadro. Sin este desglose no se sabe cual de los ~20 sitios es.
	R_CAND_INTERRUPT,   // InterruptStart/InterruptEnd/SyncEnd (interrupciones de la GE)
	R_CAND_INVALIDATE,  // InvalidateCache  <- sceKernelDcacheWritebackAll
	R_CAND_MEMOP,       // PerformMemoryCopy/Set/WriteFormatted/WriteStencil
	R_CAND_DISPLAY,     // SetDisplayFramebuffer/Prepare-/CopyDisplayToOutput
	R_CAND_LISTA,       // DequeueList/Continue/Break/BusyDrawing/PSPFrame
	// --- TRABAJO del EmuThread (no espera). Se instrumenta porque cpuEmu es el
	// UNICO campo que correlaciona con la caida de VPS (r=-0,89 sobre 222
	// ventanas): sube de 12,6 a 19,6 ms en los cuadros malos. Estas ranuras
	// dicen QUE son esos 7 ms extra.
	R_T_JIT,            // MIPSComp::IRJit::Compile  (compilar codigo nuevo)
	R_T_ATRAC,          // Atrac2::DecodeInternal    (decodificar audio)
	R_T_TEXTURA,        // TextureCacheCommon::SetTextureFramebuffer
	R_T_BLOCKXFER,      // GPUCommon::DoBlockTransfer (copias de VRAM)
	R_NUM
};

inline const char *const kNombre[] = {
	"fence_cv", "fence_vk", "cand_beginhost",
	"cand_encola", "cand_stall", "ge_sync", "cand_fbdirty", "cand_flip",
	"cand_otro", "drenaje",
	"BARRERA", "barrera_otra", "espera_idle", "cand_endhost",
	"flushsync", "postsubmit",
	"w_orden", "w_candado", "w_pasada",
	"rd_cola", "rd_acquire", "rd_present", "rd_readback",
	"sas_drain",
	"cand_interrupt", "cand_invalidate", "cand_memop", "cand_display", "cand_lista",
	"T_jit", "T_atrac", "T_textura", "T_blockxfer",
};
// Si alguien agrega una ranura y se olvida el nombre, que rompa la compilacion
// y no que el volcado imprima el nombre de la ranura de al lado.
static_assert(sizeof(kNombre) / sizeof(kNombre[0]) == (size_t)R_NUM,
	"STV medidor: kNombre y Ranura quedaron desalineados");

// --- Palanca ----------------------------------------------------------------
// Escrita solo por el EmuThread (en Cuadro), leida por los tres hilos.
inline std::atomic<int> g_activo{ 0 };
inline bool Activo() { return g_activo.load(std::memory_order_relaxed) != 0; }

// --- Relojes ----------------------------------------------------------------
// steady_clock en bionic = clock_gettime(CLOCK_MONOTONIC) por vDSO. Se usa el
// contador entero en ns a proposito: nada de doubles en el camino caliente.
inline uint64_t Ahora() {
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// CPU consumido POR ESTE HILO. Es el testigo cruzado del volcado.
inline uint64_t CpuDelHilo() {
#if defined(__linux__)
	struct timespec tp;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tp) != 0)
		return 0;
	return (uint64_t)tp.tv_sec * 1000000000ull + (uint64_t)tp.tv_nsec;
#else
	return 0;
#endif
}

// --- Cubos ------------------------------------------------------------------
// Un cubo por rol. Cada ranura la escribe UN solo hilo (el dueno del rol) y la
// lee el EmuThread al volcar: atomicos relaxed porque un uint64 cruzando hilos
// sin atomico es formalmente una carrera, y la regla del proyecto es que el
// testigo no puede tener agujeros formales. En aarch64 con LSE esto compila a
// un unico LDADD.
struct Cubo {
	std::atomic<uint64_t> ns[R_NUM];
	std::atomic<uint32_t> n[R_NUM];
	std::atomic<uint64_t> pico[R_NUM];
};
inline Cubo g_cubo[NUM_ROL];

inline thread_local int tl_rol = ROL_OTRO;
inline void MarcarRol(int r) { tl_rol = r; }

inline void Anotar(int ranura, uint64_t dt) {
	Cubo &c = g_cubo[tl_rol];
	c.ns[ranura].fetch_add(dt, std::memory_order_relaxed);
	c.n[ranura].fetch_add(1u, std::memory_order_relaxed);
	// El pico no necesita CAS: hay un solo escritor por (rol, ranura). El peor
	// caso teorico de una carrera imposible seria perder UN pico, nunca romper.
	if (dt > c.pico[ranura].load(std::memory_order_relaxed))
		c.pico[ranura].store(dt, std::memory_order_relaxed);
}

// El cronometro RAII. Apagado: constructor = un load relaxed + una rama;
// destructor = una rama sobre t0_. Cero relojes.
class Cronometro {
public:
	explicit Cronometro(Ranura r) : r_(r), t0_(Activo() ? Ahora() : 0) {}
	~Cronometro() {
		if (t0_)
			Anotar(r_, Ahora() - t0_);
	}
	Cronometro(const Cronometro &) = delete;
	Cronometro &operator=(const Cronometro &) = delete;
private:
	Ranura r_;
	uint64_t t0_;
};

// --- Ventana y volcado ------------------------------------------------------

inline uint64_t g_tCuadro = 0;      // instante del ultimo Cuadro()
inline uint64_t g_cpuCuadro = 0;    // CPU del EmuThread en el ultimo Cuadro()
inline uint64_t g_wallVentana = 0;  // suma de periodos de cuadro de la ventana
inline uint64_t g_cpuVentana = 0;   // suma de CPU del EmuThread de la ventana
inline uint64_t g_picoCuadro = 0;
inline uint32_t g_cuadros = 0;
inline uint64_t g_tPalanca = 0;     // ultima relectura de la palanca
inline uint64_t g_tMudo = 0;        // ultimo aviso de "apagado"
inline int g_ventana = 120;         // cuadros por volcado

inline void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

inline void Reiniciar() {
	for (int r = 0; r < NUM_ROL; r++) {
		for (int i = 0; i < R_NUM; i++) {
			g_cubo[r].ns[i].store(0, std::memory_order_relaxed);
			g_cubo[r].n[i].store(0, std::memory_order_relaxed);
			g_cubo[r].pico[i].store(0, std::memory_order_relaxed);
		}
	}
	g_wallVentana = 0;
	g_cpuVentana = 0;
	g_picoCuadro = 0;
	g_cuadros = 0;
}

// Lee la palanca. Devuelve la VENTANA en cuadros, o 0 si esta apagada.
// Contrato de lectura identico al de StvGeThread/StvEpilogo: el env manda
// sobre la prop, y un caracter no-digito vale 0 (apagado), no basura.
inline int LeerPalanca() {
	const char *t = nullptr;
	const char *e = getenv("STV_MED");
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
#endif
	if (e && *e) {
		t = e;
	} else {
#if defined(__ANDROID__)
		if (__system_property_get("debug.stv.med", prop) > 0 && prop[0])
			t = prop;
#endif
	}
	if (!t || *t < '0' || *t > '9')
		return 0;
	int v = *t - '0';
	if (v <= 0)
		return 0;
	if (v > 9)
		v = 9;
	return v * 60;
}

inline void Volcar() {
	if (!g_cuadros)
		return;
	const double invC = 1.0 / (double)g_cuadros;
	const double wall = (double)g_wallVentana * invC * 1e-6;   // ms por cuadro
	const double cpu = (double)g_cpuVentana * invC * 1e-6;
	(void)cpu;

	// Espera total del EmuThread = suma de sus ranuras. w_pasada y drenaje NO
	// son esperas (son trabajo) y no entran en la suma; se imprimen igual.
	double espera = 0.0;
	for (int i = 0; i < R_NUM; i++) {
		if (i == R_DRENAJE || i == R_W_PASADA)
			continue;
		espera += (double)g_cubo[ROL_EMU].ns[i].load(std::memory_order_relaxed) * invC * 1e-6;
	}

	// STV F10b (2026-08-29): EL NUMERO QUE FALTABA.
	// wall/fps de arriba miden el bucle NativeFrame, que corre a 60 Hz por el
	// VSYNC pase lo que pase: NO es la velocidad de emulacion. Se leia "59,4
	// fps" mientras el usuario veia caidas a 30, porque son cosas distintas.
	// vps = vblanks EMULADOS por segundo de pared (Core/HW/Display.cpp:81,
	// fps = frames/(now-lastFpsTime)) y ESA si es la velocidad real.
	float vps = 0.0f;
	__DisplayGetVPS(&vps);

	char b[512];
	if (g_cpuVentana) {
		snprintf(b, sizeof(b),
			"STV: esp INICIO %s cuadros=%u wall=%.3fms (%.1f present) VPS=%.1f pico=%.2fms cpuEmu=%.3fms esperaEmu=%.3fms resto=%.3fms",
			kMarca, g_cuadros, wall, wall > 0.0 ? 1000.0 / wall : 0.0, vps,
			(double)g_picoCuadro * 1e-6, cpu, espera, wall - cpu - espera);
	} else {
		// Sin CLOCK_THREAD_CPUTIME_ID no hay testigo cruzado: se dice, no se
		// inventa un resto= que seria wall-espera y parecería un dato.
		snprintf(b, sizeof(b),
			"STV: esp INICIO %s cuadros=%u wall=%.3fms (%.1f present) VPS=%.1f pico=%.2fms cpuEmu=n/a esperaEmu=%.3fms resto=n/a",
			kMarca, g_cuadros, wall, wall > 0.0 ? 1000.0 / wall : 0.0, vps,
			(double)g_picoCuadro * 1e-6, espera);
	}
	Emitir(b);

	// Se imprime TAMBIEN el rol "otro": si aparece, es que algun hilo del host
	// (loader, hilo grafico) esta entrando a estos candados y no lo sabiamos.
	for (int r = ROL_OTRO; r < NUM_ROL; r++) {
		int pos = snprintf(b, sizeof(b), "STV: esp %s", kNombreRol[r]);
		bool algo = false;
		for (int i = 0; i < R_NUM && pos < (int)sizeof(b) - 64; i++) {
			const uint64_t ns = g_cubo[r].ns[i].load(std::memory_order_relaxed);
			const uint32_t n = g_cubo[r].n[i].load(std::memory_order_relaxed);
			if (!ns && !n)
				continue;   // ranura muda = ranura que no aplica a este rol
			algo = true;
			pos += snprintf(b + pos, sizeof(b) - pos, " %s=%.3f/%.1f/%.2f",
				kNombre[i], (double)ns * invC * 1e-6, (double)n * invC,
				(double)g_cubo[r].pico[i].load(std::memory_order_relaxed) * 1e-6);
		}
		if (algo)
			Emitir(b);
	}
	Emitir("STV: esp FIN  (campo = msPorCuadro/vecesPorCuadro/picoMs)");
	Reiniciar();
}

// EL UNICO punto de llamada por cuadro. Va al principio de NativeFrame, o sea
// EN el EmuThread: aprovecha y marca el rol sin necesitar otra costura.
inline void Cuadro() {
	tl_rol = ROL_EMU;
	const uint64_t t = Ahora();

	// Relectura de la palanca 1 vez por segundo.
	if (t - g_tPalanca > 1000000000ull) {
		g_tPalanca = t;
		const int v = LeerPalanca();
		const bool antes = Activo();
		g_activo.store(v, std::memory_order_relaxed);
		if (v) {
			g_ventana = v;
			if (!antes) {   // 0 -> 1: la ventana arranca limpia
				Reiniciar();
				g_tCuadro = 0;
				char b[192];
				snprintf(b, sizeof(b), "STV: esp ENCENDIDO %s ventana=%d cuadros", kMarca, g_ventana);
				Emitir(b);
			}
		} else if (antes) {
			Emitir("STV: esp APAGADO por la palanca");
			g_tCuadro = 0;
		}
	}

	if (!Activo()) {
		// Regla del instrumento mudo: el apagado tambien habla.
		if (t - g_tMudo > 10000000000ull) {
			g_tMudo = t;
			char b[224];
			snprintf(b, sizeof(b),
				"STV: esp APAGADO (%s presente). Encender: adb shell setprop debug.stv.med 2", kMarca);
			Emitir(b);
		}
		g_tCuadro = 0;
		return;
	}

	const uint64_t cpu = CpuDelHilo();
	if (g_tCuadro) {
		const uint64_t dt = t - g_tCuadro;
		g_wallVentana += dt;
		g_cpuVentana += cpu - g_cpuCuadro;
		if (dt > g_picoCuadro)
			g_picoCuadro = dt;
		g_cuadros++;
	}
	g_tCuadro = t;
	g_cpuCuadro = cpu;

	if (g_cuadros >= (uint32_t)g_ventana)
		Volcar();
}

}  // namespace stvmed
