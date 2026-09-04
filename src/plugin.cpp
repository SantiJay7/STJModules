#include "plugin.hpp"
#include <GLFW/glfw3.h>
#include <componentlibrary.hpp>
#include <thread>
#include <osdialog.h>

#include <cmath>
#include <chrono>
#include <algorithm>
#include <random>
#include <vector>
#include <string>

using namespace rack;

Plugin* pluginInstance;

// ============================================================================
// Constantes de geometría (coinciden con gen_panel.py)
// ============================================================================

static const float UNIT_W = 180.f;
static const float PANEL_H = 380.f;
static const float COLLAPSED_W = 50.f; // Ancho al colapsar (conmutador + línea)
static const float KEY_X0 = 10.f, KEY_Y0 = 68.f;
static const float KW = 37.f, KH = 37.f;
static const float PX = 41.f, PY = 70.f;

static const int MAX_UNITS = 3;

/** Tecla física GLFW por fila/columna global del teclado (solo alfanuméricas). */
static int physKey(int r, int c) {
	static const int rows[4][12] = {
		{GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5, GLFW_KEY_6,
		 GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9, GLFW_KEY_0, GLFW_KEY_MINUS, GLFW_KEY_EQUAL},
		{GLFW_KEY_Q, GLFW_KEY_W, GLFW_KEY_E, GLFW_KEY_R, GLFW_KEY_T, GLFW_KEY_Y,
		 GLFW_KEY_U, GLFW_KEY_I, GLFW_KEY_O, GLFW_KEY_P,
		 GLFW_KEY_LEFT_BRACKET, GLFW_KEY_RIGHT_BRACKET},
		{GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D, GLFW_KEY_F, GLFW_KEY_G, GLFW_KEY_H,
		 GLFW_KEY_J, GLFW_KEY_K, GLFW_KEY_L,
		 GLFW_KEY_SEMICOLON, GLFW_KEY_APOSTROPHE, GLFW_KEY_BACKSLASH},
		{GLFW_KEY_Z, GLFW_KEY_X, GLFW_KEY_C, GLFW_KEY_V, GLFW_KEY_B, GLFW_KEY_N,
		 GLFW_KEY_M, GLFW_KEY_COMMA, GLFW_KEY_PERIOD, GLFW_KEY_SLASH, -1, -1},
	};
	if (r < 0 || r > 3 || c < 0 || c > 11)
		return -1;
	return rows[r][c];
}

// ============================================================================
// Distribuciones de teclado (etiquetas por posición física)
// ============================================================================

// slot = unidad*16 + fila*4 + columna (dentro de la unidad)

static const char* LAYOUT_EN[4][12] = {
	{"1","2","3","4","5","6","7","8","9","0","-","="},
	{"Q","W","E","R","T","Y","U","I","O","P","[","]"},
	{"A","S","D","F","G","H","J","K","L",";","'","\\"},
	{"Z","X","C","V","B","N","M",",",".","/","",""},
};
static const char* LAYOUT_DE[4][12] = {
	{"1","2","3","4","5","6","7","8","9","0","\xC3\x9F","\xCB\x88"},
	{"Q","W","E","R","T","Z","U","I","O","P","\xC3\x9C","+"},
	{"A","S","D","F","G","H","J","K","L","\xC3\x96","\xC3\x84","#"},
	{"Y","X","C","V","B","N","M",",",".","-","",""},
};
static const char* LAYOUT_ES[4][12] = {
	{"1","2","3","4","5","6","7","8","9","0","'","\xC2\xBF"},
	{"Q","W","E","R","T","Y","U","I","O","P","`","+"},
	{"A","S","D","F","G","H","J","K","L","\xC3\x91","\xC2\xB4","\xC3\x87"},
	{"Z","X","C","V","B","N","M",",",".","-","",""},
};

static const char* LAYOUT_NAMES[3] = {"Deutsch QWERTZ", "English", "Espa\xC3\xB1ol"};

/** Carga la primera fuente disponible. loadFont() puede LANZAR si el archivo
    falta o es invalido, asi que se protege con try/catch y se memoiza. */
static std::shared_ptr<window::Font> loadUiFont() {
	static std::shared_ptr<window::Font> cached;
	static bool tried = false;
	if (!tried) {
		tried = true;
		static const char* candidates[] = {
			"res/fonts/DejaVuSans-Bold.ttf",
			"res/fonts/Nunito-Bold.ttf",
			"res/fonts/DejaVuSans.ttf",
			"res/fonts/ShareTechMono-Regular.ttf",
		};
		for (const char* path : candidates) {
			try {
				auto f = APP->window->loadFont(asset::system(path));
				if (f && f->handle >= 0) {
					cached = f;
					break;
				}
			} catch (...) {
				// Fuente no disponible: probar la siguiente
			}
		}
	}
	return cached;
}

static bool fontOk(const std::shared_ptr<window::Font>& f) {
	return f && f->handle >= 0;
}

// ============================================================================
// Módulo
// ============================================================================

static int64_t nowMs(); // definida al final del archivo (requiere <chrono>)
static float wizarRand01(); // definida al final del archivo (modo Random)

enum WizarParamIds {
	CONTROL_PARAM, // Botón luminoso de modo control
	NUM_PARAMS
};

enum WizarLightIds {
	CONTROL_LIGHT,
	NUM_LIGHTS
};

struct KeyConfig {
	bool bound = false;
	// Uno o varios controles objetivo (misma tecla, mismos ajustes).
	struct Target {
		int64_t moduleId = -1;
		int paramId = -1;
		engine::Module* target = NULL; // caché resuelta (nunca buscar en el hilo de audio)
	};
	std::vector<Target> targets;
	bool isFader = false; // false = Button
	bool toggle = true;   // solo Button
	float hMin = 0.f;     // Fader/Knob: altura mínima (%)
	float hMax = 1.f;     // Fader/Knob: altura máxima (%)
	float velTime = 0.5f; // Fader/Knob: tiempo en alcanzar hMax (0.02s..60s)
	float value = 0.f;    // valor actual normalizado dentro de [hMin,hMax]
	bool pressed = false;
	// Fader reversible: cada pulsacion alterna la direccion;
	// el extremo (Height min/max) se lee SIEMPRE en vivo
	bool faderActive = false;
	bool faderUp = false;
	bool morse = false;       // Modo Morse: pasos de 1% por pulsación (corto + / largo -)
	int64_t pressMs = 0;     // Morse: instante de la pulsación (ms)
	bool random = false;     // Modo Random: valor aleatorio en [randMin,randMax] por pulsación
	float randMin = 0.f;     // Random: límite inferior (% del rango del parámetro)
	float randMax = 1.f;     // Random: límite superior (% del rango del parámetro)
};

struct WizarKeyboardModule : Module {
	int units = 1;
	int layoutIdx = 1; // English por defecto
	float morseThresholdMs = 250.f; // Umbral corto/largo para modo Morse (global, ms)
	bool collapsed = false;     // Módulo recogido a la izquierda (solo conmutador + línea)
	KeyConfig keys[MAX_UNITS * 16];
	int pendingMapSlot = -1;

	WizarKeyboardModule() {
		config(NUM_PARAMS, 0, 0, NUM_LIGHTS);
		configSwitch(CONTROL_PARAM, 0.f, 1.f, 0.f, "Modo control");
	}

	~WizarKeyboardModule() {
	}

	bool isExclusive() {
		return params[CONTROL_PARAM].getValue() > 0.5f;
	}

	bool slotValid(int slot) {
		return slot >= 0 && physKeyValid(slot);
	}

	bool physKeyValid(int slot) {
		int u = slot / 16, r = (slot % 16) / 4, c = slot % 4;
		return physKey(r, u * 4 + c) != -1;
	}

	const char* keyLabel(int slot) {
		int u = slot / 16, r = (slot / 4) % 4, c = slot % 4 + u * 4;
		const char* (*table)[12] = nullptr;
		switch (layoutIdx) {
			case 0: table = LAYOUT_DE; break;
			case 2: table = LAYOUT_ES; break;
			default: table = LAYOUT_EN; break;
		}
		return table[r][c];
	}

