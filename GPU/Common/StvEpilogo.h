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

// --- STV(aft): desglose de NotifyBlockTransferAfter (parches/blq-after.sh) ---
// Contadores planos, siempre encendidos (un incremento entero cada uno).
// La CRONOMETRIA en cambio va detras de g_crono, que solo enciende el
// instrumento diag-bloques: en el binario de produccion es una rama
// perfectamente predecible y CERO lecturas de reloj.
inline uint32_t g_aftLlamadas = 0;
inline uint32_t g_aftPuerta   = 0;
inline uint32_t g_aftReuso    = 0;
inline uint32_t g_aftBusq     = 0;
inline uint32_t g_aftDstBuf   = 0;
inline uint32_t g_aftSrcBuf   = 0;
inline uint32_t g_aftDibujo   = 0;
inline uint32_t g_aftSinVfb   = 0;
inline uint32_t g_aftEstricta = 0;   // rechazos de la escotilla por bpp que no calza (nivel 3)
inline uint64_t g_tkAftBusq   = 0;
inline uint64_t g_tkAftDib    = 0;
inline int      g_crono       = 0;   // lo enciende diag-bloques; en produccion queda en 0

// --- STV(aft): RADIOGRAFIA de los DrawPixels que ejecuta After --------------
// No es una tijera: es para decidir QUE son esas franjas antes de tocarlas.
// Tabla de hasta 4 FORMAS DISTINTAS (no 4 muestras sueltas): con la forma, el
// vfb que calzo y el rango de destinos barrido se separa una escritura
// sistematica del pipeline del juego de un falso positivo de la heuristica de
// stride (FindTransferFramebuffer:2309-2313, la que el propio upstream dice
// que le complica la vida a God of War).
// La clave es EXACTA, no hasheada: (ds, w, h, bpp, fbAddr, fbStride).
// Si aparecen mas de 4 formas, se cuenta el desborde en vez de mentir.
enum : uint32_t { kMaxDib = 4 };

struct MuestraDib {
	uint32_t ds, w, h, bpp;              // la transferencia
	uint32_t fbAddr, fbStride, fbFmt;    // el vfb que calzo
	uint32_t fbW, fbH;
	int32_t  rx, ry, rw, rh;             // el rect que resolvio la busqueda
	uint32_t dstMin, dstMax;             // rango de destinos barrido por esta forma
	uint32_t n;
	bool     usada;
};

inline MuestraDib g_dib[kMaxDib];
inline uint32_t   g_dibDesborde = 0;

inline void AnotarDib(uint32_t dst, uint32_t ds, uint32_t w, uint32_t h, uint32_t bpp,
                      uint32_t fbAddr, uint32_t fbStride, uint32_t fbFmt,
                      uint32_t fbW, uint32_t fbH,
                      int rx, int ry, int rw, int rh) {
	if (!g_crono) return;   // en produccion no se anota nada
	for (uint32_t i = 0; i < kMaxDib; i++) {
		MuestraDib &m = g_dib[i];
		if (!m.usada) {
			m.usada = true;
			m.ds = ds; m.w = w; m.h = h; m.bpp = bpp;
			m.fbAddr = fbAddr; m.fbStride = fbStride; m.fbFmt = fbFmt;
			m.fbW = fbW; m.fbH = fbH;
			m.rx = rx; m.ry = ry; m.rw = rw; m.rh = rh;
			m.dstMin = m.dstMax = dst;
			m.n = 1;
			return;
		}
		if (m.ds == ds && m.w == w && m.h == h && m.bpp == bpp &&
		    m.fbAddr == fbAddr && m.fbStride == fbStride) {
			m.n++;
			if (dst < m.dstMin) m.dstMin = dst;
			if (dst > m.dstMax) m.dstMax = dst;
			return;
		}
	}
	g_dibDesborde++;
}

inline uint64_t Ticks() {
#if defined(__aarch64__)
	uint64_t t;
	asm volatile("mrs %0, cntvct_el0" : "=r"(t));
	return t;
#else
	return 0;
#endif
}

// Lectura de reloj que respeta el interruptor del instrumento: apagado es una
// rama y un cero, igual que en diag-bloques.
inline uint64_t Marca() { return g_crono ? Ticks() : 0; }

inline bool g_hola = false;

inline void Reiniciar() {
	g_diferidas = 0;
	g_fusionadas = 0;
	g_pasadas = 0;
	g_vaciados[MOT_USO] = 0;
	g_vaciados[MOT_FIN] = 0;
	g_vaciados[MOT_UMBRAL] = 0;
	g_aftLlamadas = g_aftPuerta = g_aftReuso = g_aftBusq = 0;
	g_aftDstBuf = g_aftSrcBuf = g_aftDibujo = g_aftSinVfb = g_aftEstricta = 0;
	g_tkAftBusq = g_tkAftDib = 0;
	for (uint32_t i = 0; i < kMaxDib; i++) g_dib[i].usada = false;
	g_dibDesborde = 0;
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
// NIVELES (STV(aft)):
//   0 = todo upstream: ni fusion de Invalidate ni reuso de la busqueda
//   1 = solo fusion de Invalidate (exactamente el build anterior)
//   2 = fusion + reuso de la busqueda de NotifyBlockTransferBefore
//   3 = nivel 2 + HEURISTICA ESTRICTA: la escotilla de stride de
//       FindTransferFramebuffer deja de aceptar candidatos cuyo bpp de pixel
//       no coincide con el de la transferencia. NO es default: primero tiene
//       APROBADO EN BANCO: +5,0 % consistente (3 pares, 56,24 vs 53,55), con
//       la GPU subiendo de 91 a 94 % entregando MAS cuadros — o sea que el
//       ahorro descuenta de CPU y de GPU a la vez. Visual sin diferencia por
//       encima del ruido del metodo, con control honesto, y el testigo de
//       precision quirurgico (aft_dib=0, la escotilla sola).  <- DEFAULT
// Cualquier valor >=2 se toma como 2. Un caracter que NO sea digito se toma
// como el default, no como cero: un typo en el setprop no puede apagar la
// palanca en silencio.
inline constexpr int kNivelMax = 3;
inline constexpr int kNivelDefecto = 3;

inline int NivelDeTexto(const char *s) {
	if (!s || !*s) return kNivelDefecto;
	if (*s < '0' || *s > '9') return kNivelDefecto;
	const int v = *s - '0';
	return v > kNivelMax ? kNivelMax : v;
}

inline int g_fusion = kNivelDefecto;
inline bool g_resuelto = false;
inline constexpr uint32_t kFramesRevision = 32;
inline uint32_t g_cuentaRevision = kFramesRevision - 1;

inline int ResolverFusion() {
	const char *e = getenv("STV_EPI_FUSION");
	if (e && *e) return NivelDeTexto(e);
#if defined(__ANDROID__)
	char prop[PROP_VALUE_MAX] = { 0 };
	if (__system_property_get("debug.stv.epi", prop) > 0 && prop[0]) {
		return NivelDeTexto(prop);
	}
#endif
	return kNivelDefecto;   // ausente = nivel 3
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
		snprintf(b, sizeof(b), "STV: epi NIVEL %d (%s) — 0=upstream 1=fusion 2=fusion+reuso",
			g_fusion, kMarca);
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
