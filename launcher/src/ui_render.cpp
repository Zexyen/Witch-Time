#include "ui_render.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <vector>
#include <SDL3/SDL.h>
#include <RmlUi/Core/Log.h>
#include "../../runtime/prompt_art.h"

// Pictures by name, with parameters after '?' (colors as rrggbb, alpha 0-255):
//   gen:glow?x=&y=&rx=&ry=&c1=&c0=&stop=   soft elliptic glow c1 at (x, y) fading to c0 at stop (the screen backgrounds)
//   gen:ring?c=&a=&w=                      a circle line w texels thick in a 512 texel square (dial rings, progress)
//   gen:ticks?n=&c=&a=&w=&l=               n marks w texels wide and l long around the edge (the dial's minutes)
//   gen:moon?c=                            a crescent (the dial's centre)
//   gen:fade?c=&a=                         c fading out from left to right (the selected menu row's line)
//   gen:pill?w=&h=&f=&c=&a=&l=             a w x h texel rounded bar filled with f (if given), its edge c l texels
//                                          thick (if given): the switches; w = h is a disc. RmlUi's border-radius is jagged
//   gen:glyph?n=&c=&t=                     a button prompt, as the game shows it: n = a b cross circle enter esc key bumper dpad arrows (t: the letters)
//   gen:grad?d=&c0=&a0=&c1=&a1=&c2=&a2=&c3=&a3=&p1=&p2=
//                                          a gradient with up to four stops (c0 at 0, c3 at 1, c1 and c2 at p1 and p2),
//                                          left to right (d=h, the default) or top to bottom (d=v): the panel, the bars
// A file source ending in .bmp (ui/img, made by the design's scripts) is an 8-bit BMP whose palette carries alpha in its
// fourth byte, colours premultiplied: the wheel, the hands, the runic stripe and the logo. ?a= scales its alpha.
namespace {

struct Params {
	std::map<Rml::String, Rml::String> v;
	float num(const char* k, float def) const
	{
		auto it = v.find(k);
		return it == v.end() ? def : std::strtof(it->second.c_str(), nullptr);
	}
	Rml::Colourb color(const char* k, Rml::Colourb def) const
	{
		auto it = v.find(k);
		if (it == v.end() || it->second.size() != 6)
			return def;
		unsigned long x = std::strtoul(it->second.c_str(), nullptr, 16);
		return Rml::Colourb(Rml::byte(x >> 16), Rml::byte(x >> 8), Rml::byte(x), 255);
	}
};

Params parse(const Rml::String& query)
{
	Params p;
	size_t i = 0;
	while (i < query.size())
	{
		size_t amp = query.find('&', i);
		if (amp == Rml::String::npos)
			amp = query.size();
		Rml::String kv = query.substr(i, amp - i);
		size_t eq = kv.find('=');
		if (eq != Rml::String::npos)
			p.v[kv.substr(0, eq)] = kv.substr(eq + 1);
		i = amp + 1;
	}
	return p;
}

struct Image {
	int w, h;
	std::vector<Rml::byte> px;
	Image(int w, int h) : w(w), h(h), px(size_t(w) * h * 4, 0) {}
	// premultiplied, as the renderer wants it
	void put(int x, int y, Rml::Colourb c, float a)
	{
		a = std::clamp(a, 0.f, 1.f) * (c.alpha / 255.f);
		Rml::byte* p = &px[(size_t(y) * w + x) * 4];
		p[0] = Rml::byte(c.red * a + .5f);
		p[1] = Rml::byte(c.green * a + .5f);
		p[2] = Rml::byte(c.blue * a + .5f);
		p[3] = Rml::byte(255 * a + .5f);
	}
};

float coverage(float d) { return std::clamp(.5f - d, 0.f, 1.f); }   // signed distance in texels -> antialiased coverage

Image glow(const Params& p)
{
	Image im(320, 200);
	const float cx = p.num("x", .5f), cy = p.num("y", .5f), rx = p.num("rx", .7f), ry = p.num("ry", .9f), stop = p.num("stop", .62f);
	const Rml::Colourb c1 = p.color("c1", {36, 17, 46, 255}), c0 = p.color("c0", {10, 6, 16, 255});
	const float a1 = p.num("a1", 255), a0 = p.num("a0", 255), edge = p.num("edge", 0);
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float dx = ((x + .5f) / im.w - cx) / rx, dy = ((y + .5f) / im.h - cy) / ry;
			float t = std::clamp(std::sqrt(dx * dx + dy * dy) / stop, 0.f, 1.f);
			t = t * t * (3 - 2 * t);
			Rml::Colourb c(Rml::byte(c1.red + (c0.red - c1.red) * t), Rml::byte(c1.green + (c0.green - c1.green) * t),
				Rml::byte(c1.blue + (c0.blue - c1.blue) * t), 255);
			float a = (a1 + (a0 - a1) * t) / 255.f;
			if (edge > 0)                                       // fades to nothing at the four edges: no box around a halo
			{
				const float u = (x + .5f) / im.w, v = (y + .5f) / im.h;
				const float e = std::clamp(std::min({u, 1 - u, v, 1 - v}) / edge, 0.f, 1.f);
				a *= e * e * (3 - 2 * e);
			}
			im.put(x, y, c, a);
		}
	return im;
}