	/** Re-resuelve los punteros en caché. SOLO desde el hilo de UI.
	    Desmapea automáticamente los targets cuyo módulo ya no existe. */
	void refreshTargets() {
		if (!APP || !APP->engine)
			return;
		for (int i = 0; i < MAX_UNITS * 16; i++) {
			KeyConfig& k = keys[i];
			if (k.targets.empty()) {
				k.bound = false;
				continue;
			}
			bool any = false;
			for (size_t j = 0; j < k.targets.size(); ) {
				engine::Module* m = APP->engine->getModule(k.targets[j].moduleId);
				if (m) {
					k.targets[j].target = m;
					any = true;
					j++;
				} else {
					k.targets.erase(k.targets.begin() + j);
				}
			}
			k.bound = any;
			if (!k.bound) {
				k.pressed = false;
				k.value = k.hMin;
			}
		}
	}

	void applyValue(int slot, float normIn) {
		KeyConfig& k = keys[slot];
		float lo = k.morse ? 0.f : (k.random ? k.randMin : k.hMin);
		float hi = k.morse ? 1.f : (k.random ? k.randMax : k.hMax);
		float norm = clamp(normIn, lo, hi);
		k.value = norm;
		for (KeyConfig::Target& t : k.targets) {
			engine::Module* m = t.target;
			if (!m)
				continue;
			if (t.paramId < 0 || t.paramId >= (int) m->paramQuantities.size())
				continue;
			engine::ParamQuantity* pq = m->paramQuantities[t.paramId];
			if (!pq)
				continue;
			pq->setScaledValue(norm);
		}
	}

	void keyPress(int slot) {
		KeyConfig& k = keys[slot];
		k.pressed = true;
		if (!k.bound || pendingMapSlot == slot)
			return;
		if (k.morse) {
			// Modo Morse: registra el instante; el paso se decide al soltar
			k.pressMs = nowMs();
		} else if (k.random) {
			// Modo Random: valor aleatorio dentro de [randMin,randMax] por pulsación
			float lo = k.randMin, hi = k.randMax;
			if (hi < lo)
				std::swap(hi, lo);
			applyValue(slot, lo + (hi - lo) * wizarRand01());
		} else if (!k.isFader) {
			if (k.toggle)
				applyValue(slot, k.value > (k.hMin + k.hMax) / 2.f ? k.hMin : k.hMax);
			else
				applyValue(slot, k.hMax);
		} else {
			// Fader/Knob reversible: cada pulsacion invierte la direccion
			k.faderUp = !k.faderUp;
			k.faderActive = true;
		}
	}

	void keyRelease(int slot) {
		KeyConfig& k = keys[slot];
		k.pressed = false;
		if (!k.bound)
			return;
		if (k.morse) {
			// Morse: corto = +1%, largo = -1% (ignora Height/Velocity)
			int64_t dur = nowMs() - k.pressMs;
			float step = 0.01f;
			float target = (dur < morseThresholdMs) ? k.value + step : k.value - step;
			applyValue(slot, target);
			return;
		}
		// Botón momentáneo vuelve al mínimo al soltar
		if (!k.isFader && !k.toggle)
			applyValue(slot, k.hMin);
	}

	/** Elimina el mapeo de una tecla (todos sus targets). */
	void unbind(int slot) {
		KeyConfig& k = keys[slot];
		k.faderActive = false;
		k.bound = false;
		k.targets.clear();
		k.pressed = false;
		k.value = k.hMin;
	}

	/** Añade un target resuelto a la tecla (idempotente). Llamado por el
	    overlay de aprendizaje; todos los targets comparten los ajustes. */
	void endLearn(int slot, int64_t moduleId, int paramId, engine::Module* mod) {
		KeyConfig& k = keys[slot];
		for (const KeyConfig::Target& t : k.targets)
			if (t.moduleId == moduleId && t.paramId == paramId)
				return; // ya está: no duplicar
		KeyConfig::Target t;
		t.moduleId = moduleId;
		t.paramId = paramId;
		t.target = mod;
		k.targets.push_back(t);
		k.bound = true;
		k.faderActive = false;
		k.faderUp = false;
		applyValue(slot, k.hMin);
	}

	void process(const ProcessArgs& args) override {
		lights[CONTROL_LIGHT].setBrightness(isExclusive() ? 1.f : 0.f);
		// Rampas autonomas de fader/knob (reversibles por pulsacion)
		for (int i = 0; i < MAX_UNITS * 16; i++) {
			KeyConfig& k = keys[i];
			if (!k.isFader || !k.bound || !k.faderActive || k.morse || k.random)
				continue;
			float dur = clamp(k.velTime, 0.02f, 60.f);
			float rate = args.sampleTime / dur;
			float target = k.faderUp ? k.hMax : k.hMin;
			float diff = target - k.value;
			if (std::fabs(diff) <= rate) {
				applyValue(i, target);
				k.faderActive = false;
			} else {
				applyValue(i, k.value + std::copysign(rate, diff));
			}
		}
	}

	/** Devuelve la ranura que controla un parámetro, o -1 si ninguna. */
	int findKeyByParam(int64_t moduleId, int paramId) {
		for (int i = 0; i < MAX_UNITS * 16; i++)
			for (const KeyConfig::Target& t : keys[i].targets)
				if (t.moduleId == moduleId && t.paramId == paramId)
					return i;
		return -1;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "units", json_integer(units));
		json_object_set_new(rootJ, "layout", json_integer(layoutIdx));
		json_object_set_new(rootJ, "morseMs", json_real(morseThresholdMs));
		json_object_set_new(rootJ, "collapsed", json_boolean(collapsed));
		json_t* keysJ = json_array();
		for (int i = 0; i < MAX_UNITS * 16; i++) {
			KeyConfig& k = keys[i];
			json_t* kJ = json_object();
			json_object_set_new(kJ, "slot", json_integer(i));
			json_object_set_new(kJ, "bound", json_boolean(k.bound));
			// Targets (uno o varios)
			if (k.bound) {
				json_t* tJ = json_array();
				for (const KeyConfig::Target& t : k.targets) {
					json_t* o = json_object();
					json_object_set_new(o, "moduleId", json_integer(t.moduleId));
					json_object_set_new(o, "paramId", json_integer(t.paramId));
					json_array_append_new(tJ, o);
				}
				json_object_set_new(kJ, "targets", tJ);
				// Legado (primer target) para patches antiguos
				if (!k.targets.empty()) {
					json_object_set_new(kJ, "moduleId", json_integer(k.targets[0].moduleId));
					json_object_set_new(kJ, "paramId", json_integer(k.targets[0].paramId));
				}
			}
			json_object_set_new(kJ, "isFader", json_boolean(k.isFader));
			json_object_set_new(kJ, "toggle", json_boolean(k.toggle));
			json_object_set_new(kJ, "hMin", json_real(k.hMin));
			json_object_set_new(kJ, "hMax", json_real(k.hMax));
			json_object_set_new(kJ, "velTime", json_real(k.velTime));
			json_object_set_new(kJ, "morse", json_boolean(k.morse));
			json_object_set_new(kJ, "random", json_boolean(k.random));
			json_object_set_new(kJ, "randMin", json_real(k.randMin));
			json_object_set_new(kJ, "randMax", json_real(k.randMax));
			json_array_append_new(keysJ, kJ);
		}
		json_object_set_new(rootJ, "keys", keysJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* unitsJ = json_object_get(rootJ, "units");
		if (unitsJ)
			units = clamp(json_integer_value(unitsJ), 1, MAX_UNITS);
		json_t* layoutJ = json_object_get(rootJ, "layout");
		if (layoutJ)
			layoutIdx = clamp(json_integer_value(layoutJ), 0, 2);
		json_t* morseMsJ = json_object_get(rootJ, "morseMs");
		if (morseMsJ)
			morseThresholdMs = clamp((float) json_real_value(morseMsJ), 50.f, 1500.f);
		json_t* collapsedJ = json_object_get(rootJ, "collapsed");
		if (collapsedJ)
			collapsed = json_boolean_value(collapsedJ);
		for (int i = 0; i < MAX_UNITS * 16; i++)
			keys[i] = KeyConfig();
		json_t* keysJ = json_object_get(rootJ, "keys");
		size_t idx;
		json_t* kJ;
		json_array_foreach(keysJ, idx, kJ) {
			int slot = json_integer_value(json_object_get(kJ, "slot"));
			if (slot < 0 || slot >= MAX_UNITS * 16)
				continue;
			KeyConfig& k = keys[slot];
			k.bound = json_boolean_value(json_object_get(kJ, "bound"));
			k.targets.clear();
			json_t* tJ = json_object_get(kJ, "targets");
			if (tJ) {
				size_t tidx; json_t* o;
				json_array_foreach(tJ, tidx, o) {
					KeyConfig::Target t;
					t.moduleId = json_integer_value(json_object_get(o, "moduleId"));
					t.paramId = json_integer_value(json_object_get(o, "paramId"));
					if (t.moduleId >= 0 && t.paramId >= 0)
						k.targets.push_back(t);
				}
			} else {
				// Legado: un solo target
				int64_t mid = json_integer_value(json_object_get(kJ, "moduleId"));
				int pid = json_integer_value(json_object_get(kJ, "paramId"));
				if (mid >= 0 && pid >= 0) {
					KeyConfig::Target t;
					t.moduleId = mid;
					t.paramId = pid;
					k.targets.push_back(t);
				}
			}
			k.bound = !k.targets.empty();
			k.isFader = json_boolean_value(json_object_get(kJ, "isFader"));
			k.toggle = json_boolean_value(json_object_get(kJ, "toggle"));
			k.hMin = json_real_value(json_object_get(kJ, "hMin"));
			k.hMax = json_real_value(json_object_get(kJ, "hMax"));
			k.hMin = std::max(0.f, std::min(k.hMin, k.hMax)); // suelo 0%
			k.hMax = std::max(k.hMin, k.hMax);
			k.velTime = json_real_value(json_object_get(kJ, "velTime"));
			k.morse = json_boolean_value(json_object_get(kJ, "morse"));
			k.random = json_boolean_value(json_object_get(kJ, "random"));
			json_t* rminJ = json_object_get(kJ, "randMin");
			json_t* rmaxJ = json_object_get(kJ, "randMax");
			k.randMin = rminJ ? clamp((float) json_real_value(rminJ), 0.f, 1.f) : k.hMin;
			k.randMax = rmaxJ ? clamp((float) json_real_value(rmaxJ), 0.f, 1.f) : k.hMax;
			k.value = k.hMin;
			k.pressed = false;
			for (KeyConfig::Target& t : k.targets)
				t.target = (APP && APP->engine) ? APP->engine->getModule_NoLock(t.moduleId) : NULL;
			if (!k.bound)
				for (KeyConfig::Target& t : k.targets)
					t.target = NULL;
		}
		pendingMapSlot = -1;
	}
};

