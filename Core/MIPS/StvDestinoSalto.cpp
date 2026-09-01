#include "Core/MIPS/StvDestinoSalto.h"

#include <cstring>
#include <cstdlib>

#include "Common/Log.h"
#include "Core/MIPS/IR/IRInst.h"

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

alignas(64) SitioIC g_icSitios[kSitiosICMax];
uint64_t g_icVia0, g_icVia1, g_icFallo;
uint64_t g_lineaFallos, g_lineaParches, g_lineaCongelados;
uint64_t g_irRepliegue[256];
FnOlvidarPc g_alOlvidarPc;
FnReiniciar g_alReiniciar;
FnReiniciar g_alVolcarMapa;
static int s_icModo;
static int s_lineaModo;
static int s_lineaTope;
static int s_lineaAdapt;
static int s_repliegueModo;
static int s_mapaModo;

static int s_icProximo;   // se reparte al compilar; se reinicia con la cache

static bool s_leido;
static int s_modo;
static int s_bits;
static int s_mezcla;

static void LeerValvula() {
	if (s_leido)
		return;
	s_leido = true;
	// APAGADA POR DEFECTO. Medida: cuesta 1,7 % (53,3 -> 52,4 VPS). Con 95,2 %
	// de aciertos el mecanismo no falla: es que cambia un acceso a L2 por otro
	// acceso a L2 mas 5 instrucciones, y a 4,9 M de despachos por segundo esas
	// instrucciones solas ya son ~1 % del reloj. Queda con valvula porque los
	// contadores (tasa y despachos/s) son el instrumento para el proximo
	// intento, que tiene que sacar el acceso a memoria, no mudarlo.
	s_modo = 0;
	s_bits = 11;   // 2048 entradas = 16 KB
	s_mezcla = 0;
#ifdef __ANDROID__
	char v[PROP_VALUE_MAX];
	if (__system_property_get("debug.stv.destino", v) > 0 && v[0])
		s_modo = atoi(v);
	if (__system_property_get("debug.stv.ic", v) > 0 && v[0])
		s_icModo = atoi(v);
	if (__system_property_get("debug.stv.irfall", v) > 0 && v[0])
		s_repliegueModo = atoi(v);
	if (__system_property_get("debug.stv.irmapa", v) > 0 && v[0])
		s_mapaModo = atoi(v);
	if (__system_property_get("debug.stv.iclinea", v) > 0 && v[0])
		s_lineaModo = atoi(v);
	s_lineaTope = 4096;
	if (__system_property_get("debug.stv.iclinea.tope", v) > 0 && v[0])
		s_lineaTope = atoi(v);
	s_lineaAdapt = 4;
	if (__system_property_get("debug.stv.iclinea.adapt", v) > 0 && v[0])
		s_lineaAdapt = atoi(v);
	if (s_lineaAdapt < 1)
		s_lineaAdapt = 1;
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
int ModoIC() { LeerValvula(); return s_icModo; }
int ModoLinea() { LeerValvula(); return s_lineaModo; }
int ModoRepliegue() { LeerValvula(); return s_repliegueModo; }
int ModoMapa() { LeerValvula(); return s_mapaModo; }
void AvisarMapa(int n) { STV_LOG("STVIRMAPA: %d entradas en /data/local/tmp/stv_irmapa.txt", n); }

void VolcarRepliegue() {
	// Se informa el TRAMO del ultimo segundo, no el acumulado: el arranque
	// compila y replega muchisimo mas que el regimen, y el acumulado lo tapa.
	static uint64_t ult[256];
	struct { int op; uint64_t n; } top[256];
	int m = 0;
	uint64_t total = 0;
	for (int i = 0; i < 256; ++i) {
		uint64_t d = g_irRepliegue[i] - ult[i];
		ult[i] = g_irRepliegue[i];
		if (d) { top[m].op = i; top[m].n = d; ++m; total += d; }
	}
	if (!total) {
		STV_LOG("STVREPLIEGUE: 0 operaciones sin compilar (el backend las cubre todas)");
		return;
	}
	for (int i = 1; i < m; ++i) {           // insercion: m es chico
		auto t = top[i];
		int j = i - 1;
		while (j >= 0 && top[j].n < t.n) { top[j + 1] = top[j]; --j; }
		top[j + 1] = t;
	}
	char b[512];
	int p = snprintf(b, sizeof(b), "STVREPLIEGUE: %llu/s en %d ops |", (unsigned long long)total, m);
	for (int i = 0; i < m && i < 6 && p < (int)sizeof(b) - 40; ++i) {
		const IRMeta *meta = GetIRMeta((IROp)top[i].op);
		p += snprintf(b + p, sizeof(b) - p, " %s=%llu",
			meta && meta->name ? meta->name : "?", (unsigned long long)top[i].n);
	}
	STV_LOG("%s", b);
}
int TopeLinea() { LeerValvula(); return s_lineaTope; }
int AdaptLinea() { LeerValvula(); return s_lineaAdapt; }

int SiguienteSitioIC() {
	// Si se pasa, se reusa el ultimo: la medicion queda algo sucia en la cola
	// pero no se pisa memoria ajena. Se avisa una sola vez.
	if (s_icProximo >= kSitiosICMax) {
		static bool avisado;
		if (!avisado) { avisado = true; STV_LOG("STVIC: se acabaron los sitios (%d)", kSitiosICMax); }
		return kSitiosICMax - 1;
	}
	return s_icProximo++;
}

extern "C" uint32_t StvAnotarIC(uint32_t pc, uint32_t sitio) {
	SitioIC &s = g_icSitios[sitio & (kSitiosICMax - 1)];
	if (s.via0 == pc) {
		++g_icVia0;
		return pc;
	}
	if (s.via1 == pc) {
		++g_icVia1;
	} else {
		++g_icFallo;
	}
	s.via1 = s.via0;   // move-to-front: mide 1 via y 2 vias de una sola pasada
	s.via0 = pc;
	return pc;
}

void OlvidarTodos() {
	// Se avisa ANTES de nada: esto corre al principio de IRBlockCache::Clear,
	// o sea antes del bucle que destruye bloque por bloque. Vaciando aca el
	// registro de sitios, los OlvidarDestino de ese bucle no encuentran nada y
	// no se ponen a parchear codigo que esta por desaparecer igual.
	if (g_alReiniciar)
		g_alReiniciar();
	memset(g_destinos, 0xFF, sizeof(g_destinos));
	// Los sitios se recompilan desde cero, asi que los indices se reparten de
	// nuevo: sin reiniciar, dos sitios distintos comparten ranura y la medicion
	// miente hacia abajo.
	memset(g_icSitios, 0xFF, sizeof(g_icSitios));
	s_icProximo = 0;
}

void OlvidarDestino(uint32_t pc) {
	// Un sitio de la cache en linea que prediga este pc saltaria a la
	// traduccion vieja: hay que despredecirlo.
	if (g_alOlvidarPc)
		g_alOlvidarPc(pc);
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
	uint64_t ic = g_icVia0 + g_icVia1 + g_icFallo;
	if (ic) {
		static uint64_t u0, u1, uf;
		uint64_t d0 = g_icVia0 - u0, d1 = g_icVia1 - u1, df = g_icFallo - uf;
		u0 = g_icVia0; u1 = g_icVia1; uf = g_icFallo;
		uint64_t dt = d0 + d1 + df;
		if (dt)
			STV_LOG("STVIC[%s]: 1 via %.2f %%  2 vias %.2f %%  (%llu saltos, %d sitios)",
				motivo, 100.0 * (double)d0 / (double)dt,
				100.0 * (double)(d0 + d1) / (double)dt,
				(unsigned long long)dt, s_icProximo);
	}
	if (s_lineaModo > 0) {
		static uint64_t uf, up, uc;
		uint64_t df = g_lineaFallos - uf, dp = g_lineaParches - up;
		uf = g_lineaFallos; up = g_lineaParches; uc = g_lineaCongelados;
		// La tasa se calcula contra los saltos indirectos por segundo que midio
		// debug.stv.ic en esta misma escena. Si la escena cambia, cambiar esto.
		const double kSaltosPorSeg = 4700000.0;
		STV_LOG("STVLINEA[%s]: fallos/s=%llu (~%.1f %% de acierto)  parches/s=%llu  congelados=%llu",
			motivo, (unsigned long long)df,
			100.0 * (1.0 - (double)df / kSaltosPorSeg),
			(unsigned long long)dp, (unsigned long long)uc);
	}
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
