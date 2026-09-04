#include "plugin.hpp"
#include <cmath>
#include <cstdint>
#include <algorithm>

struct Balatube : Module {
	// Modal ratios and relative gains/decays measured from analysis of the
	// reference sample (tubulum 9.wav). The partials at 117.2 / 328.1 / 632.8 Hz
	// match the ratios 1 : 2.756 : 5.404, i.e. the bending-mode series of a
	// free-free bar (Euler-Bernoulli beam) — the same physics as a xylophone/
	// marimba bar — NOT a harmonic series and NOT a closed air column. This
	// turned out to sound like a wooden balafon/ebony bar rather than the
	// original PVC pipe target, which the "Material" knob below morphs away
	// from.
	//
	// WOOD_GAIN was recalibrated against a recorded A/B test (tubulum
	// prueba-001.wav vs tubulum 9.wav): the raw excitation naturally favors
	// higher modes over the fundamental (a fixed-duration noise burst fits
	// more oscillation cycles into faster modes), so these gains pre-correct
	// for that bias to match the real instrument's measured spectral balance,
	// where mode 2 (321Hz) rings almost as loud as the fundamental.
	//
	// PVC_* models a tube closed at one end instead of a struck bar: its
	// resonant modes are the odd harmonic series (1:3:5:7:9:11), the textbook
	// physical model for a closed-open air column. Gains fall off smoothly
	// (a clean, more "hollow" harmonic spectrum vs. the bar's mode-2 spike),
	// and each mode decays more evenly relative to the others (PVC/plastic
	// has less frequency-dependent internal damping than wood, so it "sings"
	// longer instead of losing its highs immediately).
	static const int NUM_MODES = 6;
	static constexpr float WOOD_RATIO[NUM_MODES] = {1.f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f};
	static constexpr float WOOD_GAIN[NUM_MODES]  = {1.44543f, 10.87804f, 0.34459f, 0.36782f, 0.055f, 0.00692f};
	static constexpr float WOOD_DECAY[NUM_MODES] = {1.f, 0.80f, 0.65f, 0.50f, 0.35f, 0.25f};

	// PVC_GAIN was recalibrated the same way as WOOD_GAIN: a recorded A/B
	// test (tubulum prueba-005.wav, Material fully toward PVC) showed the
	// same fixed-duration-burst bias distorting the intended smooth falloff
	// (mode 2 came out ~7dB quieter than designed), so these values
	// pre-correct for it to land on a clean ~5dB-per-harmonic curve.
	static constexpr float PVC_RATIO[NUM_MODES]  = {1.f, 3.f, 5.f, 7.f, 9.f, 11.f};
	static constexpr float PVC_GAIN[NUM_MODES]   = {1.1013f, 0.5623f, 0.7458f, 0.7487f, 0.3899f, 0.3082f};
	static constexpr float PVC_DECAY[NUM_MODES]  = {1.f, 0.90f, 0.82f, 0.75f, 0.68f, 0.60f};

	// Tune = 0 / V-Oct = 0 is pinned to C4 (261.6256 Hz, same value as
	// dsp::FREQ_C4 -- duplicated as a literal since FREQ_C4 itself isn't
	// constexpr), matching VCV Rack's standard V/oct convention (see
	// https://vcvrack.com/manual/VoltageStandards) so Balatube lines up
	// with other oscillators/modules fed the same V/Oct CV without manual
	// compensation. The reference recording's measured fundamental
	// (~117.2 Hz, close to Bb2, 14 semitones below C4) is reproduced at
	// Tune = -14 instead of at the knob's default center.
	static constexpr float BASE_FREQ = 261.6256f;

	static const int DELAY_BUF_SIZE = 2048;

	enum ParamIds {
		TUNE_PARAM,
		DECAY_PARAM,
		LEVEL_PARAM,
		RATE_PARAM,
		DEPTH_PARAM,
		FEEDBACK_PARAM,
		COLOR_PARAM,
		MIX_PARAM,
		MATERIAL_PARAM,
		DRIVE_PARAM,
		OD_MIX_PARAM,
		CRUNCH_PARAM,
		EXT_MIX_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		VOCT_INPUT,
		RATE_CV_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		// One CV input per remaining knob, added at the end so existing
		// patches' cables on the ports above keep their indices.
		TUNE_CV_INPUT,
		DECAY_CV_INPUT,
		LEVEL_CV_INPUT,
		MATERIAL_CV_INPUT,
		DEPTH_CV_INPUT,
		FEEDBACK_CV_INPUT,
		COLOR_CV_INPUT,
		CRUNCH_CV_INPUT,
		DRIVE_CV_INPUT,
		OD_MIX_CV_INPUT,
		MIX_CV_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	// --- Modal resonator voice ---
	// Exciter: a short white-noise burst (~15 ms) feeds a bank of resonant
	// two-pole "modal" filters, one per partial. Each filter rings down on its
	// own after the burst according to its own T60, exactly like a struck bar.
	struct ModalVoice {
		float y1[NUM_MODES] = {};
		float y2[NUM_MODES] = {};
		int exciteSamplesLeft = 0;
		uint32_t rngState = 22222;

