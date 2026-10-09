#include "plugin.hpp"

Plugin* pluginInstance;

// Called once when Rack loads the plugin. Every Model registered here must also be listed in plugin.json,
// with a matching slug, or Rack refuses to load the plugin.
void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelExample);
	/* your models here */
}
