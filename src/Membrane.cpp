#include "plugin.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

// Membrane — taiko drum: large cylinder with bulged center, patches both ends.
// 8 modes spanning low-mid range, dual sub-octave, click attack.
// Panel 6HP, stereo out (L+R = mono mix), 2 voices.

struct Membrane : Module {
	enum ParamIds {
		TUNE_PARAM,
		DECAY_PARAM,
		LEVEL_PARAM,
		MATERIAL_PARAM,
		CHOKE_PARAM,
		DRIVE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		VOCT_INPUT,
		VELOCITY_INPUT,
		DECAY_CV_INPUT,
		MATERIAL_CV_INPUT,
		DRIVE_CV_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		OUT_RIGHT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CHOKE_LIGHT,
		NUM_LIGHTS
	};

	static constexpr int NUM_MODES = 8;
	// Tablas por zona: Taiko (cilindro, cuero) → Djembe (goblet, una membrana) → Darbuka (plástico)
	static constexpr float LEATHER_RATIO[] = {1.00f, 1.25f, 1.75f, 2.50f, 4.00f, 6.50f, 10.00f, 15.00f};
	static constexpr float DJEMBE_RATIO[]  = {1.00f, 1.52f, 2.05f, 3.10f, 4.60f, 6.90f, 10.50f, 15.50f};
	// Darbuka: parciales reales medidos (705/1020/1305/1585/1860/2346/3808Hz), inarmónicos, espaciado uniforme ~285Hz
	static constexpr float PLASTIC_RATIO[] = {1.00f, 5.40f, 7.80f, 10.00f, 12.10f, 14.25f, 18.00f, 29.00f};
	// Gain por zona (normalizados por modeSum)
	static constexpr float LEATHER_GAIN[]  = {1.00f, 0.35f, 0.30f, 0.25f, 0.20f, 0.08f, 0.025f, 0.010f};
	static constexpr float DJEMBE_GAIN[]   = {0.80f, 0.12f, 0.12f, 0.20f, 0.20f, 0.32f, 0.050f, 0.020f};
	// Darbuka: parciales medidos; mode1 (705Hz) ataca, fundamental (125Hz) persiste
	static constexpr float PLASTIC_GAIN[]  = {0.55f, 0.90f, 0.55f, 0.45f, 0.40f, 0.25f, 0.12f, 0.05f};
	// Decay por zona (T60 relativo, x baseT60)
	static constexpr float LEATHER_DECAY[] = {0.90f, 1.00f, 1.00f, 0.90f, 0.55f, 0.45f, 0.12f, 0.07f};
	static constexpr float DJEMBE_DECAY[]  = {0.45f, 0.45f, 0.50f, 0.40f, 0.28f, 0.18f, 0.08f, 0.04f};
	// Darbuka: fundamental con ring, carcasa solo en punch
	static constexpr float PLASTIC_DECAY[] = {0.26f, 0.20f, 0.22f, 0.22f, 0.26f, 0.24f, 0.10f, 0.06f};

	struct DrumVoice {
		float envA[NUM_MODES] = {};
		float envR[NUM_MODES] = {};
		float atkR = 0.1f;
		float phaseA[NUM_MODES] = {};
		// Segunda membrana: parche opuesto acoplado por la caja de aire
		float envB[NUM_MODES] = {};
		float phaseB[NUM_MODES] = {};
		float symLp = 0.f;
		float subEnv = 0.f;
		float subPhase = 0.f;
		float sub2Env = 0.f;
		float sub2Phase = 0.f;
		// Slap de palma: racimo 2-4.5kHz, muere en ~35ms (solo djembe)
		float slapEnv = 0.f;
		float slapPh[7] = {};
		float noiseState = 0.f;
		float clickLp = 0.f;
		float hpState = 0.f;
		float pitchMul = 2.0f;
		int exciteLeft = 0;
		int samplesSinceTrig = 0;
		uint32_t rng = 33333;