		void trigger(int burstSamples) {
			exciteSamplesLeft = burstSamples;
			for (int i = 0; i < NUM_MODES; i++) {
				y1[i] = 0.f;
				y2[i] = 0.f;
			}
		}

		float noise() {
			// xorshift32
			rngState ^= rngState << 13;
			rngState ^= rngState >> 17;
			rngState ^= rngState << 5;
			return ((float)(rngState & 0xFFFFFF) / (float)0xFFFFFF) * 2.f - 1.f;
		}

		// `material` morphs the resonator bank from 0 = wood/balafon bar
		// modes to 1 = PVC closed-tube (odd harmonic) modes. Ratio, gain and
		// decay-fraction are each linearly interpolated per mode.
		//
		// `extExcite` is an optional continuous excitation signal (external
		// audio) that gets added to the internal trigger-burst excitation
		// every sample. It defaults to 0 so the polyphonic instrument voices
		// (which never pass it) behave exactly as before. When driven with a
		// continuous signal instead of a one-shot burst, the same two-pole
		// bank acts as a permanently-on resonant/formant filter: baseT60
		// then sets each mode's ringing time, i.e. its Q/bandwidth, rather
		// than a literal decay-after-impulse time.
		float process(float freq, float baseT60, float sampleRate, float material, float extExcite = 0.f) {
			float exc = extExcite;
			if (exciteSamplesLeft > 0) {
				// Envelope the burst so it starts strong and tapers off,
				// matching the broadband transient measured at t=0.
				float env = (float)exciteSamplesLeft / (0.015f * sampleRate);
				exc += noise() * std::min(1.f, env);
				exciteSamplesLeft--;
			}

			float out = 0.f;
			float gainSum = 0.f;
			for (int i = 0; i < NUM_MODES; i++) {
				float ratio = WOOD_RATIO[i] + (PVC_RATIO[i] - WOOD_RATIO[i]) * material;
				float gain  = WOOD_GAIN[i]  + (PVC_GAIN[i]  - WOOD_GAIN[i])  * material;
				float decayFrac = WOOD_DECAY[i] + (PVC_DECAY[i] - WOOD_DECAY[i]) * material;
				gainSum += gain;

				float modeFreq = freq * ratio;
				if (modeFreq >= sampleRate * 0.49f) continue; // avoid aliasing on high pitches
				float t60 = std::max(0.01f, baseT60 * decayFrac);
				float r = powf(0.001f, 1.f / (t60 * sampleRate));
				float w = 2.f * (float)M_PI * modeFreq / sampleRate;
				float y0 = 2.f * r * cosf(w) * y1[i] - r * r * y2[i] + exc * gain;
				y2[i] = y1[i];
				y1[i] = y0;
				out += y0;
			}
			return out / gainSum;
		}
	};

	struct Flanger {
		float buf[DELAY_BUF_SIZE] = {};
		int writePos = 0;
		float lfoPhase = 0.f;
		float feedbackDelay = 0.f;
		float colorState = 0.f;
		// Slow peak-follower on the input, used to scale the feedback
		// saturator (see process()) so it stays proportional to whatever
		// amplitude the signal actually runs at, instead of a fixed +-1V
		// ceiling regardless of scale.
		float peakEnv = 0.f;
		// Starting/reset LFO phase. Left at 0 for the polyphonic instrument
		// voices (unchanged behavior). The stereo external-input bus sets
		// this to 90 degrees on its R-channel instance so L/R sweep with the
		// same rate but offset phase, giving the flanger real stereo width
		// instead of both channels moving in lockstep.
		float lfoPhaseOffset = 0.f;

		void reset() {
			for (int i = 0; i < DELAY_BUF_SIZE; i++) buf[i] = 0.f;
			writePos = 0;
			lfoPhase = lfoPhaseOffset;
			feedbackDelay = 0.f;
			colorState = 0.f;
			peakEnv = 0.f;
		}

