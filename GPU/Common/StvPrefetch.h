// =============================================================================
// STV — INTERRUPTOR Y TESTIGOS DE LOS PRFM QUIRURGICOS (frente F2).
//
// Parche del proyecto STV-TSPS-Android (tools/ppsspp/parches/prefetch-a55.sh).
// NO es codigo de upstream.
//
// Aca vive SOLO la politica: que nivel esta puesto, a que distancia hay que
// prefetchear para un stride dado, y la contabilidad. Las instrucciones de
// prefetch viven en los tres sitios calientes (VertexDecoderArm64.cpp,
// VertexDecoderHandwritten.cpp, GPUCommonHW.cpp).
//
// Los contadores NO son atomicos: los toca solo el EmuThread. Son sumas
// enteras planas; una carrera podria desviar un conteo, nunca romper nada.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace stvprf {

// Cadena que prueba que el parche entro en el .so:
//   strings libppsspp_jni.so | grep STV_PREFETCH_v1
inline constexpr const char *kMarca = "STV_PREFETCH_v1";

// --- CONSTANTES DEL SILICIO Y DEL DISEÑO ------------------------------------
// Linea de cache del Cortex-A55: 64 B (fija en la arquitectura del nucleo,
// no configurable por el integrador).
inline constexpr int kLinea = 64;

// Cuantos VERTICES por delante pide el decodificador. Lo que hay que esconder
// es TIEMPO, y el tiempo por vertice es ~constante -> la unidad correcta es el
// vertice, no el byte. Ver el argumento completo en el parche, §2.a.
inline constexpr int kVertsAdelante = 3;

// Cuantos BYTES por delante pide el lazo de la display list. 128 B = 2 lineas
// = 32 comandos. La lista se recorre de a 4 B hacia adelante.
inline constexpr int kBytesLista = 128;

// Tope del campo imm12 escalado por 8 de `PRFM <prfop>, [Xn, #pimm]`.
inline constexpr int kMaxImm = 32760;

// --- NIVELES ----------------------------------------------------------------
//   0 = TODO apagado (upstream exacto)
//   1 = TODO encendido                                    <- DEFAULT
//   2 = solo decodificadores de vertices (JIT + el de a mano)
//   3 = solo la display list (FastRunLoop)
// Un caracter que NO sea digito, o un digito fuera de 0..3, se toma como el
// DEFAULT y no como cero: un typo en el setprop no puede apagar la palanca en
// silencio. (Misma regla que blq-after.)
inline constexpr int kNivelMax = 3;
inline constexpr int kNivelDefecto = 1;

inline int NivelDeTexto(const char *s) {
	if (!s || !*s) return kNivelDefecto;
	if (*s < '0' || *s > '9') return kNivelDefecto;
	const int v = *s - '0';
	return v > kNivelMax ? kNivelDefecto : v;
}

inline int  g_nivel   = kNivelDefecto;
inline bool g_dec     = true;    // prefetch en los decodificadores de vertices
inline bool g_lista   = true;    // prefetch en FastRunLoop
inline bool g_resuelto = false;
inline const char *g_via = "defecto";

// --- CONTADORES TESTIGO -----------------------------------------------------
inline uint32_t g_decCon    = 0;   // decodificadores JIT compilados CON PRFM
inline uint32_t g_decSin    = 0;   // ...SIN
inline uint32_t g_ultStride = 0;   // stride del ultimo decodificador JIT compilado
inline uint32_t g_ultDist   = 0;   // distancia emitida para ese
inline uint32_t g_hwCon     = 0;   // llamadas al decodificador a mano de GoW, con prefetch
inline uint32_t g_hwSin     = 0;   // ...sin
inline uint32_t g_lstCon    = 0;   // llamadas a FastRunLoop con prefetch
inline uint32_t g_lstSin    = 0;   // ...sin

// --- DISTANCIA DEL PREFETCH DE VERTICES -------------------------------------
// K vertices por delante, pero NUNCA menos de una linea de cache entera: con
// stride chico, K*stride cae dentro de la MISMA linea que se esta leyendo y el
// prefetch no pediria nada nuevo. Despues se redondea a multiplo de 8 (lo
// exige la forma inmediata de PRFM) y se acota al tope del campo.
//
// constexpr a proposito: en el decodificador escrito a mano el stride es
// `sizeof(struct)`, asi que la distancia se resuelve en compilacion y el
// prefetch sale con offset inmediato, sin un ADD.
inline constexpr int DistanciaVertices(int stride) {
	if (stride <= 0) return 0;
	int d = stride * kVertsAdelante;
	while (d < kLinea) d += stride;   // al menos una linea por delante
	d &= ~7;                          // PRFM immediate: multiplo de 8
	if (d < 8) d = 8;
	if (d > kMaxImm) d = kMaxImm;
	return d;
}

