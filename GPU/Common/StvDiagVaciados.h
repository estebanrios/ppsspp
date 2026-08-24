// =============================================================================
// STV — INSTRUMENTO: por que se corta el lote de draws de PPSSPP
//
// Parche del proyecto STV-TSPS-Android (tools/ppsspp/parches/diag-vaciados.sh).
// NO es codigo de upstream. Todo lo que hace es CONTAR; no cambia ni una
// decision de dibujo.
//
// Causas 0..255   -> el numero de comando GE que disparo FLAG_FLUSHBEFOREONCHANGE
//                    en GPUCommonHW::FastRunLoop. El nombre sale de ge_constants.h.
// Causas 256..    -> los sitios de Flush() explicitos del codigo.
//
// Encendido en caliente (se re-evalua 1 vez por segundo):
//   1. entorno   STV_DIAG_VACIADOS=1
//   2. propiedad debug.stv.diag=1
//   3. archivo   <memstick>/PSP/SYSTEM/stv-diag-vaciados
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

namespace stvdiag {

// Cadena que sirve para probar que el parche entro en el .so:
//   grep -a STV_DIAG_VACIADOS_v1 libppsspp_jni.so
inline constexpr const char *kMarca = "STV_DIAG_VACIADOS_v1";

enum : uint16_t {
	CAUSA_SIN_MARCAR = 256,
	CAUSA_INTERPRETE_LENTO,
	CAUSA_PRESENTAR,
	CAUSA_VERTEXTYPE_SKIN,
	CAUSA_PRIM_CULL_FLIP,
	CAUSA_BEZIER,
	CAUSA_SPLINE,
	CAUSA_BLOCKTRANSFER,
	CAUSA_TEXLEVEL,
	CAUSA_MTX_WORLD,
	CAUSA_MTX_WORLD_LENTO,
	CAUSA_MTX_VIEW,
	CAUSA_MTX_VIEW_LENTO,
	CAUSA_MTX_PROJ,
	CAUSA_MTX_PROJ_LENTO,
	CAUSA_MTX_TGEN,
	CAUSA_MTX_TGEN_LENTO,
	CAUSA_MTX_BONE,
	CAUSA_MTX_BONE_LENTO,
	CAUSA_MTX_BONE_RAPIDA,
	CAUSA_FIN_LISTA,
	CAUSA_IMM_ANTES,
	CAUSA_IMM_DESPUES,
	CAUSA_IMM_DESPACHO,
	CAUSA_TIPO_DIBUJO,
	CAUSA_PRIM_INCOMPAT,
	CAUSA_PRIM_INCOMPAT_SALTO,
	CAUSA_TOPE_VERTS,
	CAUSA_TOPE_INDS,
	CAUSA_TOPE_BUFFER,
	CAUSA_AUTOTEXTURA,
	CAUSA_CURVA,
	CAUSA_FRAMEBUF_MGR,
	CAUSA_FIN_DIFERIDO,
	NUM_CAUSAS
};

// Bits de gstate_c.dirty que se ensucian pero que NO obligan a la GPU a hacer
// un draw nuevo (ver la generacion de esta constante en el parche).
inline constexpr uint64_t kMascaraInocua = (1ULL << 43) | (1ULL << 44);

struct Contador {
	uint32_t llamadas;   // Flush() invocado con esta causa (lote vacio incluido)
	uint32_t cortes;     // de esos, los que de verdad cortaron un lote
	uint32_t unitarios;  // cortes donde el lote llevaba UN solo draw
	uint32_t inutiles;   // cortes donde NADA que le importe a la GPU habia cambiado
	uint32_t maxDraws;   // el lote mas grande que corto esta causa
	uint64_t draws;      // suma de numDrawInds_  (comandos PRIM del PSP)
	uint64_t vsub;       // suma de vertexCountInDrawCalls_ (vertices del PSP)
	uint64_t vdec;       // suma de numDecodedVerts_ (vertices que van a la GPU)
	uint64_t dirtyOr;    // OR de las mascaras DIRTY_ vistas en los cortes de esta causa
};

inline Contador  g_cont[NUM_CAUSAS];
inline uint32_t  g_dirtyHist[64];   // cuantos cortes traian cada bit DIRTY_ puesto
inline uint64_t  g_dirtyPend = 0;   // mascara capturada al ENTRAR a Flush()
inline uint16_t  g_causa   = CAUSA_SIN_MARCAR;
inline int       g_activo  = 0;     // 0 apagado, 1 encendido
inline uint32_t  g_cuadros = 0;
inline uint32_t  g_abortos = 0;     // Flush() que abandono por pipeline mala
inline double    g_t0      = 0.0;
inline double    g_tQueja  = 0.0;   // ultima vez que avisamos que esta apagado

inline const char *NombreCausa(unsigned c) {
	static const char *const geNombres[256] = {
	"NOP",
	"VADDR",
	"IADDR",
	"UNKNOWN_03",
	"PRIM",
	"BEZIER",
	"SPLINE",
	"BOUNDINGBOX",
	"JUMP",
	"BJUMP",
	"CALL",
	"RET",
	"END",
	"UNKNOWN_0D",
	"SIGNAL",
	"FINISH",
	"BASE",
	"UNKNOWN_11",
	"VERTEXTYPE",
	"OFFSETADDR",
	"ORIGIN",
	"REGION1",
	"REGION2",
	"LIGHTINGENABLE",
	"LIGHTENABLE0",
	"LIGHTENABLE1",
	"LIGHTENABLE2",
	"LIGHTENABLE3",
	"DEPTHCLAMPENABLE",
	"CULLFACEENABLE",
	"TEXTUREMAPENABLE",
	"FOGENABLE",
	"DITHERENABLE",
	"ALPHABLENDENABLE",
	"ALPHATESTENABLE",
	"ZTESTENABLE",
	"STENCILTESTENABLE",
	"ANTIALIASENABLE",
	"PATCHCULLENABLE",
	"COLORTESTENABLE",
	"LOGICOPENABLE",
	"UNKNOWN_29",
	"BONEMATRIXNUMBER",
	"BONEMATRIXDATA",
	"MORPHWEIGHT0",
	"MORPHWEIGHT1",
	"MORPHWEIGHT2",
	"MORPHWEIGHT3",
	"MORPHWEIGHT4",
	"MORPHWEIGHT5",
	"MORPHWEIGHT6",
	"MORPHWEIGHT7",
	"UNKNOWN_34",
	"UNKNOWN_35",
	"PATCHDIVISION",
	"PATCHPRIMITIVE",
	"PATCHFACING",
	"UNKNOWN_39",
	"WORLDMATRIXNUMBER",
	"WORLDMATRIXDATA",
	"VIEWMATRIXNUMBER",
	"VIEWMATRIXDATA",
	"PROJMATRIXNUMBER",
	"PROJMATRIXDATA",
	"TGENMATRIXNUMBER",
	"TGENMATRIXDATA",
	"VIEWPORTXSCALE",
	"VIEWPORTYSCALE",
	"VIEWPORTZSCALE",
	"VIEWPORTXCENTER",
	"VIEWPORTYCENTER",
	"VIEWPORTZCENTER",
	"TEXSCALEU",
	"TEXSCALEV",
	"TEXOFFSETU",
	"TEXOFFSETV",
	"OFFSETX",
	"OFFSETY",
	"UNKNOWN_4E",
	"UNKNOWN_4F",
	"SHADEMODE",
	"REVERSENORMAL",
	"UNKNOWN_52",
	"MATERIALUPDATE",
	"MATERIALEMISSIVE",
	"MATERIALAMBIENT",
	"MATERIALDIFFUSE",
	"MATERIALSPECULAR",
	"MATERIALALPHA",
	"UNKNOWN_59",
	"UNKNOWN_5A",
	"MATERIALSPECULARCOEF",
	"AMBIENTCOLOR",
	"AMBIENTALPHA",
	"LIGHTMODE",
	"LIGHTTYPE0",
	"LIGHTTYPE1",
	"LIGHTTYPE2",
	"LIGHTTYPE3",
	"LX0",
	"LY0",
	"LZ0",
	"LX1",
	"LY1",
	"LZ1",
	"LX2",
	"LY2",
	"LZ2",
	"LX3",
	"LY3",
	"LZ3",
	"LDX0",
	"LDY0",
	"LDZ0",
	"LDX1",
	"LDY1",
	"LDZ1",
	"LDX2",
	"LDY2",
	"LDZ2",
	"LDX3",
	"LDY3",
	"LDZ3",
	"LKA0",
	"LKB0",
	"LKC0",
	"LKA1",
	"LKB1",
	"LKC1",
	"LKA2",
	"LKB2",
	"LKC2",
	"LKA3",
	"LKB3",
	"LKC3",
	"LKS0",
	"LKS1",
	"LKS2",
	"LKS3",
	"LKO0",
	"LKO1",
	"LKO2",
	"LKO3",
	"LAC0",
	"LDC0",
	"LSC0",
	"LAC1",
	"LDC1",
	"LSC1",
	"LAC2",
	"LDC2",
	"LSC2",
	"LAC3",
	"LDC3",
	"LSC3",
	"CULL",
	"FRAMEBUFPTR",
	"FRAMEBUFWIDTH",
	"ZBUFPTR",
	"ZBUFWIDTH",
	"TEXADDR0",
	"TEXADDR1",
	"TEXADDR2",
	"TEXADDR3",
	"TEXADDR4",
	"TEXADDR5",
	"TEXADDR6",
	"TEXADDR7",
	"TEXBUFWIDTH0",
	"TEXBUFWIDTH1",
	"TEXBUFWIDTH2",
	"TEXBUFWIDTH3",
	"TEXBUFWIDTH4",
	"TEXBUFWIDTH5",
	"TEXBUFWIDTH6",
	"TEXBUFWIDTH7",
	"CLUTADDR",
	"CLUTADDRUPPER",
	"TRANSFERSRC",
	"TRANSFERSRCW",
	"TRANSFERDST",
	"TRANSFERDSTW",
	"UNKNOWN_B6",
	"UNKNOWN_B7",
	"TEXSIZE0",
	"TEXSIZE1",
	"TEXSIZE2",
	"TEXSIZE3",
	"TEXSIZE4",
	"TEXSIZE5",
	"TEXSIZE6",
	"TEXSIZE7",
	"TEXMAPMODE",
	"TEXSHADELS",
	"TEXMODE",
	"TEXFORMAT",
	"LOADCLUT",
	"CLUTFORMAT",
	"TEXFILTER",
	"TEXWRAP",
	"TEXLEVEL",
	"TEXFUNC",
	"TEXENVCOLOR",
	"TEXFLUSH",
	"TEXSYNC",
	"FOG1",
	"FOG2",
	"FOGCOLOR",
	"TEXLODSLOPE",
	"UNKNOWN_D1",
	"FRAMEBUFPIXFORMAT",
	"CLEARMODE",
	"SCISSOR1",
	"SCISSOR2",
	"MINZ",
	"MAXZ",
	"COLORTEST",
	"COLORREF",
	"COLORTESTMASK",
	"ALPHATEST",
	"STENCILTEST",
	"STENCILOP",
	"ZTEST",
	"BLENDMODE",
	"BLENDFIXEDA",
	"BLENDFIXEDB",
	"DITH0",
	"DITH1",
	"DITH2",
	"DITH3",
	"LOGICOP",
	"ZWRITEDISABLE",
	"MASKRGB",
	"MASKALPHA",
	"TRANSFERSTART",
	"TRANSFERSRCPOS",
	"TRANSFERDSTPOS",
	"UNKNOWN_ED",
	"TRANSFERSIZE",
	"UNKNOWN_EF",
	"VSCX",
	"VSCY",
	"VSCZ",
	"VTCS",
	"VTCT",
	"VTCQ",
	"VCV",
	"VAP",
	"VFC",
	"VSCV",
	"UNKNOWN_FA",
	"UNKNOWN_FB",
	"UNKNOWN_FC",
	"UNKNOWN_FD",
	"UNKNOWN_FE",
	"NOP_FF",
	};
	static const char *const extra[] = {
	"SIN_MARCAR",
	"INTERPRETE_LENTO",
	"PRESENTAR_PANTALLA",
	"VERTEXTYPE_CON_SKIN",
	"PRIM_VOLVER_CULL",
	"BEZIER",
	"SPLINE",
	"BLOCK_TRANSFER",
	"TEXLEVEL_MANUAL",
	"MATRIZ_MUNDO",
	"MATRIZ_MUNDO_LENTA",
	"MATRIZ_VISTA",
	"MATRIZ_VISTA_LENTA",
	"MATRIZ_PROYECCION",
	"MATRIZ_PROYECCION_LENTA",
	"MATRIZ_TEXTURA",
	"MATRIZ_TEXTURA_LENTA",
	"MATRIZ_HUESO",
	"MATRIZ_HUESO_LENTA",
	"MATRIZ_HUESO_RAPIDA",
	"FIN_DE_LISTA",
	"IMM_ANTES",
	"IMM_DESPUES",
	"IMM_DESPACHO",
	"CAMBIO_TIPO_PRIM",
	"PRIM_INCOMPATIBLE",
	"PRIM_INCOMPATIBLE_SALTO",
	"TOPE_128_GRUPOS_VERT",
	"TOPE_512_DRAWS",
	"TOPE_65536_VERTICES",
	"AUTOTEXTURA_RECT",
	"CURVA_TESELADA",
	"GESTOR_FRAMEBUFFER",
	"FIN_DIFERIDO_STALL",
	};
	if (c < 256) return geNombres[c];
	if (c < NUM_CAUSAS) return extra[c - 256];
	return "FUERA_DE_RANGO";
}

inline const char *NombreDirty(unsigned b) {
	static const char *const dNombres[64] = {
		"PROJMATRIX",
		"PROJTHROUGHMATRIX",
		"FOGCOLOR",
		"FOGCOEF",
		"TEXENV",
		"ALPHACOLORREF",
		"STENCILREPLACEVALUE",
		"ALPHACOLORMASK",
		"LIGHT0",
		"LIGHT1",
		"LIGHT2",
		"LIGHT3",
		"MATDIFFUSE",
		"MATSPECULAR",
		"MATEMISSIVE",
		"AMBIENT",
		"MATAMBIENTALPHA",
		"SHADERBLEND",
		"UVSCALEOFFSET",
		"DEPTHRANGE",
		"BIT_20",
		"WORLDMATRIX",
		"VIEWMATRIX",
		"TEXMATRIX",
		"BONEMATRIX0",
		"BONEMATRIX1",
		"BONEMATRIX2",
		"BONEMATRIX3",
		"BONEMATRIX4",
		"BONEMATRIX5",
		"BONEMATRIX6",
		"BONEMATRIX7",
		"BEZIERSPLINE",
		"TEXCLAMP",
		"CULLRANGE",
		"DEPAL",
		"COLORWRITEMASK",
		"MIPBIAS",
		"LIGHT_CONTROL",
		"TEX_ALPHA_MUL",
		"BIT_40",
		"BIT_41",
		"BIT_42",
		"CULL_PLANES",
		"FRAMEBUF",
		"TEXTURE_IMAGE",
		"TEXTURE_PARAMS",
		"BLEND_STATE",
		"DEPTHSTENCIL_STATE",
		"RASTER_STATE",
		"VIEWPORTSCISSOR_STATE",
		"VERTEXSHADER_STATE",
		"FRAGMENTSHADER_STATE",
		"GEOMETRYSHADER_STATE",
		"BIT_54",
		"BIT_55",
		"BIT_56",
		"BIT_57",
		"BIT_58",
		"BIT_59",
		"BIT_60",
		"BIT_61",
		"BIT_62",
		"BIT_63",
	};
	return b < 64 ? dNombres[b] : "FUERA_DE_RANGO";
}

inline void Emitir(const char *linea) {
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "STV", "%s", linea);
#else
	printf("%s\n", linea);
	fflush(stdout);
#endif
}