		void trigger(int burst, float material = 0.f) {
			exciteLeft = burst;
			samplesSinceTrig = 0;
			// Djembe: la mano excita todos los modos en fase (pico siempre al inicio)
			float djA = 1.f - fabsf(material - 0.5f) * 2.f; if (djA < 0.f) djA = 0.f;
			bool coherent = djA > 0.5f;
			for (int i = 0; i < NUM_MODES; i++) {
				envA[i] = 1.0f;
				envR[i] = 1.0f;
				// Fases aleatorias: cada golpe es único (decoherencia, sin slam sistemático)
				phaseA[i] = coherent ? 1.5708f : rand01() * 6.2831853f;
				// La membrana opuesta recibe el golpe a través del aire: más suave
				envB[i] = 0.7f;
				phaseB[i] = coherent ? 1.5708f : rand01() * 6.2831853f;
			}
			subEnv = 0.80f;
			subPhase = coherent ? 1.5708f : rand01() * 6.2831853f;
			sub2Env = 0.80f;
			sub2Phase = coherent ? 1.5708f : rand01() * 6.2831853f;
			slapEnv = 1.0f;
			for (int k = 0; k < 7; k++) slapPh[k] = 1.5708f;
			noiseState = 0.f;
			clickLp = 0.f;
			hpState = 0.f;
			pitchMul = 2.0f;
			symLp = 0.f;
			// Djembe: sin swell (ataque directo, sin subida intermedia)
			float djA2 = 1.f - fabsf(material - 0.5f) * 2.f; if (djA2 < 0.f) djA2 = 0.f;
			atkR = djA2 > 0.5f ? 1.0f : 0.065f;
		}

		float noise() {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			return ((float)(rng & 0xFFFFFF) / 16777215.f) * 2.f - 1.f;
		}

		float rand01() {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			return (float)(rng & 0xFFFFFF) / 16777215.f;
		}