		float process(float input, float rate, float depth, float feedback, float color, float mix, float sr) {
			// LFO: sine, 0.01 - 10 Hz
			float lfoFreq = 0.01f * powf(1000.f, rate);
			float lfo = sinf(lfoPhase);
			lfoPhase += 2.f * 3.14159265f * lfoFreq / sr;
			if (lfoPhase > 6.2831853f) lfoPhase -= 6.2831853f;

			// Delay time: 0.2ms to 12ms modulated by LFO. Widened from the
			// original 0.5-8ms range (and Feedback's ceiling raised, see
			// caller) so cranking Depth/Feedback gets into classic "jet
			// plane" territory: deep, resonant sweeping notches instead of a
			// gentle chorus-y wobble.
			float minDelay = sr * 0.0002f;
			float maxDelay = sr * 0.012f;
			float delayCenter = (minDelay + maxDelay) * 0.5f;
			float delayRange = (maxDelay - minDelay) * 0.5f;
			float delaySamples = delayCenter + lfo * depth * delayRange;

			// Write to buffer. The feedback path is soft-saturated (tanh)
			// instead of a bare multiply: this lets Feedback be pushed much
			// higher (near/at self-oscillation) for a screaming resonant
			// "jet" character without the comb filter blowing up numerically.
			//
			// Bug fixed here: tanhf() always saturates to +-1 REGARDLESS of
			// scale, but this module's working signal runs at a much larger
			// raw amplitude (order of ~100+, since the final output stage
			// applies a small fixed GAIN to bring it down to Eurorack
			// levels — see process() below). With a bare `tanhf(feedbackDelay
			// * feedback)`, the feedback term was capped to +-1 while
			// `input` sat two-plus orders of magnitude higher, so it was
			// completely swamped: the feedback loop's contribution to the
			// output was negligible, and what remained was effectively just
			// a short modulated delay summed with the dry signal — a
			// chorus-y pitch wobble, not a real resonant flanger, no matter
			// how high Feedback was turned up.
			//
			// Fix: track a slow peak envelope of the input and saturate the
			// feedback term relative to THAT scale (`scale * tanhf(x /
			// scale)` instead of `tanhf(x)`), so the feedback loop can
			// recirculate at an amplitude comparable to the actual signal
			// instead of a fixed unit ceiling. The envelope decays over
			// ~0.4s so a quiet passage (or a percussive hit's tail) still
			// gets a proportionally smaller — but not instantly
			// collapsed-to-1.0 — ceiling, preserving the long resonant tail
			// this was originally designed for.
			float peakDecay = expf(-1.f / (0.4f * sr));
			peakEnv = std::max(fabsf(input), peakEnv * peakDecay);
			float fbScale = std::max(1.f, peakEnv);
			buf[writePos] = input + fbScale * tanhf(feedbackDelay * feedback / fbScale);
			writePos = (writePos + 1) % DELAY_BUF_SIZE;

			// Read with fractional delay (linear interp)
			float readPos = (float)writePos - delaySamples;
			if (readPos < 0.f) readPos += (float)DELAY_BUF_SIZE;
			int idx0 = (int)readPos;
			int idx1 = (idx0 + 1) % DELAY_BUF_SIZE;
			float frac = readPos - (float)idx0;
			float delayed = buf[idx0] * (1.f - frac) + buf[idx1] * frac;

			// Color: one-pole lowpass on the delayed signal
			// color 0 = bright (no filtering), 1 = dark (heavy filtering)
			float coeff = 0.05f + (1.f - color) * 0.95f;
			colorState += coeff * (delayed - colorState);
			delayed = colorState;

			feedbackDelay = delayed;

			// Dry/wet mix: additive, NOT a dry/wet crossfade. With a
			// crossfade (input*(1-mix) + delayed*mix), the notch depth from
			// dry/delayed interference is deepest at mix=0.5 and completely
			// disappears at mix=1 (output becomes the delayed signal alone,
			// which has a perfectly flat frequency response — no notches,
			// no "jet" effect at all). That made turning Mix all the way up
			// — the most intuitive way to ask for "more flanger" — silently
			// give the *weakest* possible effect. Keeping the dry signal at
			// a fixed unity gain and only scaling the wet/delayed term means
			// notch depth instead grows monotonically with Mix, reaching
			// maximum (deepest, most "jet") right at Mix=1.
			return input + delayed * mix;
		}
	};

