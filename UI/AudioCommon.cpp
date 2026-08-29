// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/System/System.h"
#include "Core/Config.h"
#include "Core/HW/StereoResampler.h"  // TODO: doesn't belong in Core/HW...
#include "Core/HW/GranularMixer.h"
#include "UI/AudioCommon.h"
#include "UI/BackgroundAudio.h"
#include "Core/HW/Display.h"

StereoResampler g_resampler;
GranularMixer g_granular;

// numFrames is number of stereo frames.
// This is called from *outside* the emulator thread.
void NativeMix(int16_t *outStereo, int numFrames, int sampleRateHz, void *userdata) {
	// Mix UI sound effects on top.
	if (g_Config.iAudioPlaybackMode == (int)AudioSyncMode::GRANULAR) {
		// We use the FPS estimate, because to maintain smooth audio even though our
		// frame execution is very front (or back) heavy (as we can't count on "real time clock sync"
		// to be enabled), we need at least one whole frame buffered, plus a bit of extra.
		float fpsEstimate, vps, actualFps;
		__DisplayGetFPS(&vps, &fpsEstimate, &actualFps);

		g_granular.Mix(outStereo, numFrames, sampleRateHz, fpsEstimate);
	} else {
		g_resampler.Mix(outStereo, numFrames, false, sampleRateHz);
	}
	g_BackgroundAudio.SFX().Mix(outStereo, numFrames, sampleRateHz);
}

void System_AudioGetDebugStats(char *buf, size_t bufSize) {
	if (buf) {
		if (g_Config.iAudioPlaybackMode == (int)AudioSyncMode::GRANULAR) {
			// STV F10b (2026-08-28): EL INSTRUMENTO ESTABA TAPADO.
			// GranularMixer ya acumulaba underruns_/overruns_ y el estado de la
			// cola, pero el unico lector era la ventana ImGui del ImDebugger,
			// que en esta consola no se maneja con el pad: el overlay de texto
			// imprimia "(No stats available for granular yet)" y era imposible
			// saber, sin escribir codigo, si un artefacto venia de un hueco
			// real o del propio relleno por repeticion. Con esto, poner
			// DebugOverlay=5 en el ini alcanza para verlo en pantalla.
			// "bucle" es el dato que importa para el pico: cuando esta en 1, el
			// mixer esta repitiendo granulos porque el emulador no entrego
			// muestras a tiempo.
			GranularStats st;
			g_granular.GetStats(&st);
			// STV F10b (2026-08-29): NO se muestran queuedGranulesMin/Max: GetStats
			// los RESETEA en cada lectura (a 10000/0), asi que en pantalla se leian
			// esos defaults y no un dato. Queda lo estable y lo que decide:
			//  - under/over ACUMULADOS de la sesion (under = huecos que hubo que
			//    rellenar repitiendo; over = veces que el productor desbordo).
			//  - cola suavizada CONTRA el objetivo: si vive por debajo, el colchon
			//    nunca se llena y los cortes son inevitables.
			//  - bucle: 1 = ahora mismo esta repitiendo audio (el "robot").
			snprintf(buf, bufSize,
				"AUDIO  under %d   over %d\n"
				"cola %.1f / %d gran   %d ms\n"
				"bucle %d   fade %.2f   cuadro %.1f ms",
				st.underruns, st.overruns,
				st.smoothedQueuedGranules, st.targetQueueSize,
				(int)(st.smoothedQueuedGranules * (GranularMixer::GRANULE_SIZE * 1000.0 / 44100.0)),
				st.looping ? 1 : 0, st.fadeVolume, st.frameTimeEstimate * 1000.0f);
			} else {
			g_resampler.GetAudioDebugStats(buf, bufSize);
		}
	} else {
		g_resampler.ResetStatCounters();
	}
}

void System_AudioClear() {
	g_resampler.Clear();
	// STV F10b: el granular tambien tiene audio en vuelo. Se limpian los DOS
	// sin mirar el modo activo: limpiar el que no esta en uso no cuesta nada y
	// evita que un cambio de modo en caliente arrastre audio viejo.
	g_granular.Clear();
}

void System_AudioPushSamples(const int32_t *audio, int numSamples, float volume) {
	if (audio) {
		if (g_Config.iAudioPlaybackMode == (int)AudioSyncMode::GRANULAR) {
			g_granular.PushSamples(audio, numSamples, volume);
		} else {
			g_resampler.PushSamples(audio, numSamples, volume);
		}
	} else {
		g_resampler.Clear();
		g_granular.Clear();   // STV F10b: idem, ver System_AudioClear
	}
}
