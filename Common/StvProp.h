#pragma once
// STV: lectura de una prop de Android como entero, cacheada la primera vez.
// Las valvulas experimentales se leen UNA vez por proceso (al crear el
// contexto o las imagenes); cambiarlas exige relanzar el juego.
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <android/log.h>
#include <cstdlib>
inline int StvPropInt(const char *nombre) {
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(nombre, v) > 0) return atoi(v);
	return 0;
}
// Igual, pero con un valor por defecto cuando la prop NO esta puesta.
inline int StvPropDef(const char *nombre, int def) {
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(nombre, v) > 0 && v[0]) return atoi(v);
	return def;
}
#define STV_LOG(...) __android_log_print(ANDROID_LOG_INFO, "STV", __VA_ARGS__)
#else
inline int StvPropInt(const char *) { return 0; }
inline int StvPropDef(const char *, int def) { return def; }
#define STV_LOG(...) do {} while (0)
#endif

// STV_AB_v1 (instrumento, arco GoS 1:1 STV 2026-10-02): fase del A/B intercalado POR CUADRO. La
// lee DrawEngineVulkan::BeginFrame de debug.stv.ab (0 = A, 1 = B) una vez por cuadro; en la fase B
// la lib agrega un pase de marca (un trabajo de fragmentos mas por cuadro) para que el lector de
// contadores clasifique cada muestra por su contenido y no por el reloj (en el volcado la cola de
// cuadros en vuelo llega a ~1 s). StvAbActivo(): una valvula gateada solo actua en la fase B si
// debug.stv.abgate=1; sin abgate actua siempre.
inline int &StvAbFase() { static int v = 0; return v; }
inline int &StvAbGate() { static int v = 0; return v; }
inline bool StvAbActivo() { return !StvAbGate() || StvAbFase() == 1; }

// STV_SOMBRA_RECORTE_v1: generacion de escrituras de VRAM fuera de los draws (copias de bloque,
// memset, copias entre framebuffers). FramebufferManagerCommon la incrementa; el seguimiento de areas
// negras de StvSombra se invalida si cambio desde el inicio del cuadro.
inline int &StvVramGen() { static int g = 0; return g; }
// Rangos [direccion, bytes) escritos fuera de los draws desde la ultima consulta.
#include <vector>
#include <utility>
inline std::vector<std::pair<unsigned, unsigned>> &StvVramRangos() { static std::vector<std::pair<unsigned, unsigned>> v; return v; }
inline void StvVramEscrita(unsigned addr, unsigned bytes) {
	auto &v = StvVramRangos();
	if (v.size() < 4096) v.push_back({ addr, bytes }); else v.back() = { 0x04000000u, 0x00200000u };  // desborde: toda la VRAM
	StvVramGen()++;
}

// STV_VARY_PACK_v1: empaquetar textura+niebla en una varying vec4 (Vulkan). Se decide UNA vez por
// proceso (debug.stv.vpack, defecto 0: medido en vivo sin ganancia, 2026-10-02) y lo consultan el generador de VS y el de FS: tienen que coincidir.
// La compilacion con culling por geometry shader no lo usa (ese GS espera los nombres separados).
inline bool &StvVaryPackGS() { static bool g = false; return g; }
inline bool StvVaryPack() { static int v = -1; if (v < 0) v = StvPropDef("debug.stv.vpack", 0); return v == 1 && !StvVaryPackGS(); }