	// Simple tanh overdrive/saturator. Operates on the already level-matched
	// signal (post OUTPUT_GAIN, see process()) so Drive=0 sits at a mild,
	// natural-sounding saturation instead of instantly slamming into the
	// limiter regardless of knob position. Mix=0 fully bypasses it so
	// existing patches/defaults sound unchanged unless the user dials it in.
	// Crunch adds a cascaded asymmetric-clipping stage after Drive: biasing
	// the signal before a tanh clip compresses one half-wave harder than the
	// other, generating even harmonics (a "warmer"/more aggressive character
	// than Drive's symmetric tanh alone, which only adds odd harmonics). The
	// bias is subtracted back out afterward (using the clipper's own
	// response to a silent input) so Crunch doesn't inject a DC offset that
	// would otherwise accumulate through the downstream limiter.
	//
	// Crunch=0 must reproduce the pre-Crunch behavior bit-for-bit (existing
	// patches/defaults sound unchanged): rather than blending toward a tanh
	// of `driven`, the shaped signal is interpolated in as an offset from
	// `driven` (crunched = driven + crunch * (shaped - driven)), which is
	// exactly `driven` when crunch = 0.
	struct Overdrive {
		static float process(float input, float drive, float crunch, float mix) {
			float driveGain = 1.f + drive * 19.f; // 1x .. 20x
			float driven = tanhf(input * driveGain);

			const float bias = 0.5f;
			const float crunchGain = 3.f;
			float shaped = tanhf((driven + bias) * crunchGain) - tanhf(bias * crunchGain);
			float crunched = driven + crunch * (shaped - driven);

			return input * (1.f - mix) + crunched * mix;
		}
	};

	dsp::SchmittTrigger trig[16];
	ModalVoice modal[16];
	Flanger flanger[16];

	// Stereo external-input processing bus: two always-on, non-polyphonic
	// instances of the same resonator + flanger engine used by the
	// instrument voices above, fed by IN_L/IN_R instead of the trigger
	// burst. Shares every knob (Tune/Decay/Material/Rate/Depth/Feedback/
	// Color/Mix/Level/Drive/OD Mix/Crunch) with the instrument — only the
	// DSP state (filter/delay buffers) is separate.
	ModalVoice extModal[2];
	Flanger extFlanger[2];
	// Smoothed attenuator level for EXT_MIX_PARAM, applied to OUT_L/OUT_R
	// (click-free control of external bus level).
	float extMixLevel = 1.f;

