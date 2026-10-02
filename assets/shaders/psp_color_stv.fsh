/*
	psp_color, version STV (STV_PSPCOLOR_F16_v1, fork STV de PPSSPP)

	Misma correccion que psp_color.fsh (hunterk, Pokefan531; port de jdgleaver; dominio publico):
	pow 2.21, matriz 3x3 de la pantalla de la PSP 1000/2000, pow 1/2.2. Dos cambios de costo, no de
	resultado (maximo 1 nivel de 255 contra la formula, medido sobre escenas reales):
	  - c^2.21 se evalua con un polinomio (c^2 * cubica), sin pow: la entrada es RGBA8;
	  - con STV_F16 (Vulkan con shaderFloat16) la cuenta va en 16 bits.
	En el Mali-G57 de la TrimUI el pase final baja de 1,90 a 1,29 ms a 1280x720.
*/
#ifdef GL_ES
precision mediump float;
precision mediump int;
#endif

#ifdef STV_F16
#define H float16_t
#define H3 f16vec3
#define HM3 f16mat3
#else
#define H float
#define H3 vec3
#define HM3 mat3
#endif

uniform sampler2D sampler0;
varying vec2 v_texcoord0;

void main()
{
	H3 c = H3(texture2D(sampler0, v_texcoord0.xy).rgb);
	// c^2.21 ~= c^2 * (a0 + a1 c + a2 c^2 + a3 c^3), ajuste ponderado sobre los 256 valores de 8 bits
	H3 s = c * c * (H(0.507388) + c * (H(1.210878) + c * (H(-1.244209) + c * H(0.531055))));
	H3 y = HM3(H(0.98), H(0.04), H(0.01), H(0.20), H(0.795), H(0.01), H(-0.18), H(0.165), H(0.98)) * s;
	gl_FragColor = vec4(vec3(pow(max(y, H3(0.0)), H3(1.0 / 2.2))), 1.0);
}