// --- camino caliente --------------------------------------------------------
// Una llamada a Flush(), con el lote todavia sin tocar.
//
// OJO, esto es lo que hace que el instrumento no pueda mentir: CADA llamada a
// Flush() consume exactamente una causa. Si el lote venia vacio, Flush() se va
// por el return temprano y AlCortar() no va a correr, asi que la causa la
// devolvemos aca mismo. Sin esto, una causa vieja quedaria colgada y el
// proximo corte —justo el que podria venir de un sitio que NO instrumentamos—
// se le achacaria al comando anterior en vez de caer en SIN_MARCAR.
//
// El segundo parametro es gstate_c.dirty TAL COMO ESTA AL ENTRAR a Flush(). Eso
// es justo lo que hace falta y no se puede leer mas tarde: Flush() limpia esos
// bits mientras trabaja. Semantica exacta: son los cambios de estado ocurridos
// DESDE el corte anterior y que este draw tiene que aplicarle a la GPU. Y la
// consecuencia util: si la mascara viene vacia (o solo con bits inocuos), este
// draw pide EXACTAMENTE el mismo estado de GPU que el anterior, o sea que el
// corte no compro nada. Ese es el numero que separa "inherente al PSP" de
// "artefacto de PPSSPP" sin tener que discutirlo.
inline void AlLlamar(int numDrawVerts, uint64_t dirty) {
	if (!g_activo) {
		g_causa = CAUSA_SIN_MARCAR;
		return;
	}
	unsigned c = g_causa;
	if (c >= NUM_CAUSAS) c = CAUSA_SIN_MARCAR;
	g_cont[c].llamadas++;
	g_dirtyPend = dirty;
	if (!numDrawVerts) {
		// lote vacio: nadie mas va a consumir la causa
		g_causa = CAUSA_SIN_MARCAR;
	}
}

