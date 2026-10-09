/*
Example: a morphing oscillator with a built-in scope.

The module itself is small. Its job is to show, in working code, the pieces most modules are made of:
  - every kind of param: knobs, knobs that snap to named values, a toggle switch, a momentary button
  - ParamQuantity subclasses that display and accept values in real units (Hz, %, signed numbers)
  - modulatable parameters: knob + attenuverter × CV input, laid out as one connected group
  - polyphony, triggers in (SchmittTrigger) and out (PulseGenerator), lights updated at a reduced rate
  - realtime graphics: audio thread → lock-free hand-off → UI thread → NanoVG, on layer 1
  - extra state saved with the patch, and a context menu to change it
  - light and dark panels that follow Rack's "prefer dark panels" setting

Start a new module by copying this file, renaming Example everywhere, adding the Model to plugin.hpp,
plugin.cpp and plugin.json, and giving it a panel (see res-src/panel.py).
*/
#include "plugin.hpp"
#include "ui.hpp"
#include "Screen.hpp"
#include "TripleBuffer.hpp"
#include <deque>


static std::string formatHz(float hz) {
	int decimals = hz < 10.f ? 3 : hz < 100.f ? 2 : hz < 1000.f ? 1 : 0;
	return string::f("%.*f", decimals, hz);
}

/** Formats its value with a printf format, for fixed-width readouts: "%+.2f" gives "+0.25". */
struct FormatQuantity : ParamQuantity {
	const char* format = "%.2f";

	std::string getDisplayValueString() override {
		return string::f(format, getDisplayValue());
	}
};

/** Shows the frequency knob, which is in octaves, as Hz in the current range, and accepts Hz when typed in. */
struct FreqQuantity : ParamQuantity {
	float getDisplayValue() override;
	void setDisplayValue(float hz) override;

	std::string getDisplayValueString() override {
		return formatHz(getDisplayValue());
	}
};


/** One sweep of the scope: `cycles` periods of channel 0, phase-locked so the trace stands still at any pitch. */
struct ScopeFrame {
	enum { SIZE = 256 };
	float y[SIZE] = {};
	int head = -1;       // last point written; the points after it still hold the previous sweep
	uint32_t sweep = 0;  // counts sweeps, so the UI can tell when one finishes
	float frequency = 0.f;
	int channels = 1;
};

struct ScreenTheme {
	const char* name;
	NVGcolor trace, highlight;
};

static const ScreenTheme SCREEN_THEMES[] = {
	{"Cyan", nvgRGB(0x4c, 0xbe, 0xe4), nvgRGB(0xff, 0xd6, 0x5a)},
	{"Green", nvgRGB(0x52, 0xc8, 0x77), nvgRGB(0xf2, 0xd6, 0x55)},
	{"Amber", nvgRGB(0xe0, 0x9a, 0x3c), nvgRGB(0x8a, 0xdc, 0xe6)},
	{"Violet", nvgRGB(0x94, 0x79, 0xd0), nvgRGB(0xff, 0xd6, 0x5a)},
	{"White", nvgRGB(0xb8, 0xba, 0xc0), nvgRGB(0xff, 0xd6, 0x5a)},
	/* your colours here */
};


struct Example : Module {
	enum ParamId {
		// Knobs, attenuverters and CV inputs of the modulatable parameters are declared in the same order,
		// so modulated() can find a knob's partners by offset.
		FREQ_PARAM,
		SHAPE_PARAM,
		SKEW_PARAM,
		DRIVE_PARAM,
		FREQ_CV_PARAM,
		SHAPE_CV_PARAM,
		SKEW_CV_PARAM,
		DRIVE_CV_PARAM,
		RANGE_PARAM,
		MODE_PARAM,
		CYCLES_PARAM,
		TRAIL_PARAM,
		RESET_PARAM,
		POLARITY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FREQ_INPUT,
		SHAPE_INPUT,
		SKEW_INPUT,
		DRIVE_INPUT,
		VOCT_INPUT,
		SYNC_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		PHASE_OUTPUT,
		EOC_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(OUT_LIGHT, 2), // a green/red pair: ENUMS reserves consecutive ids
		EOC_LIGHT,
		LIGHTS_LEN
	};
	enum Mode { FOLD, CLIP, WRAP };