Image ring(const Params& p)
{
	Image im(512, 512);
	Rml::Colourb c = p.color("c", {201, 163, 90, 255});
	c.alpha = Rml::byte(p.num("a", 255));
	const float w = p.num("w", 2), r = 256 - 1 - w / 2;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float d = std::fabs(std::hypot(x + .5f - 256, y + .5f - 256) - r) - w / 2;
			if (d < 1)
				im.put(x, y, c, coverage(d));
		}
	return im;
}

Image ticks(const Params& p)
{
	Image im(512, 512);
	Rml::Colourb c = p.color("c", {201, 163, 90, 255});
	c.alpha = Rml::byte(p.num("a", 255));
	const int n = std::max(1, int(p.num("n", 60)));
	const float w = p.num("w", 3), len = p.num("l", 14), r1 = 255 - len, r2 = 255;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float dx = x + .5f - 256, dy = y + .5f - 256, r = std::hypot(dx, dy);
			if (r < r1 - 1 || r > r2 + 1)
				continue;
			float step = 2 * float(M_PI) / n, ang = std::atan2(dy, dx);
			float k = std::round(ang / step) * step;                      // nearest mark
			float across = std::fabs(-dx * std::sin(k) + dy * std::cos(k)) - w / 2;
			float along = std::max(r1 - r, r - r2);
			im.put(x, y, c, coverage(std::max(across, along)));
		}
	return im;
}

Image moon(const Params& p)
{
	Image im(256, 256);
	const Rml::Colourb c = p.color("c", {232, 207, 148, 255});
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float outer = std::hypot(x + .5f - 128, y + .5f - 128) - 120;
			float inner = std::hypot(x + .5f - 170, y + .5f - 104) - 104;  // the bite out of it
			im.put(x, y, c, coverage(std::max(outer, -inner)));
		}
	return im;
}

Image fade(const Params& p)
{
	Image im(256, 2);
	Rml::Colourb c = p.color("c", {224, 51, 138, 255});
	c.alpha = Rml::byte(p.num("a", 255));
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
			im.put(x, y, c, 1 - (x + .5f) / im.w);
	return im;
}