extern Model* modelWizarKeyboard; // definido al final, tras el Widget

static bool blinkPhase() {
	using namespace std::chrono;
	auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	return (ms / 200) % 2 == 0;
}

static int64_t nowMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/** Generador uniforme [0,1) para el modo Random (un solo rng por hilo). */
static float wizarRand01() {
	using namespace std::chrono;
	static std::mt19937 rng(
		(unsigned) duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
	std::uniform_real_distribution<float> d(0.f, 1.f);
	return d(rng);
}

// Registro global de instancias (para badges y gancho exclusivo)
static std::vector<WizarKeyboardModule*> g_modules;
static int slotForKeyGlfw(int key);

// Portapapeles de comportamiento de tecla (solo ajustes, NO el mapeo)
static KeyConfig g_keyClip;
static bool g_hasClip = false;
static void copyKeySettings(KeyConfig& k) {
	g_keyClip = KeyConfig();
	g_keyClip.isFader = k.isFader;
	g_keyClip.toggle = k.toggle;
	g_keyClip.hMin = k.hMin;
	g_keyClip.hMax = k.hMax;
	g_keyClip.velTime = k.velTime;
	g_keyClip.morse = k.morse;
	g_hasClip = true;
}
static void pasteKeySettings(KeyConfig& k) {
	if (!g_hasClip)
		return;
	k.isFader = g_keyClip.isFader;
	k.toggle = g_keyClip.toggle;
	k.hMin = g_keyClip.hMin;
	k.hMax = g_keyClip.hMax;
	k.hMin = std::max(0.f, std::min(k.hMin, k.hMax)); // suelo 0%
	k.hMax = std::max(k.hMin, k.hMax);
	k.velTime = g_keyClip.velTime;
	k.morse = g_keyClip.morse;
}

// ============================================================================
// Captura exclusiva global (gancho GLFW)
// ============================================================================

static GLFWkeyfun g_prevKeyCb = NULL;
static GLFWcharfun g_prevCharCb = NULL;
static WizarKeyboardModule* g_hookModule = NULL;

// Singleton: step() solo marca killFlag en el duplicado; el removeModule real
// se hace en onButton/onHoverKey (eventos UI de Rack, igual que el menú
// Delete), porque removeModule fuera de un evento UI corrompe el motor.

static void wizarKeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods) {
	WizarKeyboardModule* m = g_hookModule;
	if (m && m->isExclusive()) {
		// Salida de emergencia
		if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
			m->params[CONTROL_PARAM].setValue(0.f);
			if (g_prevKeyCb)
				g_prevKeyCb(win, key, scancode, action, mods);
			return;
		}
		// Pasarelas permitidas: espacio y F1..F12
		bool passthrough = (key == GLFW_KEY_SPACE)
		    || (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12);
		if (!passthrough) {
			int slot = slotForKeyGlfw(key);
			if (slot >= 0) {
				if (action == GLFW_PRESS)
					m->keyPress(slot);
				else if (action == GLFW_RELEASE)
					m->keyRelease(slot);
			}
			return; // Todo lo demás queda silenciado
		}
	}
	if (g_prevKeyCb)
		g_prevKeyCb(win, key, scancode, action, mods);
}

static void wizarCharCallback(GLFWwindow* win, unsigned int codepoint) {
	WizarKeyboardModule* m = g_hookModule;
	if (m && m->isExclusive())
		return;
	if (g_prevCharCb)
		g_prevCharCb(win, codepoint);
}

static void hookInstall(WizarKeyboardModule* m) {
	if (g_hookModule == m)
		return;
	GLFWwindow* win = (APP && APP->window) ? APP->window->win : NULL;
	if (!win)
		return;
	g_prevKeyCb = glfwSetKeyCallback(win, wizarKeyCallback);
	g_prevCharCb = glfwSetCharCallback(win, wizarCharCallback);
	g_hookModule = m;
}

static void hookRemove(WizarKeyboardModule* m) {
	if (g_hookModule != m)
		return;
	GLFWwindow* win = (APP && APP->window) ? APP->window->win : NULL;
	if (win) {
		glfwSetKeyCallback(win, g_prevKeyCb);
		glfwSetCharCallback(win, g_prevCharCb);
	}
	g_prevKeyCb = NULL;
	g_prevCharCb = NULL;
	g_hookModule = NULL;
}

// Mapa GLFW -> ranura (posiciones físicas)
static int slotForKeyGlfw(int key) {
	static int map[512];
	static bool init = false;
	if (!init) {
		for (int i = 0; i < 512; i++)
			map[i] = -1;
		for (int u = 0; u < MAX_UNITS; u++)
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) {
					int k = physKey(r, u * 4 + c);
					if (k >= 0 && k < 512)
						map[k] = u * 16 + r * 4 + c;
				}
		init = true;
	}
	return (key >= 0 && key < 512) ? map[key] : -1;
}

// ============================================================================
// Capa de letras diminutas sobre los parámetros mapeados
// ============================================================================

struct WizarKeyboardWidget;

	struct BadgeOverlay : widget::TransparentWidget {
		BadgeOverlay() {
			box = math::Rect(0, 0, 1e6f, 1e6f);
		}

		void draw(const DrawArgs& args) override {
			std::shared_ptr<window::Font> font = loadUiFont();
			if (!fontOk(font))
				return;
		if (!APP || !APP->scene || !APP->scene->rack)
			return;
		for (WizarKeyboardModule* m : g_modules) {
			if (!m)
				continue;
			auto rack = APP->scene->rack;
			for (int i = 0; i < MAX_UNITS * 16; i++) {
				KeyConfig& k = m->keys[i];
				if (!k.bound)
					continue;
				for (const KeyConfig::Target& t : k.targets) {
					app::ModuleWidget* mw = rack->getModule(t.moduleId);
					if (!mw)
						continue;
					math::Vec center(-1, -1);
					for (app::ParamWidget* pw : mw->getParams()) {
						if (pw->paramId == t.paramId) {
							center = mw->box.pos.plus(pw->box.getCenter());
							break;
						}
					}
					if (center.x < 0)
						continue;
					float bx = center.x + 10.f, by = center.y + 11.f;
					nvgBeginPath(args.vg);
					nvgCircle(args.vg, bx, by, 6.5f);
					nvgFillColor(args.vg, nvgRGBA(10, 12, 20, 230));
					nvgFill(args.vg);
					nvgStrokeColor(args.vg, nvgRGB(0x50, 0xd0, 0xff));
					nvgStrokeWidth(args.vg, 1);
					nvgStroke(args.vg);
					nvgFontFaceId(args.vg, font->handle);
					nvgFontSize(args.vg, 11);
					nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
					// Halo luminoso para resaltar sobre cualquier fondo
					nvgFontBlur(args.vg, 4.f);
					nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 180));
					std::string label = m->keyLabel(i);
					nvgText(args.vg, bx, by + 0.5f, label.c_str(), NULL);
					// Trazo nítido
					nvgFontBlur(args.vg, 0.f);
					nvgBeginPath(args.vg);
					nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
					nvgText(args.vg, bx, by + 0.5f, label.c_str(), NULL);
				}
			}
		}
		TransparentWidget::draw(args);
	}
};