	struct Voice {
		float phase = 0.f;
		dsp::SchmittTrigger sync;
		dsp::PulseGenerator eoc;
	};

	// Audio thread state
	Voice voices[PORT_MAX_CHANNELS];
	dsp::BooleanTrigger resetButton;
	dsp::PulseGenerator eocLight;
	dsp::ClockDivider lightDivider;
	ScopeFrame scope;
	int scopeCycle = 0;
	float scopeLast = 0.f;
	dsp::ClockDivider scopeDivider;

	// Shared with the UI thread
	TripleBuffer<ScopeFrame> scopeOut;
	int screenTheme = 0;

	Example() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam<FreqQuantity>(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz");
		configParam<FormatQuantity>(SHAPE_PARAM, 0.f, 3.f, 0.f, "Shape")->description = "Sine → triangle → saw → square";
		configParam<FormatQuantity>(SKEW_PARAM, -1.f, 1.f, 0.f, "Skew")->format = "%+.2f";
		configParam<FormatQuantity>(DRIVE_PARAM, 0.f, 1.f, 0.f, "Drive", "%", 0.f, 100.f)->format = "%.0f";
		for (int i = 0; i < 4; i++) {
			std::string name = paramQuantities[FREQ_PARAM + i]->name;
			configParam(FREQ_CV_PARAM + i, -1.f, 1.f, 0.f, name + " CV amount", "%", 0.f, 100.f);
			configInput(FREQ_INPUT + i, name + " CV");
		}

		// A switch quantity on a knob makes it snap between named values, shown in the tooltip and on screen.
		configSwitch(RANGE_PARAM, 0.f, 1.f, 1.f, "Range", {"LFO", "AUDIO"});
		configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Drive mode", {"FOLD", "CLIP", "WRAP"});
		configParam(CYCLES_PARAM, 1.f, 8.f, 2.f, "Scope cycles", " CYC")->snapEnabled = true;
		configParam<FormatQuantity>(TRAIL_PARAM, 0.f, 1.f, 0.5f, "Scope trails", "%", 0.f, 100.f)->format = "%.0f";
		configButton(RESET_PARAM, "Reset phase");
		configSwitch(POLARITY_PARAM, 0.f, 1.f, 0.f, "Output", {"Bipolar ±5V", "Unipolar 0-10V"});

		configInput(VOCT_INPUT, "1V/octave pitch");
		configInput(SYNC_INPUT, "Sync");
		configOutput(OUT_OUTPUT, "Waveform");
		configOutput(PHASE_OUTPUT, "Phase (0-10V ramp)");
		configOutput(EOC_OUTPUT, "End of cycle trigger");
		configLight(OUT_LIGHT, "Output polarity");
		configLight(EOC_LIGHT, "End of cycle");
		/* your configuration here. For an effect, configBypass(IN_INPUT, OUT_OUTPUT) passes audio through when bypassed. */

		lightDivider.setDivision(32);
	}

	float baseFrequency() {
		return params[RANGE_PARAM].getValue() > 0.f ? dsp::FREQ_C4 : 1.f;
	}

	/** A knob plus its attenuverter times its CV, clamped to the knob's range. At full attenuation, ±5V
	sweeps half the range either way, so from the centre a knob reaches both ends. */
	float modulated(int param, int channel) {
		int i = param - FREQ_PARAM;
		ParamQuantity* q = paramQuantities[param];
		float cv = params[FREQ_CV_PARAM + i].getValue() * inputs[FREQ_INPUT + i].getPolyVoltage(channel) / 10.f;
		return clamp(params[param].getValue() + cv * (q->maxValue - q->minValue), q->minValue, q->maxValue);
	}