	Balatube() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(TUNE_PARAM, -24.f, 24.f, 0.f, "Tune", " semitones");
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay");
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");
		configParam(RATE_PARAM, 0.f, 1.f, 0.3f, "Rate");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.4f, "Depth");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.5f, "Feedback");
		configParam(COLOR_PARAM, 0.f, 1.f, 0.5f, "Color");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix");
		configParam(MATERIAL_PARAM, 0.f, 1.f, 0.f, "Material (Wood -> PVC)");
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.3f, "Drive");
		configParam(OD_MIX_PARAM, 0.f, 1.f, 0.f, "Overdrive Mix");
		configParam(CRUNCH_PARAM, 0.f, 1.f, 0.f, "Crunch");
		configParam(EXT_MIX_PARAM, 0.f, 1.f, 1.f, "External Mix");
		configInput(TRIG_INPUT, "Trigger");
		configInput(VOCT_INPUT, "V/Oct");
		configInput(RATE_CV_INPUT, "Rate CV");
		configInput(IN_L_INPUT, "External audio L");
		configInput(IN_R_INPUT, "External audio R");
		configInput(TUNE_CV_INPUT, "Tune CV");
		configInput(DECAY_CV_INPUT, "Decay CV");
		configInput(LEVEL_CV_INPUT, "Level CV");
		configInput(MATERIAL_CV_INPUT, "Material CV");
		configInput(DEPTH_CV_INPUT, "Depth CV");
		configInput(FEEDBACK_CV_INPUT, "Feedback CV");
		configInput(COLOR_CV_INPUT, "Color CV");
		configInput(CRUNCH_CV_INPUT, "Crunch CV");
		configInput(DRIVE_CV_INPUT, "Drive CV");
		configInput(OD_MIX_CV_INPUT, "Overdrive Mix CV");
		configInput(MIX_CV_INPUT, "Flanger Mix CV");
		configOutput(OUT_OUTPUT, "Mix");
		configOutput(OUT_L_OUTPUT, "Left");
		configOutput(OUT_R_OUTPUT, "Right");

		// R channel's flanger sweeps 90 degrees out of phase with L (same
		// rate) so the stereo bus gets real width instead of both channels
		// moving in lockstep. Set directly since reset() is never called on
		// this always-on bus (no trigger to reset on).
		extFlanger[1].lfoPhaseOffset = (float)M_PI / 2.f;
		extFlanger[1].lfoPhase = (float)M_PI / 2.f;
	}

	void process(const ProcessArgs& args) override {
		// Generic CV helper for the 0..1-range knobs: +10V shifts the knob
		// across its full range (same convention the original Rate CV used).
		auto withCv01 = [this](float value, int cvInputId) {
			if (inputs[cvInputId].isConnected())
				value += inputs[cvInputId].getVoltage() * 0.1f;
			return std::max(0.f, std::min(1.f, value));
		};

		float tune = params[TUNE_PARAM].getValue();
		// Tune CV uses 1V/oct scaling (like the V/Oct input, in semitones)
		// instead of the generic 0..1 convention, since Tune's own range is
		// already semitones, not 0..1. Not clamped, same as the knob itself.
		if (inputs[TUNE_CV_INPUT].isConnected())
			tune += inputs[TUNE_CV_INPUT].getVoltage() * 12.f;
		float decay = withCv01(params[DECAY_PARAM].getValue(), DECAY_CV_INPUT);
		float level = withCv01(params[LEVEL_PARAM].getValue(), LEVEL_CV_INPUT);
		float rate = withCv01(params[RATE_PARAM].getValue(), RATE_CV_INPUT);
		float depth = withCv01(params[DEPTH_PARAM].getValue(), DEPTH_CV_INPUT);
		// Feedback mapping is deliberately nonlinear. On a percussive voice
		// the flanger's own comb resonance needs to keep ringing *longer*
		// than the note itself for a slow LFO sweep to be audible as a
		// proper "jet flyover" instead of a barely-there color change that
		// dies with the hit. Measured tail length (time the flanger's own
		// resonance stays audible after the input has decayed) vs feedback:
		//   0.85 -> ~0.5s (dies with the note, no time to sweep)
		//   0.98 -> ~0.5s (same — 0.85-0.98 barely matters for tail length)
		//   0.998 -> ~1.6s
		//   0.999 -> ~2.8s (a real, hearable multi-second "whoosh" tail)
		// That whole useful range lives in the last ~2% of a linear 0-1
		// knob, so it's unreachable in practice. This curve spends most of
		// the knob's travel there instead, topping out at 0.999 (not 1.0)
		// so it always eventually decays rather than droning forever.
		float fbKnob = withCv01(params[FEEDBACK_PARAM].getValue(), FEEDBACK_CV_INPUT);
		float feedback = 0.999f - 0.149f * powf(1.f - fbKnob, 4.f);
		float color = withCv01(params[COLOR_PARAM].getValue(), COLOR_CV_INPUT);
		float mix = withCv01(params[MIX_PARAM].getValue(), MIX_CV_INPUT);
		float material = withCv01(params[MATERIAL_PARAM].getValue(), MATERIAL_CV_INPUT);
		float drive = withCv01(params[DRIVE_PARAM].getValue(), DRIVE_CV_INPUT);
		float odMix = withCv01(params[OD_MIX_PARAM].getValue(), OD_MIX_CV_INPUT);
		float crunch = withCv01(params[CRUNCH_PARAM].getValue(), CRUNCH_CV_INPUT);

		// Shared by both the instrument voices and the stereo external-input
		// bus below (see comment at original definition site further down).
		const float GAIN = 0.007129f;
		const float CEILING = 4.5f;

		// Master decay time for the fundamental mode (0.05s - 1.2s).
		// Higher modes scale down from this via MODE_DECAY[].
		float baseT60 = 0.05f + decay * 1.15f;

		// Excitation burst duration: needs to span at least ~1 period of the
		// fundamental (~8.6ms at BASE_FREQ) for the resonator to actually
		// ring it up. A/B testing against a real recording (tubulum
		// prueba-001.wav) confirmed a too-short burst (previously tried at
		// 1ms) leaves the fundamental essentially unexcited: the higher,
		// faster modes fit several cycles into the burst while the low mode
		// barely completes one, skewing the timbre brighter than the real
		// instrument. Level safety is instead handled entirely by the tanh
		// limiter below, independent of burst length.
		int burstSamples = std::max(1, (int)(0.015f * args.sampleRate));

		int poly = std::max(inputs[TRIG_INPUT].getChannels(),
		                     std::max(inputs[VOCT_INPUT].getChannels(), 1));

		float out[16] = {};

		for (int p = 0; p < poly; p++) {
			int trigIdx = std::min(p, inputs[TRIG_INPUT].getChannels() - 1);
			int voctIdx = std::min(p, inputs[VOCT_INPUT].getChannels() - 1);

			float trigVal = inputs[TRIG_INPUT].getVoltage(trigIdx);
			float voct = inputs[VOCT_INPUT].isConnected() ?
				inputs[VOCT_INPUT].getVoltage(voctIdx) : 0.f;

			if (trig[p].process(trigVal)) {
				modal[p].trigger(burstSamples);
				flanger[p].reset();
			}

			// --- Modal resonator bank (physical modeling) ---
			float freq = BASE_FREQ * powf(2.f, (tune + voct * 12.f) / 12.f);
			float modalOut = modal[p].process(freq, baseT60, args.sampleRate, material);

			// --- Flanger ---
			float wet = flanger[p].process(modalOut, rate, depth, feedback, color, mix, args.sampleRate);

			// --- Output stage: soft limiter ---
			// The resonator bank's peak amplitude naturally varies a lot with
			// Decay/Tune (long-decay high-Q modes vs. short percussive hits),
			// and that variance also scales with the engine sample rate. A
			// fixed linear gain would either clip on some settings or be too
			// quiet on others. Instead we drive a tanh soft-limiter so the
			// output is mathematically bounded to CEILING volts no matter how
			// hard the resonators ring, regardless of Decay/Tune/sample rate.
			// GAIN sets how hard "normal" hits push into the limiter; CEILING
			// sits a bit under the 5V (0dBFS-equivalent) reference on purpose
			// so Level at maximum never exceeds 0dB, with headroom to spare
			// (boost further downstream with a mixer if a hit needs to hit harder).
			float preOD = wet * level * GAIN;

			// --- Overdrive ---
			// Runs on the already level-matched signal (typical peaks ~1)
			// instead of the raw resonator output, so Drive behaves the same
			// musically regardless of Decay/Tune/Material/sample rate. At
			// OD Mix = 0 this is a no-op and output matches the pre-overdrive
			// behavior exactly.
			float driven = Overdrive::process(preOD, drive, crunch, odMix);

			out[p] = tanhf(driven) * CEILING;
		}

		outputs[OUT_OUTPUT].setChannels(poly);
		for (int p = 0; p < poly; p++)
			outputs[OUT_OUTPUT].setVoltage(out[p], p);

		// --- Stereo external-input processing bus ---
		// Always-on (no trigger needed): IN_L/IN_R feed the resonator bank
		// continuously as excitation instead of the one-shot noise burst
		// used by the instrument voices above, turning the same modal filter
		// bank into a permanently-on resonant/formant filter over whatever
		// audio is patched in. Shares every knob with the instrument voices
		// (Tune sets the filter's center frequency, Decay sets each mode's
		// Q/ringing time, etc.) — only the DSP state is separate, and the
		// R-channel flanger runs 90 degrees out of phase with L for stereo
		// width. Input voltage is scaled down from the eurorack +-5V audio
		// convention to roughly match the -1..1 amplitude the modal filters
		// expect from the internal noise burst. Level's meaning is
		// bus-specific: dry/wet blend of resonator vs. raw input here (see
		// below), rather than a plain volume knob like it is for the
		// instrument voices above.
		// EXT_MIX_PARAM: continuous attenuator (0..1) on the external
		// input before it enters the resonator/flanger/overdrive chain.
		// Smoothed to avoid clicks when turning the knob mid-signal.
		float extMixTarget = params[EXT_MIX_PARAM].getValue();
		float smoothTime = (extMixTarget > extMixLevel) ? 0.003f : 0.02f;
		extMixLevel += (extMixTarget - extMixLevel) * (1.f - std::exp(-1.f / (smoothTime * args.sampleRate)));

		float freqExt = BASE_FREQ * powf(2.f, tune / 12.f);
		for (int ch = 0; ch < 2; ch++) {
			int inputId = (ch == 0) ? IN_L_INPUT : IN_R_INPUT;
			int outputId = (ch == 0) ? OUT_L_OUTPUT : OUT_R_OUTPUT;
			float extIn = inputs[inputId].getVoltage() * 0.2f * extMixLevel;

			float modalOut = extModal[ch].process(freqExt, baseT60, args.sampleRate, material, extIn);

			// Level is a dry/wet blend here (unlike the instrument voices,
			// where it's a plain output-volume knob, since a triggered synth
			// voice has no "dry" signal to speak of). Turning the resonator
			// Level down should NOT also gate the Flanger/Overdrive for
			// external audio, so the dry signal is scaled up by 1/GAIN to
			// sit in the same raw working range the resonator/flanger
			// already operate in (matching units before blending), then
			// GAIN is applied once at the end exactly like before. At
			// Level=1 this reduces to bit-identical old behavior (100% wet,
			// dry term multiplied by zero); at Level=0 the raw external
			// signal reaches the Flanger and Overdrive untouched.
			float dryScaled = extIn / GAIN;
			float blended = dryScaled * (1.f - level) + modalOut * level;

			float wet = extFlanger[ch].process(blended, rate, depth, feedback, color, mix, args.sampleRate);

			float preOD = wet * GAIN;
			float driven = Overdrive::process(preOD, drive, crunch, odMix);

			outputs[outputId].setVoltage(tanhf(driven) * CEILING);
		}
	}
};

