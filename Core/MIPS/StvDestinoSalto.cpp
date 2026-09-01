#include "Core/MIPS/StvDestinoSalto.h"

#include <cstring>
#include <cstdlib>

#include "Common/Log.h"

// INFO_LOG(Log::JIT, ...) NO llega a logcat en este aparato: ese canal esta
// filtrado. El testigo que no se puede leer no es un testigo. Se usa la misma
// via directa que ya usa la traza STVJIT de Arm64IRAsm.cpp.
#if defined(__ANDROID__)
#include <android/log.h>
#define STV_LOG(...) __android_log_print(ANDROID_LOG_INFO, "STV", __VA_ARGS__)
#else
#include <cstdio>
#define STV_LOG(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace stvjit {

alignas(64) EntradaDestino g_destinos[kDestinosMax];
uint64_t g_aciertos;
uint64_t g_fallos;

static bool s_leido;
static int s_modo;
static int s_bits;
static int s_mezcla;

static void LeerValvula() {
	if (s_leido)
		return;
	s_leido = true;
	s_modo = 1;
	s_bits = 11;   // 2048 entradas = 16 KB
	s_mezcla = 0;
#ifdef __ANDROID__
	char v[PROP_VALUE_MAX];
	if (__system_property_get("debug.stv.destino", v) > 0 && v[0])
		s_modo = atoi(v);
	if (__system_property_get("debug.stv.destino.mezcla", v) > 0 && v[0])
		s_mezcla = atoi(v);
	if (__system_property_get("debug.stv.destino.bits", v) > 0 && v[0]) {
		int b = atoi(v);
		if (b >= 6 && b <= kBitsMax)
			s_bits = b;
	}
#endif
	OlvidarTodos();
	STV_LOG("STVDESTINO: modo=%d bits=%d mezcla=%d (%d entradas, %d KB)",
		s_modo, s_bits, s_mezcla, 1 << s_bits,
		(int)((1 << s_bits) * sizeof(EntradaDestino) / 1024));
}

int ModoCache() { LeerValvula(); return s_modo; }
int BitsCache() { LeerValvula(); return s_bits; }
int MezclaCache() { LeerValvula(); return s_mezcla; }

void OlvidarTodos() {
	memset(g_destinos, 0xFF, sizeof(g_destinos));
}

void OlvidarDestino(uint32_t pc) {
	// Se limpia en TODOS los anchos posibles: el ancho vivo se fija al generar
	// el codigo, pero esta llamada puede llegar antes de eso. Barrer de mas es
	// gratis; barrer de menos deja una entrada que apunta a codigo liberado.
	for (int b = 6; b <= kBitsMax; ++b) {
		for (int m = 0; m < 2; ++m) {
			EntradaDestino &e = g_destinos[IndiceDestino(pc, b, m)];
			if (e.pc == pc)
				e.pc = 0xFFFFFFFFu;
		}
	}
}

void VolcarTestigo(const char *motivo) {
	uint64_t t = g_aciertos + g_fallos;
	if (!t)
		return;
	// El acumulado solo dice el promedio desde que arranco, y el arranque es
	// justo el tramo con la tabla fria: aplasta la tasa de regimen. Se informa
	// tambien el TRAMO desde el ultimo volcado, que es lo que hay que mirar.
	static uint64_t ultA, ultF;
	uint64_t dA = g_aciertos - ultA, dF = g_fallos - ultF;
	ultA = g_aciertos; ultF = g_fallos;
	uint64_t dt = dA + dF;
	STV_LOG("STVDESTINO[%s]: tramo %.2f %% (%llu saltos)  acumulado %.2f %% (aciertos=%llu fallos=%llu)",
		motivo,
		dt ? 100.0 * (double)dA / (double)dt : 0.0, (unsigned long long)dt,
		100.0 * (double)g_aciertos / (double)t,
		(unsigned long long)g_aciertos, (unsigned long long)g_fallos);
}

}  // namespace stvjit