	/** The waveform at `phase` in [0, 1), in [-1, 1]: a sine → triangle → saw → square morph, skewed, then
	driven into a folder, clipper or wrapper. Computed naively, so it aliases at audio rates; band-limit
	it (see dsp::MinBlepGenerator) or oversample it before using this in earnest. */
	static float waveform(float phase, float shape, float skew, float drive, int mode) {
		// Skew moves the half-cycle point, squeezing one half of the wave and stretching the other.
		float mid = 0.5f + 0.45f * skew;
		float p = phase < mid ? 0.5f * phase / mid : 0.5f + 0.5f * (phase - mid) / (1.f - mid);
		auto basic = [p](int k) -> float {
			switch (k) {
				case 0: return std::sin(2.f * float(M_PI) * p);
				case 1: return 1.f - 4.f * std::fabs(eucMod(p + 0.25f, 1.f) - 0.5f);
				case 2: return 2.f * eucMod(p + 0.5f, 1.f) - 1.f;
				default: return p < 0.5f ? 1.f : -1.f;
			}
		};
		int k = std::min((int) shape, 2);
		float x = crossfade(basic(k), basic(k + 1), shape - k) * (1.f + 4.f * drive);
		if (std::fabs(x) <= 1.f)
			return x;
		switch (mode) {
			case FOLD: return 1.f - std::fabs(eucMod(x + 1.f, 4.f) - 2.f);
			case CLIP: return clamp(x, -1.f, 1.f);
			default: return eucMod(x + 1.f, 2.f) - 1.f;
		}
	}

	/** Writes one sample into the scope at its place in the sweep, filling any points skipped since the last. */
	void capture(float phase, bool wrapped, bool restart, int cycles, float y) {
		if (restart)
			scopeCycle = 0;
		else if (wrapped && ++scopeCycle >= cycles)
			scopeCycle = 0;
		int i = clamp((int) ((scopeCycle + phase) / cycles * ScopeFrame::SIZE), 0, ScopeFrame::SIZE - 1);
		if (i < scope.head) {
			scope.sweep++;
			scope.head = -1;
		}
		for (int j = scope.head + 1; j < i; j++)
			scope.y[j] = crossfade(scopeLast, y, float(j - scope.head) / (i - scope.head));
		scope.y[i] = y;
		scope.head = i;
		scopeLast = y;
	}

	void process(const ProcessArgs& args) override {
		// Polyphonic: as many voices as the widest input carries.
		int channels = 1;
		for (Input& input : inputs)
			channels = std::max(channels, input.getChannels());

		float base = baseFrequency();
		int mode = (int) params[MODE_PARAM].getValue();
		int cycles = (int) params[CYCLES_PARAM].getValue();
		bool unipolar = params[POLARITY_PARAM].getValue() > 0.f;
		bool reset = resetButton.process(params[RESET_PARAM].getValue() > 0.f);
		float frequency0 = 0.f;

		for (int c = 0; c < channels; c++) {
			Voice& v = voices[c];
			float pitch = modulated(FREQ_PARAM, c) + inputs[VOCT_INPUT].getPolyVoltage(c);
			float frequency = std::min(base * dsp::exp2_taylor5(pitch), args.sampleRate / 2.f);
			bool restart = v.sync.process(inputs[SYNC_INPUT].getPolyVoltage(c), 0.1f, 1.f) || reset;

			v.phase += frequency * args.sampleTime;
			bool wrapped = v.phase >= 1.f;
			v.phase = restart ? 0.f : v.phase - std::floor(v.phase);
			if (wrapped)
				v.eoc.trigger(1e-3f);

			float y = waveform(v.phase, modulated(SHAPE_PARAM, c), modulated(SKEW_PARAM, c), modulated(DRIVE_PARAM, c), mode);
			outputs[OUT_OUTPUT].setVoltage(unipolar ? 5.f * (y + 1.f) : 5.f * y, c);
			outputs[PHASE_OUTPUT].setVoltage(10.f * v.phase, c);
			outputs[EOC_OUTPUT].setVoltage(v.eoc.process(args.sampleTime) ? 10.f : 0.f, c);

			if (c == 0) {
				frequency0 = frequency;
				capture(v.phase, wrapped, restart, cycles, y);
				if (wrapped)
					eocLight.trigger(0.05f);
			}
		}
		for (Output& output : outputs)
			output.setChannels(channels);

		// Lights don't need audio-rate updates. Smoothing keeps fast changes visible as a glow.
		if (lightDivider.process()) {
			float dt = args.sampleTime * lightDivider.getDivision();
			lights[OUT_LIGHT + 0].setBrightnessSmooth(std::max(scopeLast, 0.f), dt);
			lights[OUT_LIGHT + 1].setBrightnessSmooth(std::max(-scopeLast, 0.f), dt);
			lights[EOC_LIGHT].setBrightnessSmooth(eocLight.process(dt) ? 1.f : 0.f, dt);
		}

		// Hand the scope to the UI at about the display's frame rate.
		if (scopeDivider.process()) {
			scope.frequency = frequency0;
			scope.channels = channels;
			scopeOut.write() = scope;
			scopeOut.publish();
		}
		/* your DSP here */
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		scopeDivider.setDivision(std::max(1, (int) (e.sampleRate / 60.f)));
	}