constexpr float Balatube::WOOD_RATIO[];
constexpr float Balatube::WOOD_GAIN[];
constexpr float Balatube::WOOD_DECAY[];
constexpr float Balatube::PVC_RATIO[];
constexpr float Balatube::PVC_GAIN[];
constexpr float Balatube::PVC_DECAY[];

/** Direct-draw text label. NanoSVG (the renderer VCV Rack uses for panel
 background SVGs) does not support <text> elements, so labels baked into the
 SVG file are silently invisible in Rack. Drawing them ourselves with the
 window's UI font (same approach as VocalLamma) is the reliable alternative. */
struct BalatubeTextLabel : Widget {
	std::string text;
	float fontSize;
	NVGcolor color;

	BalatubeTextLabel(std::string t, float fs, NVGcolor c) : text(t), fontSize(fs), color(c) {}

	void draw(const DrawArgs& args) override {
		if (!APP->window->uiFont) return;
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, color);
		nvgText(args.vg, 0, 0, text.c_str(), NULL);
	}
};

/** Raster logo drawn with nanoVG (same approach as VocalLamma): the panel
 SVG cannot embed raster images since the bundled nanoSVG ignores <image>
 elements, so the PNG is painted directly in draw(). */
struct BalatubeLogoWidget : Widget {
	std::string imagePath;

