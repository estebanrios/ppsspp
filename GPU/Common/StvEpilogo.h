// =============================================================================
// STV — contadores testigo del EPILOGO DE BLOQUE DIFERIDO.
//
// Parche del proyecto STV-TSPS-Android (tools/ppsspp/parches/blq-epilogo.sh).
// NO es codigo de upstream.
//
// Aca vive SOLO la contabilidad y la marca. El mecanismo (el rango pendiente y
// su vaciado) vive en TextureCacheCommon, que es la clase duena de `cache_`.
//
// Los contadores NO son atomicos: los toca solo el EmuThread, por el camino de
// DoBlockTransfer y por SetTexture/StartFrame, que son el mismo hilo. Son
// sumas enteras planas: una carrera solo podria desviar un conteo, nunca
// romper nada.
//
// COSTO: cuatro incrementos de un uint32 global por transferencia. Se dejan
// encendidos TAMBIEN en la variante de produccion a proposito: sin ellos, la
// unica forma de saber si la fusion esta pasando seria creerlo. Regla del
// proyecto: un silencio no es un dato.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace stvepi {

// Cadena que prueba que el parche entro en el .so:
//   strings libppsspp_jni.so | grep STV_BLQ_EPILOGO_v1
inline constexpr const char *kMarca = "STV_BLQ_EPILOGO_v1";

// Tope del rango fusionado: 2 MiB = la VRAM entera del PSP, y ademas el doble
// de la ventana de +-1 MiB (LARGEST_TEXTURE_SIZE) que ya usa el recorrido de
// upstream. Con este tope, el recorrido fusionado nunca puede mirar mas del
// doble de entradas que el peor recorrido de una sola llamada.
inline constexpr uint32_t kTopeRango = 2u * 1024u * 1024u;

enum : unsigned {
	MOT_USO = 0,      // llego algo que PUEDE leer la cache de texturas
	MOT_FIN = 1,      // fin de cuadro, o la cache se vacia entera
	MOT_UMBRAL = 2,   // el rango fusionado se pasaba de kTopeRango
	NUM_MOT = 3
};

inline uint32_t g_diferidas = 0;          // llamadas que entraron al camino diferido
inline uint32_t g_fusionadas = 0;         // ...y que se fusionaron con una pendiente
inline uint32_t g_pasadas = 0;            // llamadas que NO se pudieron diferir (freno)
inline uint32_t g_vaciados[NUM_MOT] = { 0, 0, 0 };

inline bool g_hola = false;

inline void Reiniciar() {
	g_diferidas = 0;
	g_fusionadas = 0;
	g_pasadas = 0;
	g_vaciados[MOT_USO] = 0;
	g_vaciados[MOT_FIN] = 0;
	g_vaciados[MOT_UMBRAL] = 0;
}

// --- INTERRUPTOR EN CALIENTE ------------------------------------------------
//
// POR QUE EXISTE, y son dos razones distintas:
//
//   1. MEDICION. Con el interruptor, el A/B de la fusion se corre con UN SOLO
//      APK, en UNA sola instalacion, sin reinstalar entre brazos. Es la unica
//      forma de que el delta sea atribuible a la fusion y no a la diferencia
//      entre dos compilaciones. Es el mismo patron que ya usa el instrumento
//      diag-bloques (debug.stv.diag), a proposito.
//   2. SEGURIDAD. Este parche toca la invalidacion de la cache de texturas. Si
//      alguna vez aparece un artefacto de textura, el interruptor lo descarta o
//      lo confirma EN EL ACTO, sin reflashear y sin recompilar:
//          adb shell setprop debug.stv.epi 0     (fusion apagada = upstream)
//          adb shell setprop debug.stv.epi 1     (fusion encendida, el default)
//      Si el artefacto sigue con la fusion apagada, no era este parche.
//
// APAGADO significa EXACTAMENTE upstream: InvalidateDiferido() vacia lo que
// hubiera pendiente y llama a Invalidate(addr, size, GPU_INVALIDATE_HINT).
//
// El default es ENCENDIDO: la propiedad ausente no apaga nada. Se re-evalua una
// vez cada kFramesRevision cuadros, asi que prender/apagar tarda <1 s en tener
// efecto. Entre revisiones el camino caliente lee una copia en memoria: cero
// syscalls por transferencia.
//
// EL CONTADOR ARRANCA EN kFramesRevision-1 A PROPOSITO: asi la PRIMERA llamada
// a PorCuadro() —o sea el primer cuadro, antes de la primera transferencia de
// bloque— ya resuelve la propiedad. Si arrancara en 0, los primeros 32 cuadros
// de CADA corrida usarian el default (fusion encendida) aunque el coordinador
// hubiera puesto debug.stv.epi=0 antes de lanzar: medio segundo del brazo B
// contaminado con el brazo A, y un testigo que dice "fusion=1" cuando el
// aparato pidió 0. Un testigo que puede mentir no sirve.
inline int g_fusion = 1;
inline bool g_resuelto = false;
inline constexpr uint32_t kFramesRevision = 32;
inline uint32_t g_cuentaRevision = kFramesRevision - 1;

inline int ResolverFusion() {
	const char *e = getenv("STV_EPI_FUSION");
	if (e && *e) return (*e != '0') ? 1 : 0;
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.epi", prop) > 0 && prop[0]) {
		return prop[0] != '0' ? 1 : 0;
	}
#endif
	return 1;   // ausente = ENCENDIDO
}

inline void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

inline void Resolver() {
	g_fusion = ResolverFusion();
	g_resuelto = true;
}

// Se llama UNA VEZ POR CUADRO desde TextureCacheCommon::StartFrame(). Solo
// pregunta por la propiedad cada kFramesRevision cuadros — y la primera vez es
// en el PRIMER cuadro, no en el 32 (ver el comentario de g_cuentaRevision).
inline void PorCuadro() {
	if (++g_cuentaRevision < kFramesRevision) return;
	g_cuentaRevision = 0;
	const int antes = g_fusion;
	Resolver();
	if (g_fusion != antes) {
		char b[256];
		snprintf(b, sizeof(b), "STV: epi FUSION %s (%s) — debug.stv.epi",
			g_fusion ? "ENCENDIDA" : "APAGADA (camino de upstream)", kMarca);
		Emitir(b);
	}
}

// Una linea por sesion, la primera vez que se difiere algo. Dice la marca y
// —lo que de verdad importa— si la puerta de upstream (bTextureBackoffCache)
// esta abierta: con la puerta cerrada este parche no puede comprar nada,
// porque upstream tampoco recorreria el mapa.
inline void Hola(int backoff) {
	if (g_hola) return;
	g_hola = true;
	// Red por si alguna transferencia de bloque llegara ANTES del primer
	// StartFrame(): el testigo tiene que decir el estado de verdad, no el
	// default. Cuesta una lectura de propiedad, una sola vez por sesion.
	if (!g_resuelto) Resolver();
	char b[256];
	snprintf(b, sizeof(b),
		"STV: epi ACTIVO (%s) backoff=%d fusion=%d — epilogo de DoBlockTransfer diferido",
		kMarca, backoff, g_fusion);
	Emitir(b);
}

}  // namespace stvepi