	// Initialize (right-click → Initialize) resets the params; reset any other state here.
	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (Voice& v : voices)
			v.phase = 0.f;
	}

	// Params are saved with the patch automatically. Anything else worth keeping goes in here.
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "screenTheme", json_integer(screenTheme));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* theme = json_object_get(root, "screenTheme"))
			screenTheme = clamp((int) json_integer_value(theme), 0, (int) LENGTHOF(SCREEN_THEMES) - 1);
	}
};


float FreqQuantity::getDisplayValue() {
	return static_cast<Example*>(module)->baseFrequency() * std::pow(2.f, getValue());
}

void FreqQuantity::setDisplayValue(float hz) {
	if (hz > 0.f)
		setValue(std::log2(hz / static_cast<Example*>(module)->baseFrequency()));
}


struct ExampleScreen : Screen {
	enum { MAX_TRAILS = 8 };
	Example* module = nullptr;
	ScopeFrame live;
	std::deque<ScopeFrame> trails;
	double lastTrail = 0.0;

	// The UI thread steps every widget once per frame before drawing; that's the place to pick up new data.
	void step() override {
		if (module && module->scopeOut.update()) {
			const ScopeFrame& next = module->scopeOut.read();
			// Keep a finished sweep as a trail at most every 80 ms, so the trails show the last moment or so of history.
			double now = system::getTime();
			if (next.sweep != live.sweep && now - lastTrail >= 0.08) {
				// Its start is in the last frame we saw, and its end not yet overwritten in this one.
				trails.push_front(next);
				std::copy(live.y, live.y + std::max(next.head + 1, 0), trails.front().y);
				if (trails.size() > MAX_TRAILS)
					trails.pop_back();
				lastTrail = now;
			}
			live = next;
		}
		Screen::step();
	}

	/** What the module browser shows, where there is no module to read. */
	static const ScopeFrame& preview() {
		static const ScopeFrame frame = []() -> ScopeFrame {
			ScopeFrame f;
			for (int i = 0; i < ScopeFrame::SIZE; i++)
				f.y[i] = Example::waveform(eucMod(2.f * i / ScopeFrame::SIZE, 1.f), 0.5f, 0.3f, 0.4f, Example::FOLD);
			f.head = ScopeFrame::SIZE - 1;
			f.frequency = dsp::FREQ_C4;
			return f;
		}();
		return frame;
	}

