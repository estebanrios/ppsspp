// Cache de destinos de salto indirecto para el despachador del JIT.
//
// EL PROBLEMA, MEDIDO. Los saltos con destino constante se enlazan bloque a
// bloque (enableBlocklink). Los INDIRECTOS no se pueden enlazar nunca: `jr $ra`
// —el retorno de cada funcion del juego— sale por `ExitToReg` al despachador
// completo. Ahi el despachador lee la palabra marcada (emuhack) desde la RAM
// EMULADA, en la direccion destino, para saber a que bloque nativo saltar.
//
// Esa lectura es el peor acceso posible: el codigo del juego ocupa megabytes y
// el anfitrion nunca lo ejecuta —solo lo lee, 4 bytes, para esta busqueda—, asi
// que cada salto indirecto arrastra una linea de cache nueva que no se reusa.
// Dos perfiles independientes sobre la misma escena lo confirman:
//
//   ciclos de CPU  : 5,08 % en la instruccion que CONSUME esa lectura
//                    (en un nucleo en orden la muestra cae en el consumidor,
//                     no en la carga; la carga misma sale 0,94 %)
//   fallos de cache: 3,16 %, la entrada numero UNO del proceso entero,
//                    por delante de __memcpy y de memset
//
// LA CURA. Una tabla compacta y de mapeo directo pc -> palabra, indexada por
// bits bajos del pc. Todos los saltos indirectos del juego se concentran en
// unos pocos KB que se quedan en L1, en vez de dispersarse por los megabytes de
// la RAM emulada. No cambia la semantica: es un atajo para un dato que el
// despachador ya iba a leer, con verificacion de etiqueta y camino lento
// identico al de antes cuando falla.
//
// SEGURIDAD. La clave guardada es SIEMPRE una direccion de comienzo de bloque
// (es el unico lugar donde vive una marca emuhack). Cuando un bloque se
// destruye o se re-finaliza, su cookie cambia: una entrada vieja saltaria a
// codigo nativo liberado. Por eso `IRBlock::Destroy` y `IRBlock::Finalize`
// olvidan su entrada, y `IRBlockCache::Clear` olvida la tabla entera.
#pragma once

#include <cstdint>

namespace stvjit {

// Se reserva el maximo y se usan solo los bits configurados: asi el tamaño de
// la tabla se puede barrer con una propiedad, sin recompilar.
constexpr int kBitsMax = 12;                    // 4096 entradas = 32 KB
constexpr int kDestinosMax = 1 << kBitsMax;

struct EntradaDestino {
	uint32_t pc;        // etiqueta; 0xFFFFFFFF = vacia (ningun pc real esta desalineado)
	uint32_t palabra;   // la palabra emuhack tal cual: JITBASEREG ya trae el enmascarado
};

extern EntradaDestino g_destinos[kDestinosMax];

// Contadores del testigo. Solo se emiten si el modo es 2 (ver ModoCache).
extern uint64_t g_aciertos;
extern uint64_t g_fallos;

// 0 = apagada (despachador original), 1 = encendida, 2 = encendida con testigo.
// Se lee UNA vez, al generar el codigo fijo del JIT.
int ModoCache();
int BitsCache();
int MezclaCache();

// Con mezcla=0 el indice son los bits bajos del pc, asi que dos direcciones
// separadas por (4 << bits) bytes caen en la MISMA entrada. El codigo de un
// juego de PSP ocupa megabytes: con 2048 entradas eso es una colision cada
// 8 KB de codigo, y un lazo caliente repartido en unos cientos de KB se pisa a
// si mismo decenas de veces. Con mezcla=1 se le hace un XOR con los bits de
// arriba, que es una instruccion mas y reparte por todo el rango.
inline uint32_t IndiceDestino(uint32_t pc, int bits, int mezcla) {
	uint32_t i = pc >> 2;
	if (mezcla)
		i ^= pc >> (2 + bits);
	return i & ((1u << bits) - 1u);
}

// Un bloque dejo de ser valido en `pc`: su entrada no puede sobrevivirlo.
// Barre TODOS los anchos y las DOS formas de indice: el ancho y la mezcla vivos
// se fijan al generar el codigo, pero esta llamada puede llegar antes. Barrer de
// mas es gratis; barrer de menos deja una entrada apuntando a codigo liberado.
void OlvidarDestino(uint32_t pc);
void OlvidarTodos();
void VolcarTestigo(const char *motivo);

}  // namespace stvjit
