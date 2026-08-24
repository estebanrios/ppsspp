// =============================================================================
// STV — INSTRUMENTO: como son las transferencias de bloque del GE
//
// Parche del proyecto STV-TSPS-Android (tools/ppsspp/parches/diag-bloques.sh).
// NO es codigo de upstream. Todo lo que hace es CONTAR; no cambia ni una
// decision de copia.
//
// Los ids de camino son las ramas REALES de GPUCommon::DoBlockTransfer():
//   1 GESTOR           NotifyBlockTransferBefore devolvio true (GPUCommon.cpp:1746)
//   2 MEMCPY_CONTIGUO  un solo memcpy de w*h*bpp        (GPUCommon.cpp:1752)
//   3 SOLAPE_64B       memmove en trozos de 64 B/linea  (GPUCommon.cpp:1764)
//   4 POR_LINEA        un memcpy de w*bpp por linea     (GPUCommon.cpp:1867)
//   5 INVALIDO         rango invalido, no copia nada    (GPUCommon.cpp:1894)
//   0 DESCONOCIDO      NADIE marco: upstream cambio y esto miente. Denunciar.
//
// Encendido en caliente (se re-evalua 1 vez por segundo):
//   1. entorno   STV_DIAG_BLOQUES=1
//   2. propiedad debug.stv.diag=1        (la MISMA que diag-vaciados, a proposito)
//   3. archivo   <memstick>/PSP/SYSTEM/stv-diag-bloques
//
// Con el diagnostico APAGADO no se lee el reloj ni una vez, y todos los
// contadores quedan detras de una unica copia de g_activo tomada al entrar.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "Common/TimeUtil.h"

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace stvblq {

// Cadena que sirve para probar que el parche entro en el .so:
//   strings libppsspp_jni.so | grep STV_DIAG_BLOQUES_v1
inline constexpr const char *kMarca = "STV_DIAG_BLOQUES_v1";

enum : uint8_t {
	CAM_DESCONOCIDO = 0,
	CAM_GESTOR = 1,
	CAM_MEMCPY_CONTIGUO = 2,
	CAM_SOLAPE64 = 3,
	CAM_POR_LINEA = 4,
	CAM_INVALIDO = 5,
	NUM_CAMINOS = 6
};

inline const char *NombreCamino(unsigned c) {
	static const char *const n[NUM_CAMINOS] = {
		"DESCONOCIDO", "GESTOR", "MEMCPY_CONTIGUO", "SOLAPE_64B", "POR_LINEA", "INVALIDO"
	};
	return c < NUM_CAMINOS ? n[c] : "FUERA_DE_RANGO";
}

// clase = (src en VRAM ? 0 : 2) + (dst en VRAM ? 0 : 1)
inline const char *NombreClase(unsigned c) {
	static const char *const n[4] = { "VV", "VR", "RV", "RR" };
	return c < 4 ? n[c] : "FUERA_DE_RANGO";
}

// --- reloj ------------------------------------------------------------------
// CNTVCT_EL0 directo. Ver la cabecera del parche para los limites (sin ISB,
// resolucion de ~41,7 ns con cntfrq de 24 MHz, y el auto-medido del costo de
// lectura que se publica en la linea `blq RELOJ`).
inline uint64_t g_frq         = 0;     // cntfrq_el0, leido UNA vez al encender
inline double   g_lecturaNs   = 0.0;   // costo medido de una lectura
inline uint64_t g_calSumidero = 0;     // impide que el bucle de calibrado se optimice

inline uint64_t Ticks() {
#if defined(__aarch64__)
	uint64_t t;
	asm volatile("mrs %0, cntvct_el0" : "=r"(t));
	return t;
#else
	return 0;
#endif
}

inline uint64_t Frecuencia() {
#if defined(__aarch64__)
	uint64_t f;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
#else
	return 0;
#endif
}

inline uint64_t ANanos(uint64_t ticks) {
	return g_frq ? (uint64_t)((ticks * 1000000000ull) / g_frq) : 0ull;
}

// Mide cuanto cuesta UNA lectura del reloj, en el aparato de verdad, con el
// mismo codigo del camino caliente. Se corre una sola vez, al encender.
inline void Calibrar() {
	g_frq = Frecuencia();
	g_lecturaNs = 0.0;
#if defined(__aarch64__)
	if (!g_frq) return;
	const int kN = 2000;
	uint64_t acc = 0;
	const uint64_t a = Ticks();
	for (int i = 0; i < kN; i++) acc += Ticks();
	const uint64_t b = Ticks();
	g_calSumidero = acc;
	g_lecturaNs = (double)(b - a) * 1e9 / (double)g_frq / (double)kN;
#endif
}

// --- acumuladores -----------------------------------------------------------
// NO son atomicos: todo esto lo toca el EmuThread (DoBlockTransfer, y
// BeginHostFrame->BeginFrame que vuelca). Una carrera solo desviaria un conteo,
// nunca puede romper nada: son sumas enteras planas.
struct Cubo {
	uint32_t n;
	uint64_t bytes;
	uint64_t tk;
};

enum : uint32_t { kMaxFormas = 16 };

struct Forma {
	uint64_t clave;
	uint32_t w, h, bpp, ss, ds;
	uint8_t  clase;
	uint8_t  caminos;       // MASCARA de bits: 1u<<camino, por si una forma toma dos ramas
	uint32_t n;
	uint64_t bytes;
	uint64_t tk;
	uint32_t srcMin, srcMax;
	uint32_t dstMin, dstMax;
};

inline uint64_t g_llamadas  = 0;
inline uint64_t g_bytes     = 0;
inline uint64_t g_tkNotify  = 0;
inline uint64_t g_tkCopia   = 0;
inline uint64_t g_tkTotal   = 0;
inline uint32_t g_detallado = 0;   // MemBlockInfoDetailed(...) dio true
inline uint32_t g_sinGestor = 0;   // framebufferManager_ == nullptr
inline uint32_t g_fueraDeRango = 0;
inline uint32_t g_memcpyGE  = 0;   // llamadas a GPUCommon::PerformMemoryCopy
inline uint32_t g_memsetGE  = 0;   // llamadas a GPUCommon::PerformMemorySet

inline Cubo     g_cam[NUM_CAMINOS];
inline Cubo     g_cls[4];
inline uint32_t g_hist[32];
inline uint64_t g_histBytes[32];
inline Forma    g_formas[kMaxFormas];
inline uint32_t g_nFormas = 0;
inline uint32_t g_formasDesborde = 0;

inline int      g_activo    = 0;   // 0 apagado, 1 encendido
inline uint32_t g_cuadros   = 0;
inline int      g_flipsPrev = 0;
inline double   g_t0        = 0.0;
inline double   g_tQueja    = 0.0;

inline void Reiniciar() {
	g_llamadas = g_bytes = 0;
	g_tkNotify = g_tkCopia = g_tkTotal = 0;
	g_detallado = g_sinGestor = g_fueraDeRango = 0;
	g_memcpyGE = g_memsetGE = 0;
	memset(g_cam, 0, sizeof(g_cam));
	memset(g_cls, 0, sizeof(g_cls));
	memset(g_hist, 0, sizeof(g_hist));
	memset(g_histBytes, 0, sizeof(g_histBytes));
	memset(g_formas, 0, sizeof(g_formas));
	g_nFormas = 0;
	g_formasDesborde = 0;
}

// --- camino caliente --------------------------------------------------------
// El evento vive en la PILA de DoBlockTransfer. Todo el instrumento es inline
// en este header, asi que con -O2 el compilador lo promociona a registros: no
// hay estructura de verdad en memoria.
//
// `on` es una copia de g_activo tomada UNA sola vez al entrar. El resto de la
// funcion NO vuelve a leer el global: mira esa copia. Eso es lo que hace que el
// costo apagado sea un punado de ramas predecibles, y ademas garantiza que un
// encendido a mitad de camino no deje un evento a medias.
struct Evento {
	uint64_t t0;        // ticks al entrar
	uint64_t tNotify;   // ticks DENTRO de NotifyBlockTransferBefore
	uint64_t tCopia;    // al abrir el bloque de copia: la marca. al cerrar: la duracion
	uint32_t w, h, bpp, ss, ds;
	uint32_t src, dst;  // direcciones EFECTIVAS de la primera linea
	uint8_t  camino;
	uint8_t  clase;
	uint8_t  detallado;
	bool     on;
};

inline void Inicio(Evento &e) {
	e.on = g_activo != 0;
	e.camino = CAM_DESCONOCIDO;
	e.clase = 0;
	e.detallado = 0;
	e.tNotify = 0;
	e.tCopia = 0;
	e.w = e.h = e.bpp = e.ss = e.ds = 0;
	e.src = e.dst = 0;
	e.t0 = e.on ? Ticks() : 0;
}

// Lectura de reloj que respeta el interruptor. Apagado: una rama y un cero.
inline uint64_t Marca(const Evento &e) {
	return e.on ? Ticks() : 0;
}

inline void AnotarForma(Evento &e, int w, int h, int bpp, int ss, int ds,
                        uint32_t src, uint32_t dst, bool srcVram, bool dstVram) {
	if (!e.on) return;
	e.w   = (uint32_t)w;
	e.h   = (uint32_t)h;
	e.bpp = (uint32_t)bpp;
	e.ss  = (uint32_t)ss;
	e.ds  = (uint32_t)ds;
	e.src = src;
	e.dst = dst;
	e.clase = (uint8_t)((srcVram ? 0 : 2) + (dstVram ? 0 : 1));
}

inline void Detallado(Evento &e, bool v) {
	e.detallado = (uint8_t)(e.detallado | (v ? 1u : 0u));
}

inline void Registrar(Evento &e) {
	if (!e.on) return;
	const uint64_t tt = Ticks() - e.t0;
	const uint64_t bytes = (uint64_t)e.w * (uint64_t)e.h * (uint64_t)e.bpp;

	g_llamadas++;
	g_bytes    += bytes;
	g_tkNotify += e.tNotify;
	g_tkCopia  += e.tCopia;
	g_tkTotal  += tt;
	if (e.detallado) g_detallado++;

	const unsigned cam = e.camino < NUM_CAMINOS ? e.camino : (unsigned)CAM_DESCONOCIDO;
	g_cam[cam].n++; g_cam[cam].bytes += bytes; g_cam[cam].tk += tt;

	const unsigned cls = e.clase & 3u;
	g_cls[cls].n++; g_cls[cls].bytes += bytes; g_cls[cls].tk += tt;

	// cubeta log2 de bytes: e=k son las llamadas con 2^k <= bytes < 2^(k+1)
	unsigned idx = bytes ? (unsigned)(63 - __builtin_clzll(bytes)) : 0u;
	if (idx > 31) idx = 31;
	g_hist[idx]++;
	g_histBytes[idx] += bytes;

	// --- tabla de formas ---
	// Clave EXACTA, no hash: w,h,ss,ds en 11 bits cada uno, bpp en 3, clase en
	// 2 -> 49 bits. Los getters del GE dan w,h en 1..1024 y los strides en
	// 0..1024 (GPUState.h:411-420), asi que entran de sobra. Si alguno no
	// entrara, la llamada se cuenta en g_fueraDeRango y NO ensucia la tabla.
	if (e.w < 2048 && e.h < 2048 && e.ss < 2048 && e.ds < 2048 && e.bpp < 8) {
		const uint64_t clave = ((uint64_t)e.w  << 38) | ((uint64_t)e.h  << 27) |
		                       ((uint64_t)e.ss << 16) | ((uint64_t)e.ds <<  5) |
		                       ((uint64_t)e.bpp <<  2) | (uint64_t)cls;
		uint32_t i = 0;
		while (i < g_nFormas && g_formas[i].clave != clave) i++;
		if (i == g_nFormas) {
			if (g_nFormas >= kMaxFormas) { g_formasDesborde++; return; }
			Forma &nf = g_formas[i];
			nf.clave = clave;
			nf.w = e.w; nf.h = e.h; nf.bpp = e.bpp; nf.ss = e.ss; nf.ds = e.ds;
			nf.clase = (uint8_t)cls;
			nf.caminos = 0;
			nf.n = 0; nf.bytes = 0; nf.tk = 0;
			nf.srcMin = nf.srcMax = e.src;
			nf.dstMin = nf.dstMax = e.dst;
			g_nFormas++;
		}
		Forma &f = g_formas[i];
		f.n++;
		f.bytes += bytes;
		f.tk    += tt;
		f.caminos = (uint8_t)(f.caminos | (uint8_t)(1u << cam));
		if (e.src < f.srcMin) f.srcMin = e.src;
		if (e.src > f.srcMax) f.srcMax = e.src;
		if (e.dst < f.dstMin) f.dstMin = e.dst;
		if (e.dst > f.dstMax) f.dstMax = e.dst;
	} else {
		g_fueraDeRango++;
	}
}

// --- salida -----------------------------------------------------------------
inline void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

// --- encendido / apagado ----------------------------------------------------
inline void RutaArchivo(const char *memstick, char *out, size_t n) {
	snprintf(out, n, "%s/PSP/SYSTEM/stv-diag-bloques", memstick ? memstick : "(sin memstick)");
}

inline bool HayArchivo(const char *memstick) {
	if (!memstick || !*memstick) return false;
	char ruta[1024];
	RutaArchivo(memstick, ruta, sizeof(ruta));
	FILE *f = fopen(ruta, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

inline int Resolver(const char *memstick, const char **via) {
	const char *e = getenv("STV_DIAG_BLOQUES");
	if (e && *e && *e != '0') { *via = "entorno"; return 1; }
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.stv.diag", prop) > 0 && prop[0] && prop[0] != '0') {
		*via = "propiedad"; return 1;
	}
#endif
	if (HayArchivo(memstick)) { *via = "archivo"; return 1; }
	*via = "nada";
	return 0;
}

inline void EmitirReloj() {
	char b[512];
	if (!g_frq) {
		snprintf(b, sizeof(b),
			"STV: blq RELOJ AUSENTE frq=0 — sin cntvct_el0: TODOS los ns de abajo son 0 y NO son una medicion");
	} else {
		snprintf(b, sizeof(b), "STV: blq RELOJ frq=%llu tick_ns=%.2f lectura_ns=%.1f",
			(unsigned long long)g_frq, 1e9 / (double)g_frq, g_lecturaNs);
	}
	Emitir(b);
}

// --- volcado ----------------------------------------------------------------
inline void Volcar(double ventana, int flips) {
	char b[640];

	// INICIO sale SIEMPRE, aunque no haya habido ni una transferencia. Cero
	// transferencias se informa como n=0; nunca como silencio.
	snprintf(b, sizeof(b),
		"STV: blq INICIO ventana=%.3f cuadros=%u flips=%d n=%llu bytes=%llu det=%u singestor=%u fuera=%u memcpyGE=%u memsetGE=%u tnotify_ns=%llu tcopia_ns=%llu ttotal_ns=%llu frq=%llu lectura_ns=%.1f marca=%s",
		ventana, g_cuadros, flips,
		(unsigned long long)g_llamadas, (unsigned long long)g_bytes,
		g_detallado, g_sinGestor, g_fueraDeRango, g_memcpyGE, g_memsetGE,
		(unsigned long long)ANanos(g_tkNotify),
		(unsigned long long)ANanos(g_tkCopia),
		(unsigned long long)ANanos(g_tkTotal),
		(unsigned long long)g_frq, g_lecturaNs, kMarca);
	Emitir(b);

	for (unsigned i = 0; i < NUM_CAMINOS; i++) {
		if (!g_cam[i].n) continue;
		snprintf(b, sizeof(b), "STV: blq cam id=%u nombre=%s n=%u bytes=%llu ns=%llu",
			i, NombreCamino(i), g_cam[i].n,
			(unsigned long long)g_cam[i].bytes, (unsigned long long)ANanos(g_cam[i].tk));
		Emitir(b);
	}
	for (unsigned i = 0; i < 4; i++) {
		if (!g_cls[i].n) continue;
		snprintf(b, sizeof(b), "STV: blq cls id=%u nombre=%s n=%u bytes=%llu ns=%llu",
			i, NombreClase(i), g_cls[i].n,
			(unsigned long long)g_cls[i].bytes, (unsigned long long)ANanos(g_cls[i].tk));
		Emitir(b);
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!g_hist[i]) continue;
		snprintf(b, sizeof(b), "STV: blq hist e=%u n=%u bytes=%llu",
			i, g_hist[i], (unsigned long long)g_histBytes[i]);
		Emitir(b);
	}
	for (unsigned i = 0; i < g_nFormas; i++) {
		const Forma &f = g_formas[i];
		snprintf(b, sizeof(b),
			"STV: blq forma i=%u w=%u h=%u bpp=%u ss=%u ds=%u cls=%s cam=0x%02x n=%u bytes=%llu ns=%llu smin=0x%08x smax=0x%08x dmin=0x%08x dmax=0x%08x",
			i, f.w, f.h, f.bpp, f.ss, f.ds, NombreClase(f.clase), (unsigned)f.caminos,
			f.n, (unsigned long long)f.bytes, (unsigned long long)ANanos(f.tk),
			f.srcMin, f.srcMax, f.dstMin, f.dstMax);
		Emitir(b);
	}
	snprintf(b, sizeof(b), "STV: blq formas n=%u desborde=%u fuera=%u",
		g_nFormas, g_formasDesborde, g_fueraDeRango);
	Emitir(b);
	Emitir("STV: blq FIN");
}

// Se llama UNA VEZ POR CUADRO, desde DrawEngineVulkan::BeginFrame(), o sea
// fuera de todo bucle de dibujo — y CORRE HAYA O NO HAYA TRANSFERENCIAS.
inline void PorCuadro(const char *memstick, int numFlips) {
	g_cuadros++;
	const double ahora = time_now_d();
	if (g_t0 == 0.0) { g_t0 = ahora; g_tQueja = ahora; g_flipsPrev = numFlips; }
	const double ventana = ahora - g_t0;
	if (ventana < 1.0) return;

	const char *via = "nada";
	const int antes = g_activo;
	g_activo = Resolver(memstick, &via);

	if (g_activo) {
		if (!antes) {
			char b[512];
			Calibrar();
			snprintf(b, sizeof(b),
				"STV: blq ENCENDIDO via=%s (%s) — esta primera ventana esta cortada, no la uses",
				via, kMarca);
			Emitir(b);
			EmitirReloj();
		} else {
			Volcar(ventana, numFlips - g_flipsPrev);
		}
	} else if (ahora - g_tQueja >= 10.0) {
		// Regla del proyecto: un silencio no es un dato. Apagado tambien habla,
		// y dice la ruta exacta del archivo que lo enciende.
		char ruta[1024], b[1400];
		RutaArchivo(memstick, ruta, sizeof(ruta));
		snprintf(b, sizeof(b),
			"STV: blq APAGADO (%s) ruta=%s — encender con: setprop debug.stv.diag 1  |  touch %s",
			kMarca, ruta, ruta);
		Emitir(b);
		g_tQueja = ahora;
	}

	Reiniciar();
	g_cuadros = 0;
	g_flipsPrev = numFlips;
	g_t0 = ahora;
}

}  // namespace stvblq