static BadgeOverlay* g_badges = NULL;

// ============================================================================
// Overlay modal de aprendizaje
// ============================================================================

/** Textos del overlay localizados según el layout elegido (0=DE, 1=EN, 2=ES). */
static void wizarOverlayMsgs(int layout, const std::string& key, int n,
                             std::string& msg, std::string& msg2) {
	if (n == 0) {
		switch (layout) {
			case 0: // Alemán
				msg = "MAP Taste [" + key + "]: klicke auf den zu steuernden Regler";
				msg2 = "SHIFT + Klick halten f\xC3\xBCr mehr \xC2\xB7 Rechtsklick / Esc abbrechen";
				break;
			case 2: // Español
				msg = "MAP tecla [" + key + "]: haz clic sobre el control a gobernar";
				msg2 = "Mant\xC3\xA9n SHIFT + clic para a\xC3\xB1adir m\xC3\xA1s \xC2\xB7 Clic der / Esc cancela";
				break;
			default: // Inglés
				msg = "MAP key [" + key + "]: click the control to drive";
				msg2 = "Hold SHIFT + click to add more \xC2\xB7 Right-click / Esc cancels";
				break;
		}
	} else {
		switch (layout) {
			case 0:
				msg = "Taste [" + key + "]: " + std::to_string(n) + " Parameter zugeordnet";
				msg2 = "SHIFT + Klick f\xC3\xBCgt weitere hinzu \xC2\xB7 Enter beendet \xC2\xB7 Esc abbrechen";
				break;
			case 2:
				msg = "Tecla [" + key + "]: " + std::to_string(n) + " par\xC3\xA1metros mapeados";
				msg2 = "SHIFT + clic a\xC3\xB1ade m\xC3\xA1s \xC2\xB7 Enter termina \xC2\xB7 Esc cancela";
				break;
			default:
				msg = "Key [" + key + "]: " + std::to_string(n) + " parameters mapped";
				msg2 = "SHIFT + click adds more \xC2\xB7 Enter finishes \xC2\xB7 Esc cancels";
				break;
		}
	}
}

static std::string wizarOverlayReject(int layout, const std::string& label) {
	switch (layout) {
		case 0: return "Bereits von Taste [" + label + "] gesteuert!";
		case 2: return "\xC2\xA1Ya controlado por la tecla [" + label + "]!";
		default: return "Already controlled by key [" + label + "]!";
	}
}

	struct LearnOverlay : widget::OpaqueWidget {
		WizarKeyboardModule* module;
		int slot;
		std::string rejectMsg;
		int rejectTimer = -1;

		LearnOverlay(WizarKeyboardModule* module, int slot) {
			this->module = module;
			this->slot = slot;
			DEBUG("Wizar: overlay MAP abierto para slot %d", slot);
			box.pos = math::Vec(0, 0);
			box.size = APP->scene->box.size;
			module->pendingMapSlot = slot;
		}

	~LearnOverlay() {
		if (module->pendingMapSlot == slot)
			module->pendingMapSlot = -1;
	}

	void finish() {
		DEBUG("Wizar: overlay cerrado");
		requestDelete();
	}

		void draw(const DrawArgs& args) override {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, box.pos.x, box.pos.y, box.size.x, box.size.y);
			nvgFillColor(args.vg, nvgRGBA(0, 0, 10, 120));
			nvgFill(args.vg);
			std::shared_ptr<window::Font> font = loadUiFont();
			if (fontOk(font)) {
			nvgFontFaceId(args.vg, font->handle);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			if (rejectTimer > 0) {
				nvgFontSize(args.vg, 18);
				nvgFillColor(args.vg, nvgRGBA(255, 90, 90, 255));
				nvgBeginPath(args.vg);
				nvgText(args.vg, box.size.x / 2, box.size.y / 2, rejectMsg.c_str(), NULL);
			} else {
				int n = (int) module->keys[slot].targets.size();
				std::string msg, msg2;
				wizarOverlayMsgs(module->layoutIdx, module->keyLabel(slot), n, msg, msg2);
				nvgFontSize(args.vg, 18);
				nvgFillColor(args.vg, nvgRGBA(255, 220, 80, 255));
				nvgBeginPath(args.vg);
				nvgText(args.vg, box.size.x / 2, box.size.y / 2 - 12, msg.c_str(), NULL);
				nvgFontSize(args.vg, 13);
				nvgFillColor(args.vg, nvgRGBA(200, 200, 210, 255));
				nvgBeginPath(args.vg);
				nvgText(args.vg, box.size.x / 2, box.size.y / 2 + 14, msg2.c_str(), NULL);
			}
		}
		OpaqueWidget::draw(args);
	}

	bool paramAt(math::Vec rackPos, int64_t* moduleId, int* paramId, engine::Module** mod) {
		auto modules = APP->scene->rack->getModules();
		for (app::ModuleWidget* mw : modules) {
			if (!mw->box.contains(rackPos))
				continue;
			for (app::ParamWidget* pw : mw->getParams()) {
				math::Rect abs = pw->box;
				abs.pos = abs.pos.plus(mw->box.pos);
				if (abs.contains(rackPos)) {
					if (!pw->module || pw->paramId < 0)
						continue;
					*moduleId = pw->module->id;
					*paramId = pw->paramId;
					*mod = pw->module;
					return true;
				}
			}
			return false; // clic dentro de un módulo pero fuera de un parámetro
		}
		return false;
	}

	void step() override {
		if (rejectTimer > 0) {
			rejectTimer--;
			if (rejectTimer == 0) {
				finish();
				return;
			}
		}
		if (parent)
			box.size = parent->box.size;
		OpaqueWidget::step();
	}

	void onButton(const ButtonEvent& e) override {
		DEBUG("Wizar: overlay click btn=%d act=%d pos=(%.0f,%.0f)", e.button, e.action, e.pos.x, e.pos.y);
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			e.consume(this);
			finish();
			return;
		}
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
			if (rejectTimer > 0)
				return;

			// Candidato 1: posicion oficial del RackWidget (espacio mundo)
			math::Vec p1 = APP->scene->rack->getMousePos();

			// Candidato 2: compositar la cadena de transformacion real del arbol
			math::Vec p2 = e.pos;
			float z = 1.f;
			app::RackScrollWidget* rs = APP->scene->rackScroll;
			if (rs && rs->container && rs->zoomWidget) {
				z = rs->getZoom();
				// escena = rs + cont + zw + z*(rackPos + mundo)
				// => mundo = ((escena - rs - cont - zw) / z) - rackPos
				math::Vec acc = rs->box.pos
					.plus(rs->container->box.pos)
					.plus(rs->zoomWidget->box.pos);
				p2 = e.pos.minus(acc).div(z).minus(APP->scene->rack->box.pos);
			}

			auto modules = APP->scene->rack->getModules();

			int64_t moduleId = -1;
			int paramId = -1;
			engine::Module* targetMod = NULL;
			bool ok = paramAt(p1, &moduleId, &paramId, &targetMod);
			if (!ok) {
				moduleId = -1;
				paramId = -1;
				targetMod = NULL;
				ok = paramAt(p2, &moduleId, &paramId, &targetMod);
				if (ok)
					DEBUG("Wizar: encontrado via p2 (conversion manual)");
			}
			if (!ok) {
				DEBUG("Wizar: ningun parametro ni en p1 ni en p2");
				return;
			}
			int ownerSlot = -1;
			std::string ownerLabel;
			for (WizarKeyboardModule* m : g_modules) {
				if (!m)
					continue;
				int o = m->findKeyByParam(moduleId, paramId);
				if (o >= 0) {
					ownerSlot = o;
					ownerLabel = m->keyLabel(o);
					break;
				}
			}
			if (ownerSlot >= 0 && ownerSlot != slot) {
				rejectMsg = wizarOverlayReject(module->layoutIdx, ownerLabel);
				rejectTimer = 120;
				return;
			}
			// ownerSlot == slot => ya es de esta tecla: se ignora (idempotente)
			module->endLearn(slot, moduleId, paramId, targetMod);
			bool shift = (e.mods & GLFW_MOD_SHIFT) != 0;
			if (shift) {
				// Mantener el overlay abierto para acumular más targets
				rejectTimer = -1;
			} else {
				finish();
			}
		}
	}

	void onHoverKey(const HoverKeyEvent& e) override {
		if (e.key == GLFW_KEY_ESCAPE && e.action == GLFW_PRESS) {
			e.consume(this);
			finish();
			return;
		}
		if (e.key == GLFW_KEY_ENTER && e.action == GLFW_PRESS) {
			if (!module->keys[slot].targets.empty()) {
				e.consume(this);
				finish();
			}
			return;
		}
		OpaqueWidget::onHoverKey(e);
	}
};