	/** Strokes points [from, to] of a trace as a bright core inside a soft glow. */
	static void trace(NVGcontext* vg, math::Rect r, const float* y, int from, int to, NVGcolor color, float width) {
		if (to <= from)
			return;
		nvgBeginPath(vg);
		for (int i = from; i <= to; i++) {
			float px = r.pos.x + r.size.x * i / (ScopeFrame::SIZE - 1);
			float py = r.pos.y + r.size.y * 0.5f * (1.f - 0.9f * y[i]);
			if (i == from)
				nvgMoveTo(vg, px, py);
			else
				nvgLineTo(vg, px, py);
		}
		nvgLineJoin(vg, NVG_ROUND);
		nvgStrokeColor(vg, nvgTransRGBAf(color, 0.2f * color.a));
		nvgStrokeWidth(vg, 4.f * width);
		nvgStroke(vg);
		nvgStrokeColor(vg, color);
		nvgStrokeWidth(vg, width);
		nvgStroke(vg);
	}

	void drawGraphics(const DrawArgs& args, math::Rect r) override {
		NVGcontext* vg = args.vg;
		const ScreenTheme& theme = SCREEN_THEMES[module ? module->screenTheme : 0];
		const ScopeFrame& frame = module ? live : preview();
		int cycles = module ? (int) module->params[Example::CYCLES_PARAM].getValue() : 2;
		float trail = module ? module->params[Example::TRAIL_PARAM].getValue() : 0.f;

		// Graticule: the zero line and the boundaries between cycles.
		nvgBeginPath(vg);
		nvgMoveTo(vg, r.pos.x, r.getCenter().y);
		nvgLineTo(vg, r.getRight(), r.getCenter().y);
		for (int i = 1; i < cycles; i++) {
			float x = r.pos.x + r.size.x * i / cycles;
			nvgMoveTo(vg, x, r.pos.y);
			nvgLineTo(vg, x, r.getBottom());
		}
		nvgStrokeColor(vg, nvgTransRGBAf(theme.trace, 0.18f));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);

		// Additive blending, so overlapping traces bloom like phosphor. Restored by Screen's nvgRestore().
		nvgGlobalCompositeBlendFunc(vg, NVG_SRC_ALPHA, NVG_ONE);
		int n = std::min((int) std::round(trail * MAX_TRAILS), (int) trails.size());
		for (int k = n - 1; k >= 0; k--)
			trace(vg, r, trails[k].y, 0, ScopeFrame::SIZE - 1, nvgTransRGBAf(theme.trace, 0.8f * (n - k) / n), 1.f);
		// Slow sweeps are drawn as they are written, ahead of the previous sweep; fast ones as a whole.
		bool slow = frame.frequency / cycles < 15.f;
		int head = slow ? frame.head : ScopeFrame::SIZE - 1;
		trace(vg, r, frame.y, head + 1, ScopeFrame::SIZE - 1, nvgTransRGBAf(theme.trace, 0.7f), 1.f);
		trace(vg, r, frame.y, 0, head, theme.highlight, 1.4f);
		if (slow && frame.head >= 0) {
			nvgBeginPath(vg);
			nvgCircle(vg, r.pos.x + r.size.x * frame.head / (ScopeFrame::SIZE - 1), r.pos.y + r.size.y * 0.5f * (1.f - 0.9f * frame.y[frame.head]), 1.8f);
			nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
			nvgFill(vg);
		}
		nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);

		std::shared_ptr<window::Font> f = font();
		if (!f)
			return;
		nvgFontFaceId(vg, f->handle);
		nvgFontSize(vg, fontSize);
		nvgFillColor(vg, nvgRGB(0x9a, 0xa6, 0xb3));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		nvgText(vg, r.pos.x, r.pos.y, (formatHz(frame.frequency) + " Hz").c_str(), NULL);
		if (frame.channels > 1) {
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			nvgText(vg, r.getRight(), r.pos.y, string::f("%d CH", frame.channels).c_str(), NULL);
		}
		/* your graphics here */
	}
};