Image pill(const Params& p)
{
	Image im(std::max(1, int(p.num("w", 116))), std::max(1, int(p.num("h", 60))));
	const bool filled = p.v.count("f"), edged = p.v.count("c");
	const Rml::Colourb fc = p.color("f", {}), ec = p.color("c", {});
	const float r = im.h / 2.f, line = p.num("l", 2), ea = p.num("a", 255) / 255.f;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			float px = std::clamp(x + .5f, r, im.w - r);   // nearest point on the bar's spine
			float d = std::hypot(x + .5f - px, y + .5f - r) - r;
			if (filled)
				im.put(x, y, fc, coverage(d));
			if (edged)
			{
				float a = coverage(std::max(d, -d - line)) * ea;
				Rml::byte* q = &im.px[(size_t(y) * im.w + x) * 4];   // the edge over the fill, premultiplied
				for (int k = 0; k < 3; k++)
					q[k] = Rml::byte(q[k] * (1 - a) + (&ec.red)[k] * a + .5f);
				q[3] = Rml::byte(q[3] * (1 - a) + 255 * a + .5f);
			}
		}
	return im;
}

Image glyph(const Params& p)
{
	std::vector<uint8_t> s(S * S);
	const auto n = p.v.count("n") ? p.v.at("n") : Rml::String();
	const auto t = p.v.count("t") ? p.v.at("t") : Rml::String();
	if (n == "cross")
		ps_cross(s.data());
	else if (n == "circle")
		ps_circle(s.data());
	else if (n == "enter")
		keycap(s.data(), "ENT");
	else if (n == "esc")
		keycap(s.data(), "ESC");
	else if (n == "key")                                    // a keycap with the letters t
		keycap(s.data(), t.substr(0, 4).c_str());
	else if (n == "bumper")                                 // a shoulder button with the letters t: L, L1, LB
		bumper(s.data(), t.substr(0, 4).c_str());
	else if (n == "dpad")                                   // the d-pad, left and right
	{
		box(s.data(), 16, 7, 24, 33, 2, 1);
		box(s.data(), 7, 16, 33, 24, 2, 1);
		box(s.data(), 17.5f, 8.5f, 22.5f, 14, 1, 0);
		box(s.data(), 17.5f, 26, 22.5f, 31.5f, 1, 0);
	}
	else if (n == "arrows")                                 // the left and right arrow keys
	{
		box(s.data(), 3, 11.5f, 19, 28.5f, 3, 1);
		box(s.data(), 21, 11.5f, 37, 28.5f, 3, 1);
		seg(s.data(), 13, 16, 9, 20, 2, 0); seg(s.data(), 9, 20, 13, 24, 2, 0);
		seg(s.data(), 27, 16, 31, 20, 2, 0); seg(s.data(), 31, 20, 27, 24, 2, 0);
	}
	else
		letter_disc(s.data(), n == "b" ? "B" : "A");
	// the middle of the cell, two subsamples to a texel
	enum { from = 6 * SS, side = 28 * SS / 2 };
	Image im(side, side);
	const Rml::Colourb c = p.color("c", {239, 230, 216, 255});
	for (int y = 0; y < side; y++)
		for (int x = 0; x < side; x++)
		{
			const uint8_t* q = &s[(from + y * 2) * S + from + x * 2];
			im.put(x, y, c, (q[0] + q[1] + q[S] + q[S + 1]) / 4.f);
		}
	return im;
}

Image grad(const Params& p)
{
	const bool v = p.v.count("d") && p.v.at("d") == "v";
	Image im(v ? 2 : 256, v ? 256 : 2);
	struct Stop { float at; Rml::Colourb c; float a; };
	std::vector<Stop> stops;
	const char* keys[] = {"c0", "c1", "c2", "c3"};
	const char* alphas[] = {"a0", "a1", "a2", "a3"};
	const char* ats[] = {nullptr, "p1", "p2", nullptr};
	for (int i = 0; i < 4; i++)
		if (p.v.count(keys[i]))
			stops.push_back({i == 0 ? 0.f : i == 3 ? 1.f : p.num(ats[i], i / 3.f), p.color(keys[i], {}), p.num(alphas[i], 255)});
	if (stops.size() == 1)
		stops.push_back(stops[0]);
	if (stops.empty())
		return im;
	stops.back().at = 1;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
		{
			const float t = (v ? y + .5f : x + .5f) / (v ? im.h : im.w);
			size_t k = 0;
			while (k + 2 < stops.size() && t > stops[k + 1].at)
				k++;
			const Stop &s0 = stops[k], &s1 = stops[k + 1];
			const float u = s1.at > s0.at ? std::clamp((t - s0.at) / (s1.at - s0.at), 0.f, 1.f) : 1;
			Rml::Colourb c(Rml::byte(s0.c.red + (s1.c.red - s0.c.red) * u), Rml::byte(s0.c.green + (s1.c.green - s0.c.green) * u),
				Rml::byte(s0.c.blue + (s1.c.blue - s0.c.blue) * u), 255);
			im.put(x, y, c, (s0.a + (s1.a - s0.a) * u) / 255.f);
		}
	return im;
}

