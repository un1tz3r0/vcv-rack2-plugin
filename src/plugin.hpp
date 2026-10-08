#pragma once
#include <rack.hpp>

using namespace rack;

// Set by init() in plugin.cpp; asset::plugin(pluginInstance, "res/...") resolves paths inside this plugin.
extern Plugin* pluginInstance;

// One Model per module. Each is defined at the bottom of its module's .cpp and registered in plugin.cpp.
extern Model* modelExample;
/* your models here */