		void process(float freq, float baseT60, float sampleRate, float material, float &outL, float &outR, float decayMul = 1.f, float drive = 0.5f) {
			samplesSinceTrig++;
			// Cantidades de zona 0..1 (una sola vez por muestra)
			float djA = 1.f - fabsf(material - 0.5f) * 2.f; if (djA < 0.f) djA = 0.f;
			float darbukaB = fmaxf(0.f, (material - 0.5f) * 2.f);
			float taikoA = 1.f - clamp(material / 0.35f, 0.f, 1.f);

			// --- Click de ataque: 12ms ruido brillante, pasa-bajos (sin spikes) ---
			float click = 0.f;
			if (exciteLeft > 0) {
				float raw = noise();
				float hpf = raw - noiseState;
				// Darbuka: palma seca más brillante (HPF más alto)
				float hpCoef = 0.995f - 0.010f * darbukaB;
				noiseState = raw * (1.f - hpCoef) + noiseState * hpCoef;
				click = hpf * 2.0f;
				float cClk = 1.f - expf(-2.f * 3.14159265f * 4000.f / sampleRate);
				clickLp += cClk * (click - clickLp);
				// Darbuka conserva el transitorio sin suavizar
				if (material < 0.85f) click = clickLp;
				exciteLeft--;
			}

			// Pitch drop continuo x2.0 -> x1.0 (asienta rapido, cuerpo oscuro)
			float pitchT60 = baseT60 * 0.15f;
			float pitchR = powf(0.001f, 1.f / (pitchT60 * sampleRate));
			pitchMul = 1.0f + (pitchMul - 1.0f) * pitchR;
			if (pitchMul < 1.0f) pitchMul = 1.0f;
			// Darbuka: parciales metálicos estables, sin pitch-bend (el real no lo tiene)
			float pitchMulEff = 1.0f + (pitchMul - 1.0f) * (1.f - darbukaB) * (1.f - djA * 0.8f);

			// --- 8 modos × 2 pendientes: punch (transitorio) + ring (sustain con swell) ---
			float outMode = 0.f;
			float modeSum = 0.f;

			// Swell del ring (ralentiza la entrada del sustain; mas lento en darbuka)
			float swellTau = 0.04f * (1.f + darbukaB * 1.5f);
			float cR = 1.f - expf(-1.f / (swellTau * sampleRate));
			atkR += (1.f - atkR) * cR;


			for (int i = 0; i < NUM_MODES; i++) {
				float ratio, gain, decay;
				if (material < 0.5f) {
					float t = material * 2.f;
					ratio = LEATHER_RATIO[i] + (DJEMBE_RATIO[i] - LEATHER_RATIO[i]) * t;
					gain  = LEATHER_GAIN[i]  + (DJEMBE_GAIN[i]  - LEATHER_GAIN[i])  * t;
					decay = LEATHER_DECAY[i] + (DJEMBE_DECAY[i] - LEATHER_DECAY[i]) * t;
				} else {
					float t = (material - 0.5f) * 2.f;
					ratio = DJEMBE_RATIO[i] + (PLASTIC_RATIO[i] - DJEMBE_RATIO[i]) * t;
					gain  = DJEMBE_GAIN[i]  + (PLASTIC_GAIN[i]  - DJEMBE_GAIN[i])  * t;
					decay = DJEMBE_DECAY[i] + (PLASTIC_DECAY[i] - DJEMBE_DECAY[i]) * t;
				}

				float punchT60 = baseT60 * decay * 0.10f * decayMul;
				float ringT60 = baseT60 * decay * 1.50f * decayMul * (1.f - 0.58f * taikoA);
				// Darbuka: la carcasa suena ~50ms (textura del ataque), luego muere
				if (material > 0.5f && i > 0)
					punchT60 = baseT60 * decay * 0.35f * decayMul;

				float w = 2.f * M_PI * freq * ratio * pitchMulEff / sampleRate;

				envA[i] *= powf(0.001f, 1.f / (punchT60 * sampleRate));
				if (envA[i] < 1e-6f) envA[i] = 0.f;
				envR[i] *= powf(0.001f, 1.f / (ringT60 * sampleRate));
				if (envR[i] < 1e-6f) envR[i] = 0.f;
				phaseA[i] += w;
				if (phaseA[i] > 6.2831853f) phaseA[i] -= 6.2831853f;
				float punchW, ringW;
				if (material > 0.5f) {
					// Darbuka: fundamental (i=0) retardado y sostenido; parciales de carcasa (i>0) instantáneos y cortos
					if (i == 0) { punchW = 0.05f; ringW = 0.60f; }
					else        { punchW = 0.85f; ringW = 0.07f; }
				} else {
					punchW = 0.60f + djA * 0.40f;
					ringW  = 0.40f - djA * 0.25f;
					// Djembe: solo el fundamental sostiene; los parciales mueren con el punch (cola en seno puro)
					if (i > 0) ringW *= (1.f - djA * 0.87f);
				}
				float y = (envA[i] * punchW + envR[i] * atkR * ringW) * sinf(phaseA[i]);

				modeSum += gain;
				outMode += y * gain;
			}

			if (modeSum > 0.f) outMode /= modeSum;
			// Djembe: modos con onset 3ms (el slap/HF manda primero, pico tonal ~8ms)
			{
				if (djA > 0.5f) {
					float q = (float)samplesSinceTrig / (0.003f * sampleRate);
					if (q < 1.f) outMode *= 0.3f + 0.7f * (0.5f - 0.5f * cosf(3.14159265f * q));
				}
			}

			// --- Segunda membrana: mismo tono, mas sustain, rampa lenta (cuerpo) ---
			float outSym = 0.f;
			// Djembe (material ~0.5) = una sola membrana: simpática a cero en el centro
			float symGain = (material < 0.5f) ? (1.f - djA) * 0.15f : 0.f;
			for (int i = 0; i < NUM_MODES; i++) {
				float ratio, gain, decay;
				if (material < 0.5f) {
					float t = material * 2.f;
					ratio = LEATHER_RATIO[i] + (DJEMBE_RATIO[i] - LEATHER_RATIO[i]) * t;
					gain  = LEATHER_GAIN[i]  + (DJEMBE_GAIN[i]  - LEATHER_GAIN[i])  * t;
				decay = LEATHER_DECAY[i] + (DJEMBE_DECAY[i] - LEATHER_DECAY[i]) * t;
				} else {
					float t = (material - 0.5f) * 2.f;
					ratio = DJEMBE_RATIO[i] + (PLASTIC_RATIO[i] - DJEMBE_RATIO[i]) * t;
					gain  = DJEMBE_GAIN[i]  + (PLASTIC_GAIN[i]  - DJEMBE_GAIN[i])  * t;
					decay = DJEMBE_DECAY[i] + (PLASTIC_DECAY[i] - DJEMBE_DECAY[i]) * t;
				}

				// La simpática resuena larga en medios; el modo0 corto para no embarrar ≤100Hz
				float symMult = (i == 0) ? 1.2f : 2.5f;
				float symT60 = baseT60 * decay * 0.75f * symMult * decayMul * (1.f - 0.58f * taikoA);
				float w = 2.f * M_PI * freq * ratio * 1.000f * pitchMul / sampleRate;

				envB[i] *= powf(0.001f, 1.f / (symT60 * sampleRate));
				if (envB[i] < 1e-6f) envB[i] = 0.f;
				phaseB[i] += w;
				if (phaseB[i] > 6.2831853f) phaseB[i] -= 6.2831853f;
				if (symGain > 0.f) outSym += envB[i] * sinf(phaseB[i]) * gain;
			}

			if (modeSum > 0.f) outSym /= modeSum;
			outSym *= symGain;
			// El aire tarda ~3.6ms en empujar el parche opuesto: rampa de ataque
			float symRamp = samplesSinceTrig < 160 ? (float)samplesSinceTrig / 160.f : 1.f;
			outSym *= symRamp;
			// Parche lejano oído a través de la caja: pasa-bajos ~500Hz
			float symC = 1.f - expf(-2.f * 3.14159265f * 500.f / sampleRate);
			symLp += symC * (outSym - symLp);
			outSym = symLp;

			// --- Sub-octava 1 (×0.5): cuerpo grave fuerte ---
			float subFreq = freq * 0.5f;
			float subW = 2.f * M_PI * subFreq / sampleRate;
			float subT60 = 0.40f;
			float subR = powf(0.001f, 1.f / (subT60 * sampleRate));
			subEnv *= subR;
			if (subEnv < 1e-6f) subEnv = 0.f;
			subPhase += subW;
			if (subPhase > 6.2831853f) subPhase -= 6.2831853f;
			float subOut = (taikoA > 0.f) ? subEnv * sinf(subPhase) : 0.f;

			// --- Sub-octava 2 (×0.25): sub-bass ---
			float sub2Freq = freq * 0.25f;
			float sub2W = 2.f * M_PI * sub2Freq / sampleRate;
			float sub2T60 = 0.45f;
			float sub2R = powf(0.001f, 1.f / (sub2T60 * sampleRate));
			sub2Env *= sub2R;
			if (sub2Env < 1e-6f) sub2Env = 0.f;
			sub2Phase += sub2W;
			if (sub2Phase > 6.2831853f) sub2Phase -= 6.2831853f;
			float sub2Out = (taikoA > 0.f) ? sub2Env * sinf(sub2Phase) : 0.f;

			// --- Slap de palma: racimo 1.0-4.3kHz, T60 50ms, solo djembe ---
			float slapOut = 0.f;
			{
				if (djA > 0.f && slapEnv > 1e-6f) {
					static const float slapRatio[7] = {16.f, 20.f, 32.f, 39.f, 45.f, 57.f, 69.f};
					static const float slapGain[7] = {0.20f, 0.16f, 0.22f, 0.18f, 0.22f, 0.22f, 0.16f};
					slapEnv *= powf(0.001f, 1.f / (0.050f * sampleRate));
					for (int k = 0; k < 7; k++) {
						slapPh[k] += 2.f * 3.14159265f * freq * slapRatio[k] * pitchMulEff / sampleRate;
						if (slapPh[k] > 6.2831853f) slapPh[k] -= 6.2831853f;
						slapOut += sinf(slapPh[k]) * slapGain[k];
					}
					slapOut *= slapEnv * djA;
				}
			}

			// --- Mezcla (pesos; sub/slap/room se atenuan por zona) ---
			// El sub solo pertenece al taiko (parche grande); djembe y darbuka no llevan sub-bajo
			// Mano vs maza: el click de maza desaparece en djembe (mano) -> click suave de palma
			float clickGain = 0.020f * (1.f - djA * 0.80f);
			// Darbuka al tope: sin ruido (el ataque lo da el parcial tonal 705Hz)
			clickGain *= (1.f - darbukaB);
			float out = click * clickGain + outMode * 0.70f + outSym * 0.15f * (1.f - djA * 0.5f) + subOut * 0.09f * taikoA + sub2Out * 0.06f * taikoA + slapOut * 0.22f;
			// Imagen estéreo: membranas opuestas; (L+R)/2 = mezcla mono
			float diff = (outMode * 0.70f - outSym * 0.15f) * 0.25f;
			outL = out + diff;
			outR = out - diff;

			// Fade-in de ataque (maza 6ms, mano ~3ms, dedos ~2ms segun zona)
			float atkN = 0.006f * sampleRate * (1.f - fmaxf(darbukaB, djA * 0.8f) * 0.65f);
			if ((float)samplesSinceTrig < atkN) {
				float p = (float)samplesSinceTrig / atkN;
				float fade = 0.5f - 0.5f * cosf(3.14159265f * p);
				outL *= fade;
				outR *= fade;
			}

			// Harm: base sin barro (drive topado a 0.5) + excitador de agudos por encima
			float baseDrive = fminf(drive, 0.5f);
			float comp = 0.8f * sqrtf(0.5f / fmaxf(baseDrive, 0.05f));
			outL = tanhf(outL * baseDrive) * comp;
			outR = tanhf(outR * baseDrive) * comp;
			float excAmt = fmaxf(0.f, drive - 0.5f);
			if (excAmt > 0.f) {
				float hpC = 1.f - expf(-2.f * 3.14159265f * 1200.f / sampleRate);
				hpState += hpC * ((outL + outR) * 0.5f - hpState);
				float hiL = outL - hpState, hiR = outR - hpState;
				float eg = excAmt * 0.35f;
				outL += tanhf(hiL * (1.f + excAmt * 2.f)) * eg;
				outR += tanhf(hiR * (1.f + excAmt * 2.f)) * eg;
			}

			// Gate seguridad
			if (samplesSinceTrig > (int)(baseT60 * decayMul * 4.f * sampleRate)) {
				outL = 0.f;
				outR = 0.f;
			}
		}
	};