// Un corte de verdad. Se llama desde ResetAfterDrawInline(), que es el unico
// punto por el que pasan TODOS los cortes (ahi vive gpuStats.numFlushes++).
inline void AlCortar(int draws, int vsub, int vdec) {
	if (!g_activo) { g_causa = CAUSA_SIN_MARCAR; return; }
	unsigned c = g_causa;
	if (c >= NUM_CAUSAS) c = CAUSA_SIN_MARCAR;
	Contador &k = g_cont[c];
	k.cortes++;
	k.draws += (uint32_t)draws;
	k.vsub  += (uint32_t)vsub;
	k.vdec  += (uint32_t)vdec;
	if (draws == 1) k.unitarios++;
	if ((uint32_t)draws > k.maxDraws) k.maxDraws = (uint32_t)draws;

	const uint64_t d = g_dirtyPend;
	k.dirtyOr |= d;
	if (!(d & ~kMascaraInocua)) k.inutiles++;
	// histograma por bit: solo itera sobre los bits realmente puestos (~5-10)
	uint64_t m = d;
	while (m) {
		g_dirtyHist[__builtin_ctzll(m)]++;
		m &= m - 1;
	}
	g_dirtyPend = 0;
	g_causa = CAUSA_SIN_MARCAR;
}

// Flush() que abandono antes de dibujar (pipeline invalida). No es un corte
// util pero tampoco hay que dejar la causa colgada para el proximo.
inline void AlAbortar() {
	g_abortos++;
	g_causa = CAUSA_SIN_MARCAR;
}