	void draw(const DrawArgs& args) override {
		if (imagePath.empty())
			return;
		std::shared_ptr<window::Image> image = APP->window->loadImage(imagePath);
		if (!image || image->handle < 0)
			return;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		NVGpaint paint = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0.f, image->handle, 1.f);
		nvgFillPaint(args.vg, paint);
		nvgFill(args.vg);
	}
};

struct BalatubeWidget : ModuleWidget {
	BalatubeWidget(Balatube* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Balatube.svg")));

		// No mounting screws (matches VocalLamma's panel).

		// 4-column layout (same column x-positions used throughout, à la
		// VocalLamma's panel) x8 rows: 1 resonator + 2 effects + 3 CV + 2 I/O.
		// C3/C4 shifted right vs. the original evenly-spaced layout so C4 is
		// the same distance from the right edge (91.44mm panel width) as C1
		// is from the left edge (12mm each side), widening the C2-C3 gap to
		// fit the logo in the center.
		const float C1 = 12.f, C2 = 33.5f, C3 = 58.f, C4 = 79.5f;

		// --- Section 1: Modal resonator ---
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C1, 23)), module, Balatube::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C2, 23)), module, Balatube::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C3, 23)), module, Balatube::LEVEL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C4, 23)), module, Balatube::MATERIAL_PARAM));

		// --- Section 2: Effects (Flanger row, then Overdrive/Mix row) ---
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C1, 40)), module, Balatube::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C2, 40)), module, Balatube::DEPTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C3, 40)), module, Balatube::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C4, 40)), module, Balatube::COLOR_PARAM));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C1, 52.5)), module, Balatube::CRUNCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C2, 52.5)), module, Balatube::DRIVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C3, 52.5)), module, Balatube::OD_MIX_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(C4, 52.5)), module, Balatube::MIX_PARAM));

		// --- Section 3: CV, one input per knob above, same columns ---
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C1, 69.5)), module, Balatube::TUNE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C2, 69.5)), module, Balatube::DECAY_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3, 69.5)), module, Balatube::LEVEL_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C4, 69.5)), module, Balatube::MATERIAL_CV_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C1, 82)), module, Balatube::RATE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C2, 82)), module, Balatube::DEPTH_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3, 82)), module, Balatube::FEEDBACK_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C4, 82)), module, Balatube::COLOR_CV_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C1, 94.5)), module, Balatube::CRUNCH_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C2, 94.5)), module, Balatube::DRIVE_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3, 94.5)), module, Balatube::OD_MIX_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C4, 94.5)), module, Balatube::MIX_CV_INPUT));

		// --- Section 4: I/O ---
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C1, 110.5)), module, Balatube::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C2, 110.5)), module, Balatube::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3, 110.5)), module, Balatube::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C4, 110.5)), module, Balatube::IN_R_INPUT));

		// OUT L/OUT R sit directly under IN L/IN R (C3/C4). The main poly
		// OUT stays at C1, under Trig/Gate. That leaves C2 (under V/Oct)
		// free for the external-input enable switch below.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C1, 121.5)), module, Balatube::OUT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C3, 121.5)), module, Balatube::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C4, 121.5)), module, Balatube::OUT_R_OUTPUT));

		// External-input mix knob (smaller than the main knobs), in the gap
		// left at C2 by moving OUT L/OUT R under their inputs.
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(C2, 121.5)), module, Balatube::EXT_MIX_PARAM));

		// Logo, just under the title, in the C2-C3 gap (horizontally clear
		// of Row 1's knobs since it sits between columns, not on one).
		{
			BalatubeLogoWidget* logo = new BalatubeLogoWidget;
			logo->imagePath = asset::plugin(pluginInstance, "res/Logo.png");
			Vec logoSize = mm2px(Vec(12.f, 12.f));
			Vec logoCenter = mm2px(Vec(45.75f, 20.f));
			logo->box.size = logoSize;
			logo->box.pos = logoCenter.minus(logoSize.div(2));
			addChild(logo);
		}

		// --- Labels ---
		NVGcolor gold = nvgRGBA(0xe8, 0xb3, 0x4a, 0xff);
		NVGcolor dim = nvgRGBA(0x9a, 0x9a, 0xa8, 0xff);

		addLabel("BALATUBE", Vec(45.75, 9), 14.f, gold);

		addLabel("TUNE", Vec(C1, 17), 6.5f, dim);
		addLabel("DECAY", Vec(C2, 17), 6.5f, dim);
		addLabel("LEVEL", Vec(C3, 17), 6.5f, dim);
		addLabel("MATERIAL", Vec(C4, 17), 6.f, dim);

		addLabel("EFFECTS", Vec(45.75, 29.5), 7.f, gold);

		addLabel("RATE", Vec(C1, 34), 6.5f, dim);
		addLabel("DEPTH", Vec(C2, 34), 6.5f, dim);
		addLabel("FEEDBACK", Vec(C3, 34), 6.f, dim);
		addLabel("COLOR", Vec(C4, 34), 6.5f, dim);

		addLabel("CRUNCH", Vec(C1, 46.5), 6.f, dim);
		addLabel("DRIVE", Vec(C2, 46.5), 6.5f, dim);
		addLabel("OD MIX", Vec(C3, 46.5), 6.f, dim);
		addLabel("FL MIX", Vec(C4, 46.5), 6.f, dim);

		addLabel("CV", Vec(45.75, 59), 7.f, gold);

		addLabel("TUNE", Vec(C1, 63.5), 5.5f, dim);
		addLabel("DECAY", Vec(C2, 63.5), 5.5f, dim);
		addLabel("LEVEL", Vec(C3, 63.5), 5.5f, dim);
		addLabel("MATERIAL", Vec(C4, 63.5), 5.f, dim);

		addLabel("RATE", Vec(C1, 76), 5.5f, dim);
		addLabel("DEPTH", Vec(C2, 76), 5.5f, dim);
		addLabel("FEEDBACK", Vec(C3, 76), 5.f, dim);
		addLabel("COLOR", Vec(C4, 76), 5.5f, dim);

		addLabel("CRUNCH", Vec(C1, 88.5), 5.f, dim);
		addLabel("DRIVE", Vec(C2, 88.5), 5.5f, dim);
		addLabel("OD MIX", Vec(C3, 88.5), 5.f, dim);
		addLabel("FL MIX", Vec(C4, 88.5), 5.f, dim);

		addLabel("I/O", Vec(45.75, 100.6), 7.f, gold);

		addLabel("GATE", Vec(C1, 105), 6.f, dim);
		addLabel("V/OCT", Vec(C2, 105), 6.f, dim);
		addLabel("IN L", Vec(C3, 105), 6.f, dim);
		addLabel("IN R", Vec(C4, 105), 6.f, dim);

		addLabel("OUT", Vec(C1, 116.5), 6.f, dim);
		addLabel("EXT MIX", Vec(C2, 116.5), 5.5f, dim);
		addLabel("OUT L", Vec(C3, 116.5), 6.f, dim);
		addLabel("OUT R", Vec(C4, 116.5), 6.f, dim);
	}

	void addLabel(const std::string& text, math::Vec posMm, float fontSize, NVGcolor color) {
		BalatubeTextLabel* label = new BalatubeTextLabel(text, fontSize, color);
		label->box.pos = mm2px(posMm);
		addChild(label);
	}
};

Model* modelBalatube = createModel<Balatube, BalatubeWidget>("Balatube");