// an 8-bit BMP with alpha in the palette's reserved byte, premultiplied (see the top of this file); empty when it is not one
Image bmp(const Rml::String& path, const Params& p)
{
	size_t size = 0;
	void* bytes = SDL_LoadFile(path.c_str(), &size);
	Image im(1, 1);
	if (!bytes)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "witch-time: cannot read %s", path.c_str());
		return im;
	}
	const uint8_t* b = static_cast<const uint8_t*>(bytes);
	auto u32 = [b](size_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; };
	if (size >= 54 + 1024 && b[0] == 'B' && b[1] == 'M' && u32(14) >= 40 && (u32(28) & 0xffff) == 8)
	{
		const int w = int(u32(18)), h = int(u32(22));
		const size_t pal = 14 + u32(14), px = u32(10), stride = (size_t(w) + 3) & ~size_t(3);
		if (w > 0 && h > 0 && px + stride * size_t(h) <= size)
		{
			im = Image(w, h);
			const float scale = p.num("a", 255) / 255.f;
			for (int y = 0; y < h; y++)
			{
				const uint8_t* row = b + px + stride * size_t(h - 1 - y);
				for (int x = 0; x < w; x++)
				{
					const uint8_t* e = b + pal + size_t(row[x]) * 4;   // B G R A, premultiplied
					Rml::byte* q = &im.px[(size_t(y) * w + x) * 4];
					q[0] = Rml::byte(e[2] * scale + .5f);
					q[1] = Rml::byte(e[1] * scale + .5f);
					q[2] = Rml::byte(e[0] * scale + .5f);
					q[3] = Rml::byte(e[3] * scale + .5f);
				}
			}
		}
	}
	SDL_free(bytes);
	return im;
}

} // namespace

Rml::TextureHandle UiRender::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
	const size_t at = source.find("gen:");
	if (at == Rml::String::npos)
	{
		const size_t q = source.find('?');
		const Rml::String path = source.substr(0, q);
		if (path.size() < 4 || path.compare(path.size() - 4, 4, ".bmp") != 0)
			return RenderInterface_SDL_GPU::LoadTexture(texture_dimensions, source);
		Image im = bmp(path, parse(q == Rml::String::npos ? Rml::String() : source.substr(q + 1)));
		texture_dimensions = {im.w, im.h};
		return GenerateTexture({im.px.data(), im.px.size()}, texture_dimensions);
	}
	Rml::String spec = source.substr(at + 4), name = spec, query;
	if (size_t q = spec.find('?'); q != Rml::String::npos)
	{
		name = spec.substr(0, q);
		query = spec.substr(q + 1);
	}
	const Params p = parse(query);
	Image im = name == "glow" ? glow(p) : name == "ring" ? ring(p) : name == "ticks" ? ticks(p) : name == "moon" ? moon(p) : name == "pill" ? pill(p) : name == "glyph" ? glyph(p) : name == "grad" ? grad(p) : fade(p);
	texture_dimensions = {im.w, im.h};
	return GenerateTexture({im.px.data(), im.px.size()}, texture_dimensions);
}