// --- encendido / apagado ----------------------------------------------------
inline bool HayArchivo(const char *memstick) {
	if (!memstick || !*memstick) return false;
	char ruta[1024];
	snprintf(ruta, sizeof(ruta), "%s/PSP/SYSTEM/stv-diag-vaciados", memstick);
	FILE *f = fopen(ruta, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

inline void RutaArchivo(const char *memstick, char *out, size_t n) {
	snprintf(out, n, "%s/PSP/SYSTEM/stv-diag-vaciados", memstick ? memstick : "(sin memstick)");
}

inline int Resolver(const char *memstick, const char **via) {
	const char *e = getenv("STV_DIAG_VACIADOS");
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

// --- volcado ----------------------------------------------------------------
inline void Volcar(double ventana) {
	char b[512];
	uint64_t llam = 0, cort = 0, inut = 0;
	for (unsigned i = 0; i < NUM_CAUSAS; i++) {
		llam += g_cont[i].llamadas; cort += g_cont[i].cortes; inut += g_cont[i].inutiles;
	}
	snprintf(b, sizeof(b), "STV: vac INICIO ventana=%.3f cuadros=%u llam=%llu cort=%llu inut=%llu abort=%u",
		ventana, g_cuadros, (unsigned long long)llam, (unsigned long long)cort,
		(unsigned long long)inut, g_abortos);
	Emitir(b);
	for (unsigned i = 0; i < NUM_CAUSAS; i++) {
		const Contador &k = g_cont[i];
		if (!k.llamadas && !k.cortes) continue;
		snprintf(b, sizeof(b),
			"STV: vac causa=0x%02x nombre=%s llam=%u cort=%u draws=%llu vsub=%llu vdec=%llu uni=%u inut=%u maxd=%u dor=0x%llx",
			i, NombreCausa(i), k.llamadas, k.cortes,
			(unsigned long long)k.draws, (unsigned long long)k.vsub, (unsigned long long)k.vdec,
			k.unitarios, k.inutiles, k.maxDraws, (unsigned long long)k.dirtyOr);
		Emitir(b);
	}
	// Que cambio DE VERDAD en cada corte, mirado por bandera DIRTY_ y no por
	// quien llego primero a llamar a Flush().
	for (unsigned bit = 0; bit < 64; bit++) {
		if (!g_dirtyHist[bit]) continue;
		snprintf(b, sizeof(b), "STV: vac sucio bit=%u nombre=%s cort=%u inocuo=%d",
			bit, NombreDirty(bit), g_dirtyHist[bit],
			((1ULL << bit) & kMascaraInocua) ? 1 : 0);
		Emitir(b);
	}
	Emitir("STV: vac FIN");
}

// Se llama UNA VEZ POR CUADRO, desde DrawEngineVulkan::BeginFrame(), o sea
// fuera de todo bucle de draws.
inline void PorCuadro(const char *memstick) {
	g_cuadros++;
	const double ahora = time_now_d();
	if (g_t0 == 0.0) { g_t0 = ahora; g_tQueja = ahora; }
	const double ventana = ahora - g_t0;
	if (ventana < 1.0) return;

	const char *via = "nada";
	const int antes = g_activo;
	g_activo = Resolver(memstick, &via);

	if (g_activo) {
		if (!antes) {
			char b[512];
			snprintf(b, sizeof(b), "STV: vac ENCENDIDO via=%s (era la primera ventana, los numeros de abajo pueden estar cortados)", via);
			Emitir(b);
		} else {
			Volcar(ventana);
		}
	} else if (ahora - g_tQueja >= 10.0) {
		// Regla del instrumento mudo: apagado tambien habla, y dice como
		// encenderlo. Silencio total en logcat = la lib NO tiene el parche.
		char ruta[1024], b[1280];
		RutaArchivo(memstick, ruta, sizeof(ruta));
		snprintf(b, sizeof(b), "STV: vac APAGADO (%s) — encender con: setprop debug.stv.diag 1  |  touch %s",
			kMarca, ruta);
		Emitir(b);
		g_tQueja = ahora;
	}

	memset(g_cont, 0, sizeof(g_cont));
	memset(g_dirtyHist, 0, sizeof(g_dirtyHist));
	g_abortos = 0;
	g_cuadros = 0;
	g_t0 = ahora;
}

}  // namespace stvdiag