// ============================================================================
// Cantidades para deslizadores de menú
// ============================================================================

struct FloatQuantity : Quantity {
	float* val;
	float* other = NULL;
	bool isMinBound = false;
	bool disabled = false;
	float minV = 0.f; // rango explícito (0 = usar el de percent/time)
	float maxV = 0.f;
	float floorV = -1e9f; // suelo absoluto (p.ej. 0 para Height mínimo)
	bool ms = false;   // mostrar como "N ms"
	std::function<void()> onChange;
	std::string label;
	bool percent = false;
	bool time = false;

	FloatQuantity(float* val, std::string label, bool percent, bool time)
		: val(val), label(label), percent(percent), time(time) {}

	void setValue(float v) override {
		if (disabled)
			return;
		if (other) {
			if (isMinBound)
				v = std::min(v, *other);
			else
				v = std::max(v, *other);
		}
		if (v < floorV)
			v = floorV;
		*val = v;
		if (onChange)
			onChange();
	}
	float getValue() override { return *val; }
	float getMinValue() override { return (minV > 0.f || maxV > 0.f) ? minV : (percent ? 0.f : 0.02f); }
	float getMaxValue() override { return (minV > 0.f || maxV > 0.f) ? maxV : (percent ? 1.f : 60.f); }
	std::string getLabel() override { return label; }
	std::string getDisplayValueString() override {
		if (percent) {
			char b[16];
			snprintf(b, sizeof(b), "%.0f %%", *val * 100.f);
			return b;
		}
		if (ms) {
			char b[16];
			snprintf(b, sizeof(b), "%.0f ms", *val);
			return b;
		}
		if (*val <= 0.07f)
			return "instant\xC3\xA1neo";
		if (*val >= 59.f)
			return "1 minuto";
		char b[16];
		snprintf(b, sizeof(b), "%.1f s", *val);
		return b;
	}
};

// Declaraciones adelantadas
struct WizarKeyboardWidget;
static void startLearnFlow(WizarKeyboardModule* module, int slot);
static void requestSetUnits(WizarKeyboardWidget* w, int units);

/** ¿Es `m` un duplicado que debe morir? (no es la instancia de menor id). */
	static bool isWizarDuplicate(WizarKeyboardModule* m) {
		if (!m)
			return false;
		if (!APP || !APP->scene || !APP->scene->rack)
			return false;
		std::vector<int64_t> ids;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (!mw->module || mw->module->model != modelWizarKeyboard)
			continue;
		if (APP->engine->getModule(mw->module->id))
			ids.push_back(mw->module->id);
	}
	if (ids.size() <= 1)
		return false;
	int64_t minId = ids[0];
	for (int64_t id : ids)
		if (id < minId)
			minId = id;
	return m->id != minId;
}

// ============================================================================
// Widget de tecla
// ============================================================================

// Etiqueta por defecto (Inglés) para el preview del navegador (module == NULL)
static const char* defaultKeyLabel(int unit, int row, int col) {
	int c = col + unit * 4;
	if (c < 0 || c > 11)
		return "";
	return LAYOUT_EN[row][c];
}

