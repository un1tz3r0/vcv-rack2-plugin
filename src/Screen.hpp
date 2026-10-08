#pragma once
#include "plugin.hpp"

/** Base for a self-illuminated display that also shows the values of the panel controls around it.

Readouts line up with the controls they belong to. BOTTOM readouts sit in a row along the bottom edge,
centred over their control (the panel draws a line between the two); RIGHT readouts sit in a column along
the right edge, level with their control. The text comes from the control's ParamQuantity, so the screen
always agrees with the tooltip, and the colour from the control's cap.

Subclasses draw realtime graphics in drawGraphics(), inside the space the readouts leave free. Everything is
drawn on layer 1, which Rack keeps at full brightness when the room lights are dimmed, as real screens do. */
struct Screen : widget::Widget {
	enum Edge { BOTTOM, RIGHT };

	struct Readout {
		app::ParamWidget* control;
		Edge edge;
		NVGcolor color;
	};

	std::vector<Readout> readouts;
	float fontSize = 8.f;
	float rowHeight = 13.f;
	float columnWidth = 34.f;
	float padding = 4.f;
	NVGcolor textColor = nvgRGB(0xdc, 0xdb, 0xd8);
	NVGcolor dividerColor = nvgRGB(0x34, 0x35, 0x3a);

	/** Shows the value of `control`, a widget already added to the same ModuleWidget, along `edge`. If the
	control's background graphic has a shape with id "cap", the text takes its colour. */
	void addReadout(app::ParamWidget* control, Edge edge);
	bool has(Edge edge) const;
	math::Rect graphicsBox() const;
	std::shared_ptr<window::Font> font() const;

	virtual void drawGraphics(const DrawArgs& args, math::Rect box) {}
	void drawReadouts(const DrawArgs& args);
	void drawLayer(const DrawArgs& args, int layer) override;
};
