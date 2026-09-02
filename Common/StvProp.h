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