// ¿El módulo está realmente en el motor? El módulo temporal del preview del
// navegador NO lo está, y no debe instalar el gancho ni reaccionar a eventos.
static bool wizarInEngine(WizarKeyboardModule* m) {
	return m && APP && APP->engine && APP->engine->getModule(m->id) != NULL;
}

	struct SlotWidget : widget::OpaqueWidget {
		WizarKeyboardModule* module;
		int unit, row, col, slot;

		SlotWidget(WizarKeyboardModule* module, int unit, int row, int col)
			: module(module), unit(unit), row(row), col(col) {
			slot = unit * 16 + row * 4 + col;
			box.size = math::Vec(KW, KH);
		}

	bool visible() {
		int u = module ? module->units : 1;
		return unit < u && physKey(row, unit * 4 + col) != -1
		    && !(module && module->collapsed);
	}

	void step() override {
		box.pos = math::Vec(unit * UNIT_W + KEY_X0 + col * PX, KEY_Y0 + row * PY);
		OpaqueWidget::step();
	}

		void draw(const DrawArgs& args) override {
			if (!visible())
				return;
			bool hasMod = module != NULL;
			bool learning = hasMod && module->pendingMapSlot == slot;
			bool pressed = hasMod && module->keys[slot].pressed;
			const char* label = hasMod ? module->keyLabel(slot) : defaultKeyLabel(unit, row, col);

			std::shared_ptr<window::Font> font = loadUiFont();
			if (fontOk(font)) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 15);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgBeginPath(args.vg);
			nvgFillColor(args.vg, nvgRGB(0x12, 0x14, 0x1c)); // negro: mejor lectura sobre tecla clara
			std::string labelS = label;
			nvgText(args.vg, box.size.x / 2, box.size.y / 2 + 1, labelS.c_str(), NULL);
		}

		if (pressed && !learning) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, box.pos.x, box.pos.y, box.size.x, box.size.y, 3);
			nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 60));
			nvgFill(args.vg);
		}

		if (learning && blinkPhase()) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, box.pos.x, box.pos.y, box.size.x, box.size.y, 3);
			nvgFillColor(args.vg, nvgRGBA(255, 220, 60, 100));
			nvgFill(args.vg);
			nvgStrokeColor(args.vg, nvgRGB(0xff, 0xdc, 0x3c));
			nvgStrokeWidth(args.vg, 2);
			nvgStroke(args.vg);
		}
		OpaqueWidget::draw(args);
	}

	void onButton(const ButtonEvent& e) override {
		if (!visible() || !module)
			return;
		// Preview del navegador: sin interacción
		if (!wizarInEngine(module))
			return;
		// Un duplicado NO se elimina aquí: borrar el módulo padre desde el
		// handler de un widget hijo corrompe el árbol de eventos de Rack.
		// Solo dejamos pasar el evento al WizarKeyboardWidget (padre), que sí
		// puede eliminarse a sí mismo de forma segura en su propio onButton.
		if (isWizarDuplicate(module))
			return;
		bool noMods = (e.mods & RACK_MOD_MASK) == 0;
		// En modo control no hay menús ni configuración: solo tocar
		if (noMods && e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			if (module->isExclusive()) {
				module->keyPress(slot);
				e.consume(this);
			}
		}
		else if (noMods && e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_RELEASE) {
			if (module->isExclusive()) {
				module->keyRelease(slot);
				e.consume(this);
			}
		}
		else if (noMods && e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS
		         && !module->isExclusive()) {
			createContextMenu();
			e.consume(this);
		}
	}

	void createContextMenu() {
		ui::Menu* menu = createMenu();
		KeyConfig& k = module->keys[slot];
		std::string label = module->keyLabel(slot);

		menu->addChild(createMenuLabel(std::string("Tecla [") + label + "]"));

		menu->addChild(createSubmenuItem("Button", k.isFader ? "" : (k.toggle ? "Toggle" : "Momentary"),
		[this](ui::Menu* m) {
			m->addChild(createBoolMenuItem("Toggle", "",
				[this]() { KeyConfig& k = module->keys[slot]; return !k.isFader && k.toggle; },
				[this](bool) { KeyConfig& k = module->keys[slot]; k.isFader = false; k.toggle = true; }
			));
			m->addChild(createBoolMenuItem("Momentary", "",
				[this]() { KeyConfig& k = module->keys[slot]; return !k.isFader && !k.toggle; },
				[this](bool) { KeyConfig& k = module->keys[slot]; k.isFader = false; k.toggle = false; }
			));
			m->addChild(new ui::MenuSeparator);
			m->addChild(createMenuItem("Map...", "", [this]() {
				KeyConfig& k = module->keys[slot];
				k.isFader = false;
				k.morse = false;
				k.random = false;
				startLearnFlow(module, slot);
			}));
			m->addChild(createMenuItem("Unmap", "", [this]() {
				module->unbind(slot);
			}, !module->keys[slot].bound));
		}));

		menu->addChild(createSubmenuItem("Fader/Knob", k.random ? "?" : (k.morse ? "~" : (k.isFader ? "" : "")),
		[this](ui::Menu* m) {
			KeyConfig& k = module->keys[slot];
			m->addChild(createBoolMenuItem("Morse", "",
				[this]() { return module->keys[slot].morse; },
				[this](bool) {
					KeyConfig& k = module->keys[slot];
					k.morse = !k.morse;
					if (k.morse) {
						k.isFader = true; // Morse es un control de parámetro
						k.random = false; // Morse y Random son excluyentes
					}
				}
			));
			m->addChild(createBoolMenuItem("Random", "",
				[this]() { return module->keys[slot].random; },
				[this](bool) {
					KeyConfig& k = module->keys[slot];
					k.random = !k.random;
					if (k.random) {
						k.isFader = true; // Random es un control de parámetro
						k.morse = false; // Morse y Random son excluyentes
					}
				}
			));
			if (k.morse)
				m->addChild(createMenuLabel("Pasos de 1%: corto sube, largo baja"));
			if (k.random)
				m->addChild(createMenuLabel("Random: valor al azar por pulsación"));
			ui::Slider* sHmax = new ui::Slider;
			FloatQuantity* qHmax = new FloatQuantity(&k.hMax, "Height m\xC3\xA1ximo", true, false);
			qHmax->other = &k.hMin;
			qHmax->onChange = [&k]() { k.isFader = true; };
			qHmax->disabled = k.morse || k.random;
			sHmax->quantity = qHmax;
			sHmax->box.size.x = 160.f;
			m->addChild(sHmax);
			ui::Slider* sHmin = new ui::Slider;
			FloatQuantity* qHmin = new FloatQuantity(&k.hMin, "Height m\xC3\xADnimo", true, false);
			qHmin->other = &k.hMax;
			qHmin->isMinBound = true;
			qHmin->floorV = 0.f; // El mínimo nunca baja de 0% (evita faders lentos)
			qHmin->onChange = [&k]() { k.isFader = true; };
			qHmin->disabled = k.morse || k.random;
			sHmin->quantity = qHmin;
			sHmin->box.size.x = 160.f;
			m->addChild(sHmin);
			ui::Slider* sVel = new ui::Slider;
			FloatQuantity* qVel = new FloatQuantity(&k.velTime, "Velocity", false, true);
			qVel->onChange = [&k]() { k.isFader = true; };
			qVel->disabled = k.morse || k.random;
			sVel->quantity = qVel;
			sVel->box.size.x = 160.f;
			m->addChild(sVel);
			// Rango del modo Random (deshabilitado si no es Random)
			ui::Slider* sRmin = new ui::Slider;
			FloatQuantity* qRmin = new FloatQuantity(&k.randMin, "Random m\xC3\xADn", true, false);
			qRmin->other = &k.randMax;
			qRmin->isMinBound = true;
			qRmin->floorV = 0.f;
			qRmin->onChange = [&k]() { k.random = true; };
			qRmin->disabled = !k.random;
			sRmin->quantity = qRmin;
			sRmin->box.size.x = 160.f;
			m->addChild(sRmin);
			ui::Slider* sRmax = new ui::Slider;
			FloatQuantity* qRmax = new FloatQuantity(&k.randMax, "Random m\xC3\xA1x", true, false);
			qRmax->other = &k.randMin;
			qRmax->floorV = 0.f;
			qRmax->onChange = [&k]() { k.random = true; };
			qRmax->disabled = !k.random;
			sRmax->quantity = qRmax;
			sRmax->box.size.x = 160.f;
			m->addChild(sRmax);
			m->addChild(new ui::MenuSeparator);
			m->addChild(createMenuItem("Map...", "", [this]() {
				module->keys[slot].isFader = true;
				startLearnFlow(module, slot);
			}));
			m->addChild(createMenuItem("Unmap", "", [this]() {
				module->unbind(slot);
			}, !module->keys[slot].bound));
		}, module->pendingMapSlot == slot));

		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuItem("Copy settings", "", [this]() {
			copyKeySettings(module->keys[slot]);
		}));
		menu->addChild(createMenuItem("Paste settings", "", [this]() {
			pasteKeySettings(module->keys[slot]);
		}, !g_hasClip));


	}
};

static void startLearnFlow(WizarKeyboardModule* module, int slot) {
	APP->scene->addChild(new LearnOverlay(module, slot));
}

// ============================================================================
// Agarre de expansión (arrastrar hacia la derecha)
// ============================================================================

static void wizarApplyUnits(WizarKeyboardWidget* w); // definida al final del archivo

struct DragGripWidget : widget::OpaqueWidget {
	WizarKeyboardModule* module;
	WizarKeyboardWidget* widget;
	float startX = 0.f;
	int startUnits = 1;

	DragGripWidget(WizarKeyboardModule* module, WizarKeyboardWidget* widget)
		: module(module), widget(widget) {
	}

	void step() override {
		float right = module->collapsed ? COLLAPSED_W : module->units * UNIT_W;
		box.pos = math::Vec(right - 13.f, 60.f);
		box.size = math::Vec(13.f, PANEL_H - 120.f);
		OpaqueWidget::step();
	}

	void draw(const DrawArgs& args) override {
		// Agarre invisible: el borde derecho se arrastra para ampliar/colapsar
		OpaqueWidget::draw(args);
	}

	void onButton(const ButtonEvent& e) override {
		// Igual que SlotWidget: el duplicado delega al padre (no consume, no
		// elimina desde aquí) para no corromper el árbol de eventos.
		if (isWizarDuplicate(module))
			return;
		// Preview del navegador: sin interacción
		if (!wizarInEngine(module))
			return;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			startX = 0.f;
			startUnits = module->units;
			e.consume(this);
		}
	}

	void onDragStart(const DragStartEvent& e) override {
		e.consume(this);
	}

	void onDragMove(const DragMoveEvent& e) override {
		// Preview del navegador: sin arrastre
		if (!wizarInEngine(module))
			return;
		startX += e.mouseDelta.x;
		if (module->collapsed) {
			// Expandiendo: arrastrar a la derecha despliega de nuevo
			if (startX > COLLAPSED_W * 0.6f) {
				module->collapsed = false;
				wizarApplyUnits(widget);
				startUnits = module->units;
				startX = 0.f;
			}
			return;
		}
		int newUnits = startUnits + (int) std::round(startX / UNIT_W);
		if (newUnits < 1) {
			module->collapsed = true;
			wizarApplyUnits(widget);
		} else if (newUnits != module->units) {
			requestSetUnits(widget, clamp(newUnits, 1, MAX_UNITS));
		}
	}
};

// ============================================================================
// Etiquetas del panel (Rack no renderiza <text> en el SVG)
// ============================================================================

	struct PanelLabelsWidget : widget::TransparentWidget {
		WizarKeyboardModule* module;

		PanelLabelsWidget(WizarKeyboardModule* m) : module(m) {
		}

			void draw(const DrawArgs& args) override {
				std::shared_ptr<window::Font> font = loadUiFont();
				if (!fontOk(font) || (module && module->collapsed))
					return;
		// Nombre del módulo
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFontSize(args.vg, 13);
		nvgFillColor(args.vg, nvgRGB(0xdf, 0xe9, 0xf5));
		nvgText(args.vg, 90.f, 16.f, "WIZAR MATRIX", NULL);
		// Etiqueta CONTROL bajo el botón luminoso
		nvgFontSize(args.vg, 7);
		nvgFillColor(args.vg, nvgRGB(0x9f, 0xb3, 0xc8));
		nvgText(args.vg, 26.f, 60.f, "CONTROL", NULL);
	}
};

// ============================================================================
// Logo (imagen PNG del panel)
// ============================================================================

struct WizarLogoWidget : widget::TransparentWidget {
	// El logo se carga en draw() (no como miembro): el caché de la ventana
	// libera el Image de forma segura al cerrar. Si lo guardáramos como
	// miembro, su destructor correría tras destruirse la ventana/GL y haría
	// SIGSEGV al liberar la textura.
	std::string path = asset::plugin(pluginInstance, "res/Logo.png");