	DrumVoice voices[2];
	int lastVoice = 0;
	int trigCount = 0;
	dsp::SchmittTrigger trig[2];
	float limEnv[2] = {0.f, 0.f};

	// --- Room compartida: dos peines con damping (sala pequeña) ---
	std::vector<float> roomA, roomB;
	int roomPos = 0;
	int roomSR = 0;
	float roomLpA = 0.f, roomLpB = 0.f;
	float roomDampC = 0.3f;

	void roomProcess(float x, float sampleRate, float &wetL, float &wetR) {
		int sr = (int)sampleRate;
		if (sr != roomSR || roomA.empty()) {
			roomSR = sr;
			int N = (int)(0.2f * sampleRate) + 8;
			roomA.assign(N, 0.f);
			roomB.assign(N, 0.f);
			roomPos = 0;
			roomLpA = roomLpB = 0.f;
			roomDampC = 1.f - expf(-2.f * (float)M_PI * 2800.f / sampleRate);
		}
		int N = (int)roomA.size();
		int dA = (int)(0.009f * sampleRate);
		int dB = (int)(0.013f * sampleRate);
		if (dA >= N) dA = N - 1;
		if (dB >= N) dB = N - 1;
		float tapA = roomA[(roomPos - dA + N) % N];
		float tapB = roomB[(roomPos - dB + N) % N];
		roomLpA += roomDampC * (tapA - roomLpA);
		roomLpB += roomDampC * (tapB - roomLpB);
		roomA[roomPos] = x + 0.50f * roomLpA;
		roomB[roomPos] = x + 0.50f * roomLpB;
		roomPos++;
		if (roomPos >= N) roomPos = 0;
		// Peine A -> L, peine B -> R; djembe atenúa room (caja pequeña)
		// Se escala fuera, aquí deja el wet base
		wetL = tapA * 0.10f;
		wetR = tapB * 0.10f;
	}