// --- POR QUE TODO LO DE ABAJO ES `noinline` ---------------------------------
// MEDIDO, no supuesto. En la primera version Dec() hacia una resolucion
// perezosa (`if (!g_resuelto) Resolver();`) y clang INTEGRO `getenv` y
// `__system_property_get` DENTRO de VtxDec_Tu16_C8888_Pfloat: la funcion paso
// de 28 a 127 instrucciones y, por el `char prop[PROP_VALUE_MAX]`, se gano un
// canario de pila (-fstack-protector-strong) que antes no tenia. Cien
// instrucciones de codigo frio metidas en la cache de instrucciones de un lazo
// caliente es exactamente lo contrario de lo que busca este parche.
//
// La cura tiene dos partes:
//   1. Dec() y Lista() son LECTURAS PURAS de un bool ya resuelto: cero ramas,
//      cero llamadas, cero marco de pila.
//   2. Todo lo que huele a syscall o a snprintf lleva `noinline`, asi no puede
//      volver a colarse en el llamador aunque el inliner cambie de opinion.
// La resolucion vive donde corresponde: una vez por cuadro, en PorCuadro().
#define STV_PRF_FRIO __attribute__((noinline))

STV_PRF_FRIO inline void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

STV_PRF_FRIO inline int ResolverNivel(const char **via) {
	const char *e = getenv("STV_PRF");
	if (e && *e) { *via = "env"; return NivelDeTexto(e); }
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.prf", prop) > 0 && prop[0]) {
		*via = "prop";
		return NivelDeTexto(prop);
	}
#endif
	*via = "defecto";
	return kNivelDefecto;
}

STV_PRF_FRIO inline void Resolver() {
	g_nivel = ResolverNivel(&g_via);
	g_dec   = (g_nivel == 1 || g_nivel == 2);
	g_lista = (g_nivel == 1 || g_nivel == 3);
	g_resuelto = true;
}

// LECTURAS PURAS. Ni una rama, ni una llamada: los dos bool ya estan resueltos
// (arrancan en el default y PorCuadro() los pone al dia en el PRIMER cuadro).
//
// QUE PASA SI UN DECODIFICADOR SE COMPILA ANTES DEL PRIMER CUADRO: se compila
// con el DEFAULT, y la primera PorCuadro() lo detecta —devuelve "cambio" si el
// nivel real no es el default— y tira el cache de decodificadores. O sea que la
// ventana se cierra sola, sin pagar una rama por cada uso.
inline bool Dec()   { return g_dec; }
inline bool Lista() { return g_lista; }

// --- POR CUADRO -------------------------------------------------------------
// Se llama UNA VEZ POR CUADRO desde GPUCommonHW::CheckConfigChanged(), que
// upstream documenta como "Called once per frame".
//
// DEVUELVE true si cambio la parte del nivel que afecta a los DECODIFICADORES.
// El que llama traduce ese true en `configChanged_ = true`, y con eso upstream
// tira el cache de decodificadores por SU PROPIO camino
// (drawEngineCommon_->NotifyConfigChanged()). No inventamos un vaciado nuevo.
//
// EL CONTADOR ARRANCA EN kFramesRevision-1 A PROPOSITO: asi la PRIMERA llamada
// —el primer cuadro— ya resuelve la propiedad. Si arrancara en 0, los primeros
// 32 cuadros de cada corrida usarian el default aunque el coordinador hubiera
// puesto debug.stv.prf antes de lanzar: medio segundo del brazo B contaminado
// con el brazo A, y un testigo que miente.
inline constexpr uint32_t kFramesRevision = 32;    // ~0,5 s
inline constexpr uint32_t kFramesCenso    = 256;   // ~4-5 s
inline uint32_t g_cuentaRev   = kFramesRevision - 1;
inline uint32_t g_cuentaCenso = kFramesCenso - 1;
inline bool g_hola = false;

STV_PRF_FRIO inline void Censo() {
	char b[512];
	snprintf(b, sizeof(b),
		"STV: prf CENSO nivel=%d dec_con=%u dec_sin=%u stride=%u dist=%u"
		" hw_con=%u hw_sin=%u lst_con=%u lst_sin=%u",
		g_nivel, g_decCon, g_decSin, g_ultStride, g_ultDist,
		g_hwCon, g_hwSin, g_lstCon, g_lstSin);
	Emitir(b);
}

STV_PRF_FRIO inline bool PorCuadro() {
	bool cambioDec = false;

	if (++g_cuentaRev >= kFramesRevision) {
		g_cuentaRev = 0;
		const bool antesDec = g_dec, antesLista = g_lista;
		const int antesNivel = g_nivel;
		Resolver();
		cambioDec = (g_dec != antesDec);
		if (g_nivel != antesNivel || g_dec != antesDec || g_lista != antesLista) {
			char b[256];
			snprintf(b, sizeof(b),
				"STV: prf NIVEL %d -> dec=%d lista=%d (%s) — debug.stv.prf",
				g_nivel, (int)g_dec, (int)g_lista, kMarca);
			Emitir(b);
		}
	}

	if (!g_hola) {
		g_hola = true;
		if (!g_resuelto) Resolver();
		char b[256];
		snprintf(b, sizeof(b),
			"STV: prf ACTIVO (%s) nivel=%d via=%s dec=%d lista=%d"
			" — K=%d verts, %d B en la display list",
			kMarca, g_nivel, g_via, (int)g_dec, (int)g_lista,
			kVertsAdelante, kBytesLista);
		Emitir(b);
	}

	if (++g_cuentaCenso >= kFramesCenso) {
		g_cuentaCenso = 0;
		Censo();
	}

	return cambioDec;
}

}  // namespace stvprf

// El macro solo sirve adentro de este header; no se lo deja suelto en el resto
// del arbol.
#undef STV_PRF_FRIO