	void draw(const DrawArgs& args) override {
		if (!APP || !APP->window)
			return;
		std::shared_ptr<window::Image> img = APP->window->loadImage(path);
		if (!img || img->handle < 0)
			return;
		int w = 0, h = 0;
		nvgImageSize(APP->window->vg, img->handle, &w, &h);
		if (box.size.x <= 0.f && w > 0)
			box.size = math::Vec(w, h);
		NVGpaint pat = nvgImagePattern(args.vg, 0, 0, w, h, 0, img->handle, 1.f);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, w, h);
		nvgFillPaint(args.vg, pat);
		nvgFill(args.vg);
	}
};

	// Conmutador CONTROL decorativo para el preview del navegador (module == NULL).
	// Se dibuja ENCENDIDO con el verde auténtico de Rack y centrado en (26,38),
	// igual que el componente real, para que la miniatura quede idéntica.
	struct PreviewControlWidget : widget::OpaqueWidget {
		void draw(const DrawArgs& args) override {
			Vec c = box.size.div(2);
			// bisel oscuro
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, 11.f);
			nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x14));
			nvgFill(args.vg);
			nvgStrokeColor(args.vg, nvgRGB(0x00, 0x00, 0x00));
			nvgStrokeWidth(args.vg, 1.0f);
			nvgStroke(args.vg);
			// halo verde
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, 9.f);
			nvgFillColor(args.vg, nvgRGBA(0x90, 0xc7, 0x3e, 60));
			nvgFill(args.vg);
			// luz verde auténtica (SCHEME_GREEN)
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, 5.5f);
			nvgFillColor(args.vg, nvgRGB(0x90, 0xc7, 0x3e));
			nvgFill(args.vg);
		}
	};