	float getDjembeAmt(float material) {
		float a = 1.f - fabsf(material - 0.5f) * 2.f;
		return a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
	}

	Membrane() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(TUNE_PARAM, -24.f, 24.f, -12.f, "Tune", " semitones");
		configParam(DECAY_PARAM, 0.05f, 4.f, 2.0f, "Decay", " s");
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.75f, "Level");
		configParam(MATERIAL_PARAM, 0.f, 1.f, 0.f, "Material Leatherx2<>Leatherx1<>Plastic");
		configParam(CHOKE_PARAM, 0.f, 1.f, 0.f, "Choke");
		configParam(DRIVE_PARAM, 0.f, 2.f, 0.5f, "Harmonics");
		configInput(TRIG_INPUT, "Gate");
		configInput(VOCT_INPUT, "V/Oct");
		configInput(VELOCITY_INPUT, "Velocity");
		configInput(DECAY_CV_INPUT, "Decay CV");
		configInput(MATERIAL_CV_INPUT, "Material CV");
		configInput(DRIVE_CV_INPUT, "Harm CV");
		configOutput(OUT_OUTPUT, "Left");
		configOutput(OUT_RIGHT, "Right");
	}

	void process(const ProcessArgs& args) override {
		bool choke = params[CHOKE_PARAM].getValue() > 0.5f;
		lights[CHOKE_LIGHT].setBrightness(choke ? 1.f : 0.f);
		float outMixL = 0.f;
		float outMixR = 0.f;
		int maxChan = 1;
		for (int id : {TRIG_INPUT, VOCT_INPUT, VELOCITY_INPUT}) maxChan = std::max(maxChan, inputs[id].getChannels());
		if (maxChan > 2) maxChan = 2;

		for (int c = 0; c < maxChan; c++) {
			// Lecturas polifónicas seguras: nunca leer un canal que el puerto no tenga
			float trigVal = inputs[TRIG_INPUT].getPolyVoltage(c);
			bool doTrig = trig[c].process(rescale(trigVal, 0.01f, 1.f, 0.f, 1.f));
			float voct = inputs[VOCT_INPUT].getNormalPolyVoltage(0.f, c);
			float harm = clamp(params[DRIVE_PARAM].getValue() + inputs[DRIVE_CV_INPUT].getNormalPolyVoltage(0.f, c) / 5.f, 0.f, 2.f);
			float material = clamp(params[MATERIAL_PARAM].getValue() + inputs[MATERIAL_CV_INPUT].getNormalPolyVoltage(0.f, c) / 3.f, 0.f, 1.f);
			float baseT60 = clamp(params[DECAY_PARAM].getValue() + inputs[DECAY_CV_INPUT].getNormalPolyVoltage(0.f, c) / 3.f, 0.05f, 4.f);
			float tune = params[TUNE_PARAM].getValue();
			// BASE_FREQ 180Hz; djembe (centro) baja ~0.65× para cuerpo 60-80Hz
			float djembePitch = 1.f - 0.30f * (1.f - fabsf(material - 0.5f) * 2.f);
			if (djembePitch < 0.70f) djembePitch = 0.70f;
			// Darbuka (tope) sube de tono: cuerpo metálico tenso
			float darbukaPitch = 1.f + 0.45f * fmaxf(0.f, (material - 0.5f) * 2.f);
			// Darbuka: tiempo ×0.4 en el tope (slap seco, muerto a 300ms)
			float darbukaTime = 1.f - 0.60f * fmaxf(0.f, (material - 0.5f) * 2.f);
			float freq = 180.f * powf(2.f, (tune + voct * 12.f) / 12.f) * djembePitch * darbukaPitch;
			freq = clamp(freq, 30.f, args.sampleRate * 0.45f);
			float vel = inputs[VELOCITY_INPUT].isConnected() ? clamp(inputs[VELOCITY_INPUT].getNormalPolyVoltage(10.f, c) / 10.f, 0.f, 1.f) : 1.f;
			float level = params[LEVEL_PARAM].getValue() * vel;
			// Djembe necesita +10dB de nivel (sin tocar taiko/darbuka)
			float matGain = 1.f + 10.0f * (1.f - fabsf(material - 0.5f) * 2.f) + 6.0f * fmaxf(0.f, (material - 0.5f) * 2.f) + 7.00f * (1.f - clamp(material / 0.35f, 0.f, 1.f));
			if (matGain < 1.f) matGain = 1.f;
			level *= matGain;

			if (doTrig) {
				int v = choke ? (1 - lastVoice) : ((lastVoice + 1) % 2);
				if (choke) {
					voices[1 - v].exciteLeft = 0;
					voices[1 - v].samplesSinceTrig = 99999;
				}
				int burst = std::max(1, (int)(0.012f * args.sampleRate)); // 12ms click
				// Reseed por golpe: cada golpe único aunque alternen voces
				voices[v].rng ^= (uint32_t)(trigCount * 2654435761u + 97u);
				trigCount++;
				voices[v].trigger(burst, material);
				lastVoice = v;
			}
			float voiceL = 0.f;
			float voiceR = 0.f;
			for (int v = 0; v < 2; v++) {
				float vL = 0.f, vR = 0.f;
				voices[v].process(freq, baseT60, args.sampleRate, material, vL, vR, darbukaTime, harm);
				voiceL += vL;
				voiceR += vR;
			}
			voiceL *= level;
			voiceR *= level;
			float wetL = 0.f, wetR = 0.f;
			roomProcess((voiceL + voiceR) * 0.5f, args.sampleRate, wetL, wetR);
			float djembeAmt2 = getDjembeAmt(material);
			float darbukaAmt2 = fmaxf(0.f, (material - 0.5f) * 2.f);
			wetL *= (1.f - djembeAmt2 * 0.9f) * (1.f - darbukaAmt2 * 0.85f);
			wetR *= (1.f - djembeAmt2 * 0.9f) * (1.f - darbukaAmt2 * 0.85f);
			voiceL += wetL;
			voiceR += wetR;
			// Limitador enlazado: misma ganancia en ambos lados (no rompe la imagen)
			{
				float a = fmaxf(fabsf(voiceL), fabsf(voiceR));
				float atkC = 1.f - expf(-1.f / (0.0002f * args.sampleRate));
				float relC = 1.f - expf(-1.f / (0.008f * args.sampleRate));
				limEnv[c] += (a - limEnv[c]) * (a > limEnv[c] ? atkC : relC);
				float djembeThr = 0.032f + getDjembeAmt(material) * 0.088f + fmaxf(0.f, (material - 0.5f) * 2.f) * 0.080f + (1.f - clamp(material / 0.35f, 0.f, 1.f)) * 0.018f;
				float g = djembeThr / (limEnv[c] + 1e-6f);
				if (g > 1.f) g = 1.f;
				voiceL *= g;
				voiceR *= g;
			}
			if (c == 0) {
				outMixL = voiceL;
				outMixR = voiceR;
			}
			else {
				outMixL = (outMixL + voiceL) * 0.5f;
				outMixR = (outMixR + voiceR) * 0.5f;
			}
		}
		outputs[OUT_OUTPUT].setVoltage(clamp(outMixL * 52.f, -10.f, 10.f));
		outputs[OUT_OUTPUT].setChannels(1);
		outputs[OUT_RIGHT].setVoltage(clamp(outMixR * 52.f * 1.43f, -10.f, 10.f));
		outputs[OUT_RIGHT].setChannels(1);
	}
};

