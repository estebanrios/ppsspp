#include "Core/MIPS/StvDestinoSalto.h"

#include <cstring>
#include <cstdlib>

#include "Common/Log.h"

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

static void LeerValvula() {
	if (s_leido)
		return;
	s_leido = true;
	s_modo = 1;
	s_bits = 11;   // 2048 entradas = 16 KB
#ifdef __ANDROID__
	char v[PROP_VALUE_MAX];
	if (__system_property_get("debug.stv.destino", v) > 0 && v[0])
		s_modo = atoi(v);
	if (__system_property_get("debug.stv.destino.bits", v) > 0 && v[0]) {
		int b = atoi(v);
		if (b >= 6 && b <= kBitsMax)
			s_bits = b;
	}
#endif
	OlvidarTodos();
	INFO_LOG(Log::JIT, "STVDESTINO: modo=%d bits=%d (%d entradas, %d KB)",
		s_modo, s_bits, 1 << s_bits, (int)((1 << s_bits) * sizeof(EntradaDestino) / 1024));
}

int ModoCache() { LeerValvula(); return s_modo; }
int BitsCache() { LeerValvula(); return s_bits; }

void OlvidarTodos() {
	memset(g_destinos, 0xFF, sizeof(g_destinos));
}

void OlvidarDestino(uint32_t pc) {
	// Se limpia en TODOS los anchos posibles: el ancho vivo se fija al generar
	// el codigo, pero esta llamada puede llegar antes de eso. Barrer de mas es
	// gratis; barrer de menos deja una entrada que apunta a codigo liberado.
	for (int b = 6; b <= kBitsMax; ++b) {
		EntradaDestino &e = g_destinos[IndiceDestino(pc, b)];
		if (e.pc == pc)
			e.pc = 0xFFFFFFFFu;
	}
}

void VolcarTestigo(const char *motivo) {
	uint64_t t = g_aciertos + g_fallos;
	if (!t)
		return;
	INFO_LOG(Log::JIT, "STVDESTINO[%s]: aciertos=%llu fallos=%llu  tasa=%.2f %%",
		motivo, (unsigned long long)g_aciertos, (unsigned long long)g_fallos,
		100.0 * (double)g_aciertos / (double)t);
}

}  // namespace stvjit