struct ExampleWidget : ModuleWidget {
	ExampleWidget(Example* module) {
		setModule(module);
		std::string panel = asset::plugin(pluginInstance, "res/Example.svg");
		setPanel(createPanel(panel, asset::plugin(pluginInstance, "res/Example-dark.svg")));

		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Every position comes from the panel's components layer. AT(X) looks up the component named X and
		// passes the module and enum value X along with it, so each widget names its component exactly once.
		PanelLayout layout(panel);
#define AT(ID) layout.center(#ID), module, Example::ID

		ExampleScreen* screen = createWidget<ExampleScreen>(Vec());
		screen->box = layout.box("SCREEN_WIDGET");
		screen->module = module;
		addChild(screen);

		// Modulatable parameters: knob, attenuverter and CV input, with the knob's value on screen above it.
		addParamWithReadout(screen, Screen::BOTTOM, createParamCentered<PastelKnob<Pastel::Salmon>>(AT(FREQ_PARAM)));
		addParamWithReadout(screen, Screen::BOTTOM, createParamCentered<PastelKnob<Pastel::Yellow>>(AT(SHAPE_PARAM)));
		addParamWithReadout(screen, Screen::BOTTOM, createParamCentered<PastelKnob<Pastel::Teal>>(AT(SKEW_PARAM)));
		addParamWithReadout(screen, Screen::BOTTOM, createParamCentered<PastelKnob<Pastel::Lavender>>(AT(DRIVE_PARAM)));
		addParam(createParamCentered<Trimpot>(AT(FREQ_CV_PARAM)));
		addParam(createParamCentered<Trimpot>(AT(SHAPE_CV_PARAM)));
		addParam(createParamCentered<Trimpot>(AT(SKEW_CV_PARAM)));
		addParam(createParamCentered<Trimpot>(AT(DRIVE_CV_PARAM)));
		addInput(createInputCentered<PJ301MPort>(AT(FREQ_INPUT)));
		addInput(createInputCentered<PJ301MPort>(AT(SHAPE_INPUT)));
		addInput(createInputCentered<PJ301MPort>(AT(SKEW_INPUT)));
		addInput(createInputCentered<PJ301MPort>(AT(DRIVE_INPUT)));

		// Soft keys: the value shows on the screen's right edge, level with the knob.
		addParamWithReadout(screen, Screen::RIGHT, createParamCentered<SoftKnob>(AT(RANGE_PARAM)));
		addParamWithReadout(screen, Screen::RIGHT, createParamCentered<SoftKnob>(AT(MODE_PARAM)));
		addParamWithReadout(screen, Screen::RIGHT, createParamCentered<SoftKnob>(AT(CYCLES_PARAM)));
		addParamWithReadout(screen, Screen::RIGHT, createParamCentered<SoftKnob>(AT(TRAIL_PARAM)));

		addInput(createInputCentered<PJ301MPort>(AT(VOCT_INPUT)));
		addInput(createInputCentered<PJ301MPort>(AT(SYNC_INPUT)));
		addParam(createParamCentered<PushButton>(AT(RESET_PARAM)));
		addParam(createParamCentered<CKSS>(AT(POLARITY_PARAM)));

		addOutput(createOutputCentered<PJ301MPort>(AT(OUT_OUTPUT)));
		addOutput(createOutputCentered<PJ301MPort>(AT(PHASE_OUTPUT)));
		addOutput(createOutputCentered<PJ301MPort>(AT(EOC_OUTPUT)));
		addChild(createLightCentered<SmallLight<GreenRedLight>>(AT(OUT_LIGHT)));
		addChild(createLightCentered<SmallLight<GreenLight>>(AT(EOC_LIGHT)));
		/* your widgets here: add the component in res-src/panel.py, regenerate, then one line here */
#undef AT
	}

	void addParamWithReadout(Screen* screen, Screen::Edge edge, app::ParamWidget* knob) {
		addParam(knob);
		screen->addReadout(knob, edge);
	}

	void appendContextMenu(Menu* menu) override {
		Example* module = getModule<Example>();
		std::vector<std::string> themes;
		for (const ScreenTheme& theme : SCREEN_THEMES)
			themes.push_back(theme.name);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Screen colour", themes, &module->screenTheme));
		/* your menu items here */
	}
};


Model* modelExample = createModel<Example, ExampleWidget>("Example");