constexpr float Membrane::LEATHER_RATIO[];
constexpr float Membrane::DJEMBE_RATIO[];
constexpr float Membrane::PLASTIC_RATIO[];
constexpr float Membrane::LEATHER_GAIN[];
constexpr float Membrane::DJEMBE_GAIN[];
constexpr float Membrane::PLASTIC_GAIN[];
constexpr float Membrane::LEATHER_DECAY[];
constexpr float Membrane::DJEMBE_DECAY[];
constexpr float Membrane::PLASTIC_DECAY[];

struct MembraneWidget : ModuleWidget {
	MembraneWidget(Membrane* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Membrane.svg")));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(8, 22)), module, Membrane::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 22)), module, Membrane::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(8, 40)), module, Membrane::LEVEL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(22, 40)), module, Membrane::MATERIAL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(8, 56)), module, Membrane::DRIVE_PARAM));
		addParam(createParamCentered<VCVBezelLatch>(mm2px(Vec(22, 56)), module, Membrane::CHOKE_PARAM));
		addChild(createLightCentered<VCVBezelLight<RedLight>>(mm2px(Vec(22, 56)), module, Membrane::CHOKE_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8, 72)), module, Membrane::DRIVE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22, 72)), module, Membrane::DECAY_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8, 86)), module, Membrane::VELOCITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22, 86)), module, Membrane::MATERIAL_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8, 100)), module, Membrane::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22, 100)), module, Membrane::VOCT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8, 118)), module, Membrane::OUT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(22, 118)), module, Membrane::OUT_RIGHT));
	}
};

Model* modelMembrane = createModel<Membrane, MembraneWidget>("membrane");