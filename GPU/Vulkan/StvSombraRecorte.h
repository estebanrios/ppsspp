#pragma once
// STV_SOMBRA_RECORTE_v1 (arco GoS 1:1 STV, 2026-10-02)
//
// Ghost of Sparta proyecta la sombra de cada personaje sobre el piso con un draw que muestrea una
// casilla de 64x64 de un mapa de sombras (texgen por matriz de textura, proyectivo) y mezcla
// dst * (1 - texel): donde el mapa es negro el resultado es EXACTAMENTE dst. Cada proyeccion cubre
// la huella de la casilla entera (~media pantalla), pero la silueta ocupa el 15-30 % de la casilla:
// el resto es trabajo que no cambia ni un bit. Medido: las 4 proyecciones son el 11 % de la GPU.
//
// Este modulo sigue, POR CUADRO, que partes de un area de framebuffer pueden no ser negras:
//   - un rectangulo negro opaco (through, sin textura) crea/limpia un AREA;
//   - un draw 3D dentro del area ensucia la caja de sus vertices transformados en la CPU (+margen);
//   - una copia through desde un area hacia otra (el desenfoque del mapa) mapea las cajas sucias;
//   - cualquier otra escritura (draw no reconocido, copia de bloque, memset) invalida el area.
// En el draw de proyeccion recorta cada triangulo en espacio homogeneo contra los planos donde
// (u/q, v/q) cae en la caja sucia de la casilla (con q de los dos signos y el clamp de la textura),
// y usa la caja en pantalla resultante como scissor. Si no queda nada, el draw no se envia.
// Exacto por construccion: fuera de la caja el fragmento hubiera escrito dst sin cambios.
//
// Valvula debug.stv.sombra: 0 apagado, 1 encendido (defecto), 2 encendido + estadisticas en log.
// Respeta STV_AB_v1 (debug.stv.abgate=1: solo actua en la fase B).

#include <vector>
#include <algorithm>
#include <cmath>
#include "Common/StvProp.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VertexDecoderCommon.h"

namespace StvSombra {

struct Caja { float x1, y1, x2, y2; };   // [x1,x2) x [y1,y2): x en BYTES dentro de la fila, y en filas

// Un area es memoria en bytes: el mismo bloque se dibuja en 8888 (stride 512) y se lee en 565
// (stride 1024); lo que importa es que los bytes sean cero (negro en cualquier formato).
struct Area {
	u32 base;        // desplazamiento en VRAM (bytes) del origen del area
	int pitch;       // bytes por fila
	Caja rect;       // rectangulo conocido (cero + sucio), en bytes x filas
	bool valida;
	int nSil = 0, nVert = 0;   // presupuesto: un area que recibe la escena entera no es un mapa de sombras
	// v2: lo sucio es un mapa de celdas de cw bytes x ch filas (1 = puede no ser cero)
	int cw = 4, ch = 2, ncx = 0, ncy = 0;
	std::vector<uint8_t> bm;
	void Iniciar() {
		cw = 4; ch = 2;
		const float w = rect.x2 - rect.x1, h = rect.y2 - rect.y1;
		for (;;) {
			ncx = (int)std::ceil(w / cw); ncy = (int)std::ceil(h / ch);
			if ((long)ncx * ncy <= 65536) break;
			cw *= 2; ch *= 2;
		}
		bm.assign((size_t)std::max(ncx, 0) * std::max(ncy, 0), 0);
	}
	bool HaySucio() const { for (uint8_t v : bm) if (v) return true; return false; }
	void Marcar(Caja c) {
		const int x1 = std::max(0, (int)std::floor((c.x1 - rect.x1) / cw)), x2 = std::min(ncx, (int)std::ceil((c.x2 - rect.x1) / cw));
		const int y1 = std::max(0, (int)std::floor((c.y1 - rect.y1) / ch)), y2 = std::min(ncy, (int)std::ceil((c.y2 - rect.y1) / ch));
		for (int y = y1; y < y2; y++) for (int x = x1; x < x2; x++) bm[(size_t)y * ncx + x] = 1;
	}
	void LimpiarDentro(Caja c) {   // celdas ENTERAS dentro de c -> cero
		const int x1 = std::max(0, (int)std::ceil((c.x1 - rect.x1) / cw)), x2 = std::min(ncx, (int)std::floor((c.x2 - rect.x1) / cw));
		const int y1 = std::max(0, (int)std::ceil((c.y1 - rect.y1) / ch)), y2 = std::min(ncy, (int)std::floor((c.y2 - rect.y1) / ch));
		for (int y = y1; y < y2; y++) for (int x = x1; x < x2; x++) bm[(size_t)y * ncx + x] = 0;
	}
	// tiras horizontales de celdas sucias dentro de r (cajas en bytes x filas)
	void Tiras(const Caja &r, std::vector<Caja> *out) const {
		out->clear();
		const int x1 = std::max(0, (int)std::floor((r.x1 - rect.x1) / cw)), x2 = std::min(ncx, (int)std::ceil((r.x2 - rect.x1) / cw));
		const int y1 = std::max(0, (int)std::floor((r.y1 - rect.y1) / ch)), y2 = std::min(ncy, (int)std::ceil((r.y2 - rect.y1) / ch));
		for (int y = y1; y < y2; y++) {
			int x = x1;
			while (x < x2) {
				while (x < x2 && !bm[(size_t)y * ncx + x]) x++;
				if (x >= x2) break;
				int e = x;
				while (e < x2 && bm[(size_t)y * ncx + e]) e++;
				const Caja c = { rect.x1 + x * cw, rect.y1 + y * ch, rect.x1 + e * cw, rect.y1 + (y + 1) * ch };
				// juntar con la tira de la fila anterior si tiene las mismas columnas
				bool unida = false;
				for (Caja &k : *out) if (k.x1 == c.x1 && k.x2 == c.x2 && k.y2 == c.y1) { k.y2 = c.y2; unida = true; break; }
				if (!unida) out->push_back(c);
				x = e;
			}
		}
	}
};

inline std::vector<Area> &Areas() { static std::vector<Area> a; return a; }
inline int &Modo() { static int m = 1; return m; }

struct Stats { int proyecciones = 0, recortadas = 0, salteadas = 0, invalidadas = 0, negros = 0, copias = 0, siluetas = 0, vramCambio = 0, cand = 0, rech[16] = {}; double areaTotal = 0, areaRecorte = 0, areaRecorte2 = 0, aTri = 0, aPoli = 0, aCajaTri = 0; };
inline Stats &St() { static Stats s; return s; }

inline u32 OffVram(u32 addr) { return addr & 0x001FFFFF; }
inline bool EsVram(u32 addr) { return (addr & 0x0F000000) == 0x04000000; }
inline int Bpp(GEBufferFormat f) { return f == GE_FORMAT_8888 ? 4 : 2; }

inline void InicioCuadro() {
	Modo() = StvPropDef("debug.stv.sombra", 1);
	Areas().clear();
	StvVramRangos().clear();
	if (Modo() >= 2) {
		static int n = 0;
		Stats &s = St();
		if (++n >= 120) {
			n = 0;
			STV_LOG("STVSOMBRA: proyecciones=%d recortadas=%d salteadas=%d invalidadas=%d area %.0f%% del original | negros=%d copias=%d siluetas=%d vramCambio=%d cand=%d rech=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
				s.proyecciones, s.recortadas, s.salteadas, s.invalidadas, s.areaTotal > 0 ? 100.0 * s.areaRecorte / s.areaTotal : 0.0,
				s.negros, s.copias, s.siluetas, s.vramCambio, s.cand, s.rech[0], s.rech[1], s.rech[2], s.rech[3], s.rech[4], s.rech[5], s.rech[6], s.rech[7], s.rech[8], s.rech[9], s.rech[10], s.rech[11]);
			STV_LOG("STVSOMBRA areas en pantalla (px PSP por cuadro): triangulos %.0f, poligonos recortados %.0f, cajas por triangulo %.0f | scissor total %.0f, por grupos %.0f (px render)",
				s.aTri / 120, s.aPoli / 120, s.aCajaTri / 120, s.areaRecorte / 120, s.areaRecorte2 / 120);
			s = Stats();
		}
	}
}

inline bool Activo() { return Modo() >= 1 && StvAbActivo(); }

inline long A0(const Area &a) { return a.base + (long)a.rect.y1 * a.pitch + (long)a.rect.x1; }
inline long A1(const Area &a) { return a.base + (long)a.rect.y2 * a.pitch; }

inline void Invalidar(Area &a) { if (a.valida) St().invalidadas++; a.valida = false; }

// Procesa las escrituras de VRAM fuera de draws: invalida solo las areas cuyos bytes tocan.
inline bool VramIntacta() {
	auto &v = StvVramRangos();
	if (v.empty()) return true;
	for (const auto &r : v) {
		St().vramCambio++;
		if (!EsVram(r.first)) continue;
		const long w0 = OffVram(r.first), w1 = w0 + (long)r.second;
		for (Area &a : Areas())
			if (a.valida && w0 < A1(a) && A0(a) < w1) Invalidar(a);
	}
	v.clear();
	return true;
}

// Origen (x en bytes, y en filas) de una direccion dentro de un area con el mismo pitch.
inline bool PosEnArea(const Area &a, u32 addr, int pitch, int *x, int *y) {
	if (!EsVram(addr) || pitch != a.pitch || pitch <= 0) return false;
	long d = (long)OffVram(addr) - (long)a.base;
	if (d < 0) return false;
	*x = (int)(d % pitch); *y = (int)(d / pitch);
	return true;
}

// El area valida cuyo rectangulo CONTIENE el origen de la direccion (mismo pitch).
inline Area *AreaDe(u32 addr, int pitch, int *x, int *y) {
	for (Area &a : Areas()) {
		int px, py;
		if (a.valida && PosEnArea(a, addr, pitch, &px, &py) && px >= a.rect.x1 && px < a.rect.x2 && py >= a.rect.y1 && py < a.rect.y2) {
			*x = px; *y = py; return &a;
		}
	}
	return nullptr;
}

inline bool Dentro(const Caja &c, const Caja &r) { return c.x1 >= r.x1 && c.y1 >= r.y1 && c.x2 <= r.x2 && c.y2 <= r.y2; }
inline bool Toca(const Caja &a, const Caja &b) { return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2; }

inline void AgregarSucio(Area &a, Caja c) { a.Marcar(c); }

// Cualquier escritura no reconocida que pueda tocar los bytes de un area la invalida.
// filas = alto del render target virtual (si no se sabe, 512).
inline void Escritura(u32 fbAddr, int pitch, int filas = 512) {
	if (!EsVram(fbAddr) || Areas().empty()) return;
	const long r0 = OffVram(fbAddr), r1 = r0 + (long)pitch * std::max(filas, 1);
	for (Area &a : Areas())
		if (a.valida && r0 < A1(a) && A0(a) < r1) Invalidar(a);
}

// Rectangulo de bytes CERO escrito en el render target (caja en PIXELES del destino).
inline void Negro(u32 fbAddr, int stride, GEBufferFormat fmt, Caja c) {
	if (!EsVram(fbAddr)) return;
	const int bpp = Bpp(fmt), pitch = stride * bpp;
	const Caja cb = { c.x1 * bpp, c.y1, c.x2 * bpp, c.y2 };
	int x, y;
	Area *a = AreaDe(fbAddr, pitch, &x, &y);
	if (a) {
		const Caja cc = { cb.x1 + x, cb.y1 + y, cb.x2 + x, cb.y2 + y };
		if (Dentro(cc, a->rect)) {
			a->LimpiarDentro(cc);
			return;
		}
	}
	Area n;
	n.base = OffVram(fbAddr); n.pitch = pitch; n.rect = cb; n.valida = true;
	n.Iniciar();
	const long n0 = A0(n), n1 = A1(n);
	for (Area &o : Areas())
		if (o.valida && n0 < A1(o) && A0(o) < n1) Invalidar(o);   // un area vieja que se pisa en parte: fuera
	St().negros++;
	Areas().push_back(n);
}

// Draw 3D cuyo destino cae en un area: ensucia la caja (en PIXELES del destino) + 1 px de margen.
inline void Sucio3D(u32 fbAddr, int stride, GEBufferFormat fmt, Caja c) {
	const int bpp = Bpp(fmt);
	int x, y;
	Area *a = AreaDe(fbAddr, stride * bpp, &x, &y);
	if (!a) { Escritura(fbAddr, stride * bpp); return; }
	St().siluetas++;
	AgregarSucio(*a, Caja{ (std::floor(c.x1) - 1) * bpp + x, std::floor(c.y1) - 1 + y, (std::ceil(c.x2) + 1) * bpp + x, std::ceil(c.y2) + 1 + y });
}

// Copia through (sin blend) de una caja de TEXELS de la fuente a un rectangulo de PIXELES del destino.
// clampEntera: src es la textura entera y esta en clamp (el filtro no lee fuera de src).
inline void Copia(u32 srcAddr, int srcStride, int srcBpp, Caja src, bool clampEntera, u32 dstAddr, int dstStride, int dstBpp, Caja dst) {
	int sx, sy, dx, dy;
	Area *as = AreaDe(srcAddr, srcStride * srcBpp, &sx, &sy);
	Area *ad = AreaDe(dstAddr, dstStride * dstBpp, &dx, &dy);
	if (!ad) { Escritura(dstAddr, dstStride * dstBpp); return; }
	const Caja s = { src.x1 * srcBpp + sx, src.y1 + sy, src.x2 * srcBpp + sx, src.y2 + sy };
	const Caja d = { dst.x1 * dstBpp + dx, dst.y1 + dy, dst.x2 * dstBpp + dx, dst.y2 + dy };
	const Caja s1 = clampEntera ? s : Caja{ s.x1 - srcBpp, s.y1 - 1, s.x2 + srcBpp, s.y2 + 1 };
	if (!as || s.x2 <= s.x1 || s.y2 <= s.y1 || !Dentro(s1, as->rect) || !Dentro(d, ad->rect)) { Invalidar(*ad); return; }
	St().copias++;
	const float kx = (d.x2 - d.x1) / (s.x2 - s.x1), ky = (d.y2 - d.y1) / (s.y2 - s.y1);   // bytes destino por byte fuente
	const float kxp = kx * srcBpp / dstBpp;                                                    // pixeles destino por texel
	// bilineal: un texel sucio influye en las muestras a menos de 1 texel -> kxp px de destino, +1 px de redondeo
	const float mx = (kxp + 1.0f) * dstBpp, my = ky + 1.0f;
	static std::vector<Caja> tiras;
	as->Tiras(s, &tiras);
	std::vector<Caja> nuevo;
	for (const Caja &k : tiras) {
		const Caja i = { std::max(k.x1, s.x1), std::max(k.y1, s.y1), std::min(k.x2, s.x2), std::min(k.y2, s.y2) };
		if (i.x1 >= i.x2 || i.y1 >= i.y2) continue;
		const Caja m = { d.x1 + (i.x1 - s.x1) * kx, d.y1 + (i.y1 - s.y1) * ky, d.x1 + (i.x2 - s.x1) * kx, d.y1 + (i.y2 - s.y1) * ky };
		nuevo.push_back(Caja{ std::floor(m.x1 - mx), std::floor(m.y1 - my), std::ceil(m.x2 + mx), std::ceil(m.y2 + my) });
	}
	// el destino de la copia queda reemplazado (sin blend, cubre su rectangulo)
	ad->LimpiarDentro(d);
	for (const Caja &c : nuevo) {
		const Caja r = { std::max(c.x1, d.x1), std::max(c.y1, d.y1), std::min(c.x2, d.x2), std::min(c.y2, d.y2) };
		if (r.x1 < r.x2 && r.y1 < r.y2) ad->Marcar(r);
	}
}

// ---------------------------------------------------------------------------------------------
// Recorte de la proyeccion.
struct V7 { float X, Y, Z, W, u, v, q; };

inline V7 Lerp(const V7 &a, const V7 &b, float t) {
	return V7{ a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t,
		a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.q + (b.q - a.q) * t };
}

// Recorta el poligono contra f(v) >= 0, con f lineal en los atributos.
template <typename F>
inline void Recortar(std::vector<V7> &p, F f) {
	if (p.empty()) return;
	std::vector<V7> o;
	o.reserve(p.size() + 4);
	for (size_t i = 0; i < p.size(); i++) {
		const V7 &a = p[i], &b = p[(i + 1) % p.size()];
		float fa = f(a), fb = f(b);
		if (fa >= 0) o.push_back(a);
		if ((fa >= 0) != (fb >= 0)) o.push_back(Lerp(a, b, fa / (fa - fb)));
	}
	p.swap(o);
}

// Caja en pantalla (pixeles del render target PSP, ya sin offset) de los fragmentos cuyo (u/q,v/q) cae
// en [u0,u1] x [v0,v1] (infinitos permitidos). Devuelve false si no queda nada.
inline double AreaPoli(const std::vector<V7> &p) {
	double a = 0;
	for (size_t i = 0; i < p.size(); i++) {
		const V7 &v = p[i], &w = p[(i + 1) % p.size()];
		a += (double)(v.X / v.W) * (w.Y / w.W) - (double)(w.X / w.W) * (v.Y / v.W);
	}
	return std::fabs(a) * 0.5;
}

struct SubDraw { int primero, cuenta; Caja c; };   // triangulos [primero, primero+cuenta) (en indices) y su caja en px PSP

inline bool CajaPantalla(const std::vector<V7> &verts, const u16 *ind, int nInd, float u0, float u1, float v0, float v1, Caja *out, std::vector<SubDraw> *subs) {
	if (subs) subs->clear();
	Caja c = { 1e9f, 1e9f, -1e9f, -1e9f };
	bool hay = false;
	std::vector<V7> p;
	for (int t = 0; t + 2 < nInd; t += 3) {
		if (Modo() >= 2) {
			p.assign({ verts[ind[t]], verts[ind[t + 1]], verts[ind[t + 2]] });
			Recortar(p, [](const V7 &v) { return v.W - 1e-4f; });
			// recortar a la pantalla PSP (0..480 x 0..272) para medir lo que de verdad se rasteriza
			Recortar(p, [](const V7 &v) { return v.X; });
			Recortar(p, [](const V7 &v) { return 480.0f * v.W - v.X; });
			Recortar(p, [](const V7 &v) { return v.Y; });
			Recortar(p, [](const V7 &v) { return 272.0f * v.W - v.Y; });
			St().aTri += AreaPoli(p);
		}
		Caja ct = { 1e9f, 1e9f, -1e9f, -1e9f };
		for (int s = 0; s < 2; s++) {
			const float sg = s == 0 ? 1.0f : -1.0f;   // q > 0 o q < 0
			p.assign({ verts[ind[t]], verts[ind[t + 1]], verts[ind[t + 2]] });
			Recortar(p, [](const V7 &v) { return v.W - 1e-4f; });
			Recortar(p, [&](const V7 &v) { return sg * v.q; });
			if (std::isfinite(u0)) Recortar(p, [&](const V7 &v) { return sg * (v.u - u0 * v.q); });
			if (std::isfinite(u1)) Recortar(p, [&](const V7 &v) { return sg * (u1 * v.q - v.u); });
			if (std::isfinite(v0)) Recortar(p, [&](const V7 &v) { return sg * (v.v - v0 * v.q); });
			if (std::isfinite(v1)) Recortar(p, [&](const V7 &v) { return sg * (v1 * v.q - v.v); });
			if (Modo() >= 2) {
				std::vector<V7> pp = p;
				Recortar(pp, [](const V7 &v) { return v.X; });
				Recortar(pp, [](const V7 &v) { return 480.0f * v.W - v.X; });
				Recortar(pp, [](const V7 &v) { return v.Y; });
				Recortar(pp, [](const V7 &v) { return 272.0f * v.W - v.Y; });
				St().aPoli += AreaPoli(pp);
			}
			for (const V7 &v : p) {
				const float x = v.X / v.W, y = v.Y / v.W;
				c.x1 = std::min(c.x1, x); c.y1 = std::min(c.y1, y); c.x2 = std::max(c.x2, x); c.y2 = std::max(c.y2, y);
				ct.x1 = std::min(ct.x1, x); ct.y1 = std::min(ct.y1, y); ct.x2 = std::max(ct.x2, x); ct.y2 = std::max(ct.y2, y);
				hay = true;
			}
		}
		if (subs && ct.x1 < ct.x2) {
			// agrupar con el anterior si es contiguo y la caja unida no crece mas de 15 % sobre la suma
			const float at = (ct.x2 - ct.x1) * (ct.y2 - ct.y1);
			if (!subs->empty() && subs->back().primero + subs->back().cuenta == t) {
				SubDraw &u = subs->back();
				const Caja un = { std::min(u.c.x1, ct.x1), std::min(u.c.y1, ct.y1), std::max(u.c.x2, ct.x2), std::max(u.c.y2, ct.y2) };
				const float au = (u.c.x2 - u.c.x1) * (u.c.y2 - u.c.y1);
				if ((un.x2 - un.x1) * (un.y2 - un.y1) <= 1.15f * (au + at)) { u.c = un; u.cuenta += 3; goto agrupado; }
			}
			subs->push_back(SubDraw{ t, 3, ct });
		agrupado:;
		}
		if (Modo() >= 2 && ct.x1 < ct.x2) {
			// area del triangulo original dentro de su caja recortada (lo que rasterizaria con scissor por triangulo)
			const float x1 = std::max(ct.x1, 0.0f), y1 = std::max(ct.y1, 0.0f), x2 = std::min(ct.x2, 480.0f), y2 = std::min(ct.y2, 272.0f);
			if (x2 > x1 && y2 > y1) {
				std::vector<V7> q = { verts[ind[t]], verts[ind[t + 1]], verts[ind[t + 2]] };
				Recortar(q, [](const V7 &v) { return v.W - 1e-4f; });
				Recortar(q, [&](const V7 &v) { return v.X - x1 * v.W; });
				Recortar(q, [&](const V7 &v) { return x2 * v.W - v.X; });
				Recortar(q, [&](const V7 &v) { return v.Y - y1 * v.W; });
				Recortar(q, [&](const V7 &v) { return y2 * v.W - v.Y; });
				St().aCajaTri += AreaPoli(q);
			}
		}
	}
	*out = c;
	return hay;
}

}  // namespace StvSombra