// ============================================================================
// Widget principal
// ============================================================================

	struct WizarKeyboardWidget : ModuleWidget {
		widget::TransformWidget* logoWidget = NULL;
		float lastControl = 0.f;
		bool killFlag = false;

		void onShow(const ShowEvent& e) override {
			ModuleWidget::onShow(e);
		}

		void onButton(const ButtonEvent& e) override {
			if (killFlag) {
				if (e.action == GLFW_PRESS)
					removeAction();
				killFlag = false;
				return;
			}
			ModuleWidget::onButton(e);
		}

	WizarKeyboardWidget(WizarKeyboardModule* module) : ModuleWidget() {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/WizarKeyboard.svg")));

		// El navegador de Rack crea el widget con module == NULL para el
		// preview. Las capas VISUALES (teclas, nombre, logo) se añaden SIEMPRE
		// para que la miniatura muestre las letras, "WIZAR MATRIX" y el logo.
		// Lo que toca el motor (param/light, agarre, registro global) solo con
		// módulo real; en el preview se dibuja un conmutador CONTROL decorativo
		// encendido.

		// El navegador crea previsualizaciones con module == NULL; por eso
		// las capas VISUALES (teclas, nombre, logo) se añaden SIEMPRE para que
		// la miniatura muestre las letras, "WIZAR MATRIX" y el logo. Lo que
		// toca el motor (param/light, agarre, registro global) solo con módulo real.

		// Teclas (se autoocultan si la unidad no está activa o está colapsado)
		for (int u = 0; u < MAX_UNITS; u++)
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) {
					int k = physKey(r, u * 4 + c);
					if (k == -1)
						continue;
					addChild(new SlotWidget(module, u, r, c));
				}

		// Logo (PNG, debajo de las teclas)
		{
			logoWidget = new widget::TransformWidget;
			logoWidget->scale(math::Vec(0.18f, 0.18f));
			logoWidget->addChild(new WizarLogoWidget());
			logoWidget->box.pos = math::Vec(90.f - 260.f * 0.18f / 2.f, 328.f);
			addChild(logoWidget);
		}

		// Etiquetas del panel (nombre, CONTROL) dibujadas por código
		addChild(new PanelLabelsWidget(module));

		if (module) {
			// Botón luminoso de modo control (estilo VocalLamma)
			addParam(createParamCentered<VCVBezelLatch>(Vec(26, 38), module, CONTROL_PARAM));
			addChild(createLightCentered<VCVBezelLight<GreenLight>>(Vec(26, 38), module, CONTROL_LIGHT));
			// Agarre de expansión (invisible: se arrastra el borde derecho)
			addChild(new DragGripWidget(module, this));
			// Solo registramos en el registro global si el módulo está en el
			// motor (el módulo temporal del preview NO debe registrarse).
			if (APP && APP->engine && APP->engine->getModule(module->id))
				g_modules.push_back(module);
		} else {
			// Preview del navegador: conmutador CONTROL decorativo ENCENDIDO,
			// centrado en (26,38) como el real.
			PreviewControlWidget* pcw = new PreviewControlWidget();
			pcw->box.pos = Vec(15, 27); // centro (26,38)
			pcw->box.size = Vec(22, 22);
			addChild(pcw);
		}

		// Capa de badges
		if (!g_badges) {
			g_badges = new BadgeOverlay();
			APP->scene->rack->addChild(g_badges);
		}

		applyUnits();
	}

		~WizarKeyboardWidget() {
			WizarKeyboardModule* m = getWizarModule();
			hookRemove(m);
			g_modules.erase(std::remove(g_modules.begin(), g_modules.end(), m), g_modules.end());
			if (g_modules.empty() && g_badges) {
				if (g_badges->parent)
					g_badges->parent->removeChild(g_badges);
				g_badges->requestDelete();
				g_badges = NULL;
			}
		}

	WizarKeyboardModule* getWizarModule() {
		return dynamic_cast<WizarKeyboardModule*>(getModule());
	}

	void setUnits(int units) {
		WizarKeyboardModule* m = getWizarModule();
		if (!m)
			return;
		m->units = units;
		applyUnits();
	}

	void applyUnits() {
		WizarKeyboardModule* m = getWizarModule();
		int units = m ? m->units : 1;
		bool col = m ? m->collapsed : false;
		box.size.x = col ? COLLAPSED_W : units * UNIT_W;
		box.size.y = PANEL_H;
	}

	/** Pinta fondo, marco y relleno de TODAS las teclas (una sola fuente de
	    verdad, evita el estiramiento del SVG y las líneas residuales). */
	void draw(const DrawArgs& args) override {
		WizarKeyboardModule* m = getWizarModule();
		int units = m ? m->units : 1;
		bool collapsed = m && m->collapsed;
		NVGpaint bg = nvgLinearGradient(args.vg, 0, 0, 0, PANEL_H,
			nvgRGB(0x1a, 0x1a, 0x24), nvgRGB(0x0d, 0x0d, 0x12));
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, PANEL_H);
		nvgFillPaint(args.vg, bg);
		nvgFill(args.vg);

		if (!collapsed) {
		for (int u = 0; u < units; u++) {
			float x0 = u * UNIT_W;
			// Relleno de teclas (gradiente azul por tecla)
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++) {
					if (physKey(r, u * 4 + c) == -1)
						continue;
					int kslot = u * 16 + r * 4 + c;
					bool mapped = m && m->keys[kslot].bound;
					float kx = x0 + KEY_X0 + c * PX;
					float ky = KEY_Y0 + r * PY;
					// Tecla mapeada: rojo; sin mapear: azul cristalino
					NVGpaint kg = mapped
						? nvgLinearGradient(args.vg, kx, ky, kx, ky + KH,
							nvgRGB(0xff, 0xd0, 0xd0), nvgRGB(0xe0, 0x5f, 0x5f))
						: nvgLinearGradient(args.vg, kx, ky, kx, ky + KH,
							nvgRGB(0xcd, 0xea, 0xff), nvgRGB(0x5f, 0xa8, 0xe0));
					nvgBeginPath(args.vg);
					nvgRoundedRect(args.vg, kx, ky, KW, KH, 4);
					nvgFillPaint(args.vg, kg);
					nvgFill(args.vg);
					nvgStrokeColor(args.vg, mapped ? nvgRGB(0xa0, 0x2a, 0x2a)
					                                  : nvgRGB(0x2a, 0x6a, 0xa0));
					nvgStrokeWidth(args.vg, 0.75);
					nvgStroke(args.vg);
				}
			// Separador recto entre unidades (el borde exterior redondeado
			// lo pinta el código más abajo, igual que el módulo 1).
			if (u >= 1) {
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, x0, 4.f);
				nvgLineTo(args.vg, x0, PANEL_H - 4.f);
				nvgStrokeColor(args.vg, nvgRGB(0x3a, 0x3a, 0x4a));
				nvgStrokeWidth(args.vg, 1.5);
				nvgStroke(args.vg);
				std::shared_ptr<window::Font> font = loadUiFont();
				if (fontOk(font)) {
					nvgFontFaceId(args.vg, font->handle);
					nvgFontSize(args.vg, 9);
					nvgFillColor(args.vg, nvgRGB(0x88, 0x88, 0xa0));
					nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
					nvgBeginPath(args.vg);
					std::string t = "EXT ";
					t += std::to_string(u + 1);
					nvgText(args.vg, x0 + UNIT_W - 12.f, 60.f, t.c_str(), NULL);
				}
			}
		}
		}
		// Borde exterior redondeado para TODO el módulo (todas las unidades y
		// también colapsado), con margen de 4px como el SVG original, para que
		// la esquina derecha siempre exista y haya respiración respecto al borde.
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 4.f, 4.f, box.size.x - 8.f, PANEL_H - 8.f, 6.f);
		nvgStrokeColor(args.vg, nvgRGB(0x3a, 0x3a, 0x4a));
		nvgStrokeWidth(args.vg, 1.5f);
		nvgStroke(args.vg);
		ModuleWidget::draw(args);

		// Marca visual del duplicado (se autoelimina al clicar): borde
		// naranja para que quede claro cuál instancia va a desaparecer.
		if (isWizarDuplicate(m)) {
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, 1.5f, 1.5f, box.size.x - 3.f, box.size.y - 3.f, 6.f);
			nvgStrokeColor(args.vg, nvgRGB(0xff, 0x8c, 0x1a));
			nvgStrokeWidth(args.vg, 3.f);
			nvgStroke(args.vg);
		}
	}

	/** Captura por hover cuando el modo control está apagado. */
		void onHoverKey(const HoverKeyEvent& e) override {
			if (killFlag) {
				if (e.action == GLFW_PRESS)
					removeAction();
				killFlag = false;
				return;
			}
			WizarKeyboardModule* m = getWizarModule();
			// Preview del navegador: sin procesar teclas
			if (!wizarInEngine(m)) {
				ModuleWidget::onHoverKey(e);
				return;
			}
			if (m && m->isExclusive() && (e.mods & RACK_MOD_MASK) == 0) {
			int slot = slotForKeyGlfw(e.key);
			if (slot >= 0 && m->physKeyValid(slot)
			    && (e.action == GLFW_PRESS || e.action == GLFW_RELEASE)) {
				if (e.action == GLFW_PRESS)
					m->keyPress(slot);
				else
					m->keyRelease(slot);
				e.consume(this);
				return;
			}
		}
		ModuleWidget::onHoverKey(e);
	}

	void step() override {
		WizarKeyboardModule* m = getWizarModule();
		// Preview del navegador (módulo temporal, fuera del motor): no ejecutar
		// la lógica de gancho/refresh; solo el paso base.
		if (m && !wizarInEngine(m)) {
			ModuleWidget::step();
			return;
		}
		if (m) {
			// Singleton: solo una instancia (un solo teclado físico y un
			// único gancho GLFW global para el modo CONTROL). Se conserva la
			// instancia de menor id; las demás son duplicados marcados para
			// eliminación. Un duplicado NUNCA debe poseer el gancho (si lo
			// instaló, lo libera para que la instancia principal lo recupere).
			bool duplicate = isWizarDuplicate(m);
			killFlag = duplicate;
			if (duplicate) {
				hookRemove(m);
			}
			else {
				// (Re)instala el gancho si CONTROL está ON. Es idempotente y
				// además recupera el gancho tras borrar un duplicado que lo
				// sustrajo, aunque el estado de CONTROL no haya cambiado.
				if (m->params[CONTROL_PARAM].getValue() > 0.5f)
					hookInstall(m);
				else
					hookRemove(m);
				lastControl = m->params[CONTROL_PARAM].getValue();
			}
			float w = m->collapsed ? COLLAPSED_W : m->units * UNIT_W;
			if (std::fabs(box.size.x - w) > 0.5f)
				applyUnits();
			if (logoWidget)
				logoWidget->visible = !m->collapsed;
			static int tgtFrame = 0;
			if (++tgtFrame >= 15) {
				tgtFrame = 0;
				m->refreshTargets();
			}
		}
		ModuleWidget::step();
	}

	void appendContextMenu(ui::Menu* menu) override {
		WizarKeyboardModule* m = getWizarModule();
		if (!m)
			return;

		menu->addChild(new ui::MenuSeparator);

		// Distribución de teclado
		menu->addChild(createSubmenuItem("Distribuci\xC3\xB3n", LAYOUT_NAMES[m->layoutIdx],
		[m](ui::Menu* sub) {
			for (int i = 0; i < 3; i++) {
				sub->addChild(createBoolMenuItem(LAYOUT_NAMES[i], "",
					[m, i]() { return m->layoutIdx == i; },
					[m, i](bool) { m->layoutIdx = i; }
				));
			}
		}));

		// Unidades
		menu->addChild(createSubmenuItem("Unidades", std::to_string(m->units),
		[this](ui::Menu* sub) {
			for (int i = 1; i <= MAX_UNITS; i++) {
				sub->addChild(createBoolMenuItem(std::to_string(i), "",
					[this, i]() { return getWizarModule()->units == i; },
					[this, i](bool) { setUnits(i); }
				));
			}
		}));

		// Colapsar / Expandir
		menu->addChild(createBoolMenuItem(m->collapsed ? "Expandir" : "Colapsar", "",
			[m]() { return m->collapsed; },
			[this, m](bool) { m->collapsed = !m->collapsed; applyUnits(); }
		));

		// Umbral Morse (ms)
		{
			ui::Slider* sM = new ui::Slider;
			FloatQuantity* qM = new FloatQuantity(&m->morseThresholdMs, "Umbral Morse (ms)", false, false);
			qM->ms = true;
			qM->minV = 50.f;
			qM->maxV = 1500.f;
			sM->quantity = qM;
			sM->box.size.x = 200.f;
			menu->addChild(sM);
		}

		menu->addChild(new ui::MenuSeparator);

		menu->addChild(createMenuItem("Guardar preset...", "", [this]() {
			savePresetDialog();
		}));

		menu->addChild(createMenuItem("Cargar preset...", "", [this]() {
			loadPresetDialog();
		}));
	}

	std::string presetDir() {
		return asset::plugin(pluginInstance, "");
	}

	void savePresetDialog() {
		std::string dir = presetDir();
		osdialog_filters* filters = osdialog_filters_parse("Preset Wizar:*.json");
		char* pathC = osdialog_file(OSDIALOG_SAVE, dir.c_str(), "wizar-preset.json", filters);
		osdialog_filters_free(filters);
		if (!pathC)
			return;
		std::string path = pathC;
		free(pathC);

		json_t* rootJ = getWizarModule()->dataToJson();
		FILE* f = fopen(path.c_str(), "w");
		if (f) {
			json_dumpf(rootJ, f, JSON_INDENT(2));
			fclose(f);
		}
		json_decref(rootJ);
	}

	void loadPresetDialog() {
		std::string dir = presetDir();
		osdialog_filters* filters = osdialog_filters_parse("Preset Wizar:*.json");
		char* pathC = osdialog_file(OSDIALOG_OPEN, dir.c_str(), "", filters);
		osdialog_filters_free(filters);
		if (!pathC)
			return;
		std::string path = pathC;
		free(pathC);

		json_error_t err;
		json_t* rootJ = json_load_file(path.c_str(), 0, &err);
		if (!rootJ)
			return;
		getWizarModule()->dataFromJson(rootJ);
		json_decref(rootJ);
		applyUnits();
	}
};

static void requestSetUnits(WizarKeyboardWidget* w, int units);

// Creación del modelo tras definir el Widget completo
Model* modelWizarKeyboard = createModel<WizarKeyboardModule, WizarKeyboardWidget>("wizarkeyboard");

static void requestSetUnits(WizarKeyboardWidget* w, int units) {
	w->setUnits(units);
}

static void wizarApplyUnits(WizarKeyboardWidget* w) {
	if (w)
		w->applyUnits();
}

// --- Función init requerida por VCV Rack ---

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelVocalLamma);
	p->addModel(modelBalatube);
	p->addModel(modelWizarKeyboard);
}
