#include "Screen.hpp"
#include "ui.hpp"
#include <algorithm>

void Screen::addReadout(app::ParamWidget* control, Edge edge) {
	RoundKnob* knob = dynamic_cast<RoundKnob*>(control);
	readouts.push_back({control, edge, knob ? svgFill(knob->bg->svg, "cap", textColor) : textColor});
}

bool Screen::has(Edge edge) const {
	return std::any_of(readouts.begin(), readouts.end(), [=](const Readout& r) { return r.edge == edge; });
}

math::Rect Screen::graphicsBox() const {
	math::Rect r = box.zeroPos().shrink(math::Vec(padding, padding));
	if (has(BOTTOM))
		r.size.y -= rowHeight;
	if (has(RIGHT))
		r.size.x -= columnWidth;
	return r;
}

std::shared_ptr<window::Font> Screen::font() const {
	// Rack caches loaded fonts per window, so loading in every frame is cheap; holding on to one is not safe.
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

void Screen::drawReadouts(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	float w = box.size.x, h = box.size.y;

	nvgBeginPath(vg);
	if (has(BOTTOM)) {
		nvgMoveTo(vg, padding, h - rowHeight);
		nvgLineTo(vg, w - padding, h - rowHeight);
	}
	if (has(RIGHT)) {
		nvgMoveTo(vg, w - columnWidth, padding);
		nvgLineTo(vg, w - columnWidth, h - padding - (has(BOTTOM) ? rowHeight : 0.f));
	}
	nvgStrokeColor(vg, dividerColor);
	nvgStrokeWidth(vg, 1.f);
	nvgStroke(vg);

	std::shared_ptr<window::Font> f = font();
	if (!f)
		return;
	nvgFontFaceId(vg, f->handle);
	nvgFontSize(vg, fontSize);
	for (const Readout& r : readouts) {
		// No quantity means no module: the module browser's preview. Leave the readout blank.
		ParamQuantity* q = r.control->getParamQuantity();
		if (!q)
			continue;
		std::string text = q->getDisplayValueString() + q->getUnit();
		// Both widgets are children of the ModuleWidget, so their boxes share a coordinate system.
		math::Vec c = r.control->box.getCenter().minus(box.pos);
		nvgFillColor(vg, r.color);
		if (r.edge == BOTTOM) {
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgText(vg, c.x, h - rowHeight / 2, text.c_str(), NULL);
		}
		else {
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgText(vg, w - padding, c.y, text.c_str(), NULL);
		}
	}
}

void Screen::drawLayer(const DrawArgs& args, int layer) {
	if (layer == 1) {
		math::Rect g = graphicsBox();
		nvgSave(args.vg);
		nvgIntersectScissor(args.vg, RECT_ARGS(g.grow(math::Vec(padding, padding) / 2)));
		drawGraphics(args, g);
		nvgRestore(args.vg);
		drawReadouts(args);
	}
	Widget::drawLayer(args, layer);
}
