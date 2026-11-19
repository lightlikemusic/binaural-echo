// Binaural Echo - a 3D headphone panner and moving echo for the MPC, by Lightlike
//
// Places the track's audio around the listener's head. Azimuth, elevation and
// distance sit on the first four Q-Links (with Mix), and three render modes
// trade realism for CPU:
//
//   Head   Each input channel is a point source rendered through a parametric
//          head model (no measured HRTF data): interaural time difference
//          (spherical head), a head-shadow shelf per ear, five pinna echoes and
//          a shoulder echo whose delays move with direction, and a darker
//          treble for sources behind. Near sources get extra level difference.
//   Field  First-order ambisonics. The two input channels are encoded into a
//          4-channel sound field (W, X, Y, Z) at +/- Width, the field is
//          rotated by Azimuth and Elevation, then decoded to eight virtual
//          speakers on a cube, each rendered with the head model. The 4-channel
//          signal never leaves the plugin: the MPC only sees stereo.
//   Lite   The cheap version: time and level differences between the ears, a
//          one-pole shadow filter on the far ear and the rear treble cue.
//
// All modes share the distance cues: level falls with distance, air
// absorption dulls far sources, early reflections arrive sooner after the
// direct sound when the source is far away, and a small late reverb gives the
// room (its level stays constant, so far sources sound wetter).
//
// Motion: an optional built-in mover adds a path to the Azimuth / Elevation /
// Distance you set (Orbit, Swing, Figure 8, Spiral, Fly-by), at a free rate in
// Hz or locked to the MPC's tempo. The screen pictures follow the moving source
// through three read-only display parameters (~Az, ~El, ~Dist) that the plugin
// updates and reports to the host about 30 times a second.
//
// Echo: a delay whose repeats travel around the head. The delay runs on a
// first-order sound field (W, X, Y, Z) and the field is rotated by Step (and
// Tilt) on every trip round the feedback loop, so each repeat comes back from
// a new direction; a lowpass in the loop makes each repeat darker and more
// distant. The echoes are decoded through the same eight virtual speakers as
// Field mode.
//
// Signal flow
//   in L/R -> [distance gain, air lowpass] -> render mode --+--> mix -> volume
//   in L+R -> room send -> early reflections -> FDN reverb --/
//
// Coordinates: x = right, y = up, f = front. Azimuth 0 = front, +90 = right.
// Elevation +90 = straight up.
//
// The head model follows the structural HRTF model of Brown & Duda (1998):
// spherical-head shadow filter, Woodworth ITD, pinna and shoulder echoes.
// It is written from the paper's equations; no measured data is used.

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstring>

namespace {

enum ParamIndex
{
    kVolume = 0,  // dB, output level (parameter 0: harmless)
    kAzimuth,     // degrees, -180..180
    kElevation,   // degrees, -90..90
    kDistance,    // metres, 0.2..10 (log)
    kMix,         // % wet
    kMode,        // Head / Field / Lite
    kWidth,       // % spread of the two input channels (100% = +/-45 degrees)
    kRoom,        // % early reflections + reverb
    kAir,         // % air absorption with distance
    kMotion,      // Off / Orbit / Swing / Figure 8 / Spiral / Fly-by
    kRate,        // Hz, one motion cycle per 1/Rate seconds (Sync off)
    kSync,        // lock the cycle to the tempo
    kDivision,    // cycle length when synced
    kDepth,       // % size of the movement
    kReverse,     // move the other way
    kShowAz,      // read-only: where the source is now (for the screen)
    kShowEl,
    kShowDist,
    kIntensity,   // % how strongly the 3D cues are exaggerated (0 = natural head)
    kEcho,        // % echo level (0 = off)
    kEchoTime,    // ms between repeats (Echo Sync off)
    kEchoSync,    // lock the echo time to the tempo
    kEchoDiv,     // echo time when synced
    kFeedback,    // % repeats
    kStep,        // degrees each repeat turns (+ = to the right)
    kTilt,        // degrees each repeat tilts (+ = up in front)
    kDamping,     // % how fast repeats darken
    kNumParams
};

enum Mode { kModeHead = 0, kModeField, kModeLite };
enum Motion { kMotionOff = 0, kOrbit, kSwing, kFigure8, kSpiral, kFlyBy };

inline bool isDisplayParam(int i) { return i >= kShowAz && i <= kShowDist; }
constexpr double kDivisionQn[7] = { 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5 };   // quarter notes per cycle
constexpr int    kNumEchoDiv = 9;
constexpr double kEchoDivQn[kNumEchoDiv] = { 0.125, 0.25, 0.375, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 };
constexpr float  kEchoMaxSeconds = 4.0f;

constexpr float kPi       = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kHeadA    = 0.0875f;   // head radius, m
constexpr float kSound    = 343.0f;    // speed of sound, m/s
constexpr int   kSub      = 16;        // samples per control sub-block

constexpr int kNumPinna = 5;
// Pinna echoes (Brown & Duda): reflection gains and delay shape, in samples at 44.1 kHz.
constexpr float kPinnaRho[kNumPinna] = { 0.5f, -1.0f, 0.5f, -0.25f, 0.25f };
constexpr float kPinnaA[kNumPinna]   = { 1.0f, 5.0f, 5.0f, 5.0f, 5.0f };
constexpr float kPinnaB[kNumPinna]   = { 2.0f, 4.0f, 7.0f, 11.0f, 13.0f };
constexpr float kPinnaD              = 0.85f;
constexpr float kPinnaAmount         = 0.45f;   // echo level at Intensity 0 (tames the comb)

// ---------------------------------------------------------------------------
// Ring buffer with fractional reads. Power-of-two size, no allocation.
// ---------------------------------------------------------------------------
template <int N>
struct Ring
{
    static_assert((N & (N - 1)) == 0, "power of two");
    float buf[N] = {};
    int   pos = 0;

    void reset() { std::memset(buf, 0, sizeof(buf)); pos = 0; }
    void write(float x) { pos = (pos + 1) & (N - 1); buf[pos] = x; }
    // 0 <= delay < N - 2 (callers clamp)
    float read(float delay) const
    {
        const int   di = (int)delay;
        const float fr = delay - (float)di;
        const float a  = buf[(pos - di) & (N - 1)];
        const float b  = buf[(pos - di - 1) & (N - 1)];
        return a + (b - a) * fr;
    }
};

using SourceRing = Ring<1024>;   // ITD + pinna + shoulder: < 3 ms, fine up to 192 kHz

// ---------------------------------------------------------------------------
// Per-ear rendering parameters for one source direction.
// ---------------------------------------------------------------------------
struct EarParams
{
    float delay = 2.0f;                 // base delay (ITD), samples
    float tapDelay[kNumPinna + 1] = {}; // pinna echoes + shoulder, relative to base
    float tapGain[kNumPinna + 1]  = {};
    int   numTaps = 0;
    float gain = 1.0f;                  // broadband level
    float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f;   // first-order shadow filter
    float rear = 0.0f;                  // treble cut for sources behind (0..1)
};

struct EarState
{
    float x1 = 0.0f, y1 = 0.0f, lp = 0.0f;
    void reset() { x1 = y1 = lp = 0.0f; }
};

struct Vec3 { float x = 0.0f, y = 0.0f, f = 1.0f; };

// Peaking EQ (RBJ cookbook), transposed direct form II.
struct Peak
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    void set(float hz, float q, float dB, float sampleRate)
    {
        hz = mpc::clamp(hz, 20.0f, 0.45f * sampleRate);
        const float A = std::pow(10.0f, dB / 40.0f);
        const float w = 2.0f * 3.14159265f * hz / sampleRate;
        const float alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
        const float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0; b1 = -2.0f * c / a0; b2 = (1.0f - alpha * A) / a0;
        a1 = -2.0f * c / a0;          a2 = (1.0f - alpha / A) / a0;
    }
    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0f; }
};

// Direction colour ("directional bands"): listeners hear a lift around 3 kHz as
// in front, a lift around 8 kHz as above and a dip there as below. Applied to
// each source before the head model, scaled by Intensity.
struct DirectionEq
{
    Peak front, high;
    void set(float fx, float up, float k, float sampleRate)
    {
        front.set(3200.0f, 0.9f, k * 6.0f * std::fmax(0.0f, fx) * (1.0f - std::fabs(up)), sampleRate);
        high.set(8000.0f, 1.0f, k * (9.0f * std::fmax(0.0f, up) - 7.0f * std::fmax(0.0f, -up)), sampleRate);
    }
    float process(float x) { return high.process(front.process(x)); }
    void reset() { front.reset(); high.reset(); }
};

inline Vec3 dirFromAngles(float azDeg, float elDeg)
{
    const float az = azDeg * kDegToRad, el = elDeg * kDegToRad;
    Vec3 v;
    v.x = std::cos(el) * std::sin(az);
    v.y = std::sin(el);
    v.f = std::cos(el) * std::cos(az);
    return v;
}

inline Vec3 normalise(Vec3 v, const Vec3& fallback)
{
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.f * v.f);
    if (l < 1.0e-3f) return fallback;
    v.x /= l; v.y /= l; v.f /= l;
    return v;
}

inline float itdSeconds(float x)
{
    const float lateral = std::fabs(std::asin(mpc::clamp(x, -1.0f, 1.0f)));
    return (kHeadA / kSound) * (lateral + std::sin(lateral));   // Woodworth
}

// Head-model parameters for one ear. side = +1 right ear, -1 left ear.
// nearField 0..1 adds level difference for sources closer than 1 m.
// k (Intensity, 0..1) exaggerates the cues: a bigger head (wider time gap), a
// deeper shadow and extra level difference, stronger ear echoes, darker rear.
void computeHeadEar(EarParams& p, const Vec3& d, float side, float sampleRate, float nearField, float k)
{
    const float srScale = sampleRate / 44100.0f;
    const float lx = mpc::clamp(d.x * side, -1.0f, 1.0f);     // +1 = facing this ear

    // Interaural time difference: only the far ear is delayed.
    p.delay = 2.0f + (lx < 0.0f ? itdSeconds(d.x) * (1.0f + 0.8f * k) * sampleRate : 0.0f);

    // Head shadow: alpha 2 (+6 dB treble) facing the ear, down to alpha_min
    // 150 degrees round the head. H(s) = (1 + alpha s T) / (1 + s T), bilinear.
    const float theta = std::acos(lx);
    const float aMin  = 0.1f * (1.0f - 0.6f * nearField) * (1.0f - 0.7f * k);
    float alpha = (1.0f + aMin * 0.5f) + (1.0f - aMin * 0.5f) * std::cos(theta * (180.0f / 150.0f));
    alpha = mpc::clamp(alpha, 0.05f, 2.0f);
    const float TK = (kHeadA / (2.0f * kSound)) * 2.0f * sampleRate;
    const float n  = 1.0f / (1.0f + TK);
    p.b0 = (1.0f + alpha * TK) * n;
    p.b1 = (1.0f - alpha * TK) * n;
    p.a1 = (1.0f - TK) * n;

    // Pinna echoes. phi folds the rear onto the front hemisphere (-90..90);
    // front/back is told apart by the rear treble cut below.
    float phi = std::atan2(d.y, d.f) / kDegToRad;   // 0 = front, 90 = up
    if (phi > 90.0f)  phi = 180.0f - phi;
    if (phi < -90.0f) phi = -180.0f - phi;
    const float earAz = std::asin(lx);              // + = toward this ear
    const float cosHalf = std::cos(earAz * 0.5f);
    const float s = std::sin(kPinnaD * (90.0f - phi) * kDegToRad);
    for (int i = 0; i < kNumPinna; ++i)
    {
        p.tapDelay[i] = std::fabs(kPinnaA[i] * cosHalf * s + kPinnaB[i]) * srScale;
        p.tapGain[i]  = kPinnaRho[i] * (kPinnaAmount + 0.35f * k);
    }
    // Shoulder echo: about 0.3..1.4 ms, fading out for sources below the ear.
    const float thS = earAz / kDegToRad;            // -90..90
    float tsh = 1.2f * (180.0f - thS) / 180.0f;
    const float q = (phi - 80.0f) * 180.0f / (180.0f + thS);
    tsh *= (1.0f - 0.00004f * q * q);
    tsh = mpc::clamp(tsh, 0.3f, 1.4f);
    p.tapDelay[kNumPinna] = tsh * 0.001f * sampleRate;
    p.tapGain[kNumPinna]  = 0.25f * mpc::clamp((phi + 45.0f) / 45.0f, 0.0f, 1.0f);
    p.numTaps = kNumPinna + 1;

    // Near-field level difference; darker treble for sources behind.
    // The +6 dB treble shelf facing the ear makes side sources louder overall;
    // 1 / (1 + 0.6|x|) keeps a pan from front to side roughly level.
    const float nearSide = std::fmax(0.0f, lx), farSide = std::fmax(0.0f, -lx);
    p.gain = 0.62f / (1.0f + (0.6f + 0.4f * k) * std::fabs(d.x))
           * (1.0f + nearField * (0.35f * nearSide - 0.5f * farSide))
           * (1.0f + k * (0.25f * nearSide - 0.55f * farSide));
    p.rear = (0.55f + 0.35f * k) * std::fmax(0.0f, -d.f);
}

// Lite: delay + level + a one-pole lowpass on the far ear.
void computeLiteEar(EarParams& p, const Vec3& d, float side, float sampleRate, float nearField, float k)
{
    const float lx = mpc::clamp(d.x * side, -1.0f, 1.0f);
    p.delay = 2.0f + (lx < 0.0f ? itdSeconds(d.x) * (1.0f + 0.8f * k) * sampleRate : 0.0f);

    const float shade = std::fmax(0.0f, -lx);       // 0 near ear .. 1 opposite
    float fc = 20000.0f * std::pow(0.12f, shade);
    fc = mpc::clamp(fc, 200.0f, 0.45f * sampleRate);
    const float c = 1.0f - std::exp(-2.0f * kPi * fc / sampleRate);
    p.b0 = c; p.b1 = 0.0f; p.a1 = -(1.0f - c);

    p.numTaps = 0;
    const float ild = 0.3f + 0.25f * nearField + 0.35f * k;
    p.gain = 0.535f * std::fmax(0.05f, 1.0f + ild * lx);
    p.rear = (0.5f + 0.35f * k) * std::fmax(0.0f, -d.f);
}

// ---------------------------------------------------------------------------
// Rotating sound-field delay: four delay lines (W, X, Y, F), the field rotated
// and lowpassed inside the feedback loop. Buffers are sized in prepare().
// ---------------------------------------------------------------------------
struct EchoLoop
{
    std::vector<float> buf[4];
    int   mask = 0, pos = 0;
    float lp[4] = {};

    void prepare(int maxDelay)
    {
        int n = 1;
        while (n < maxDelay + 4) n <<= 1;
        for (auto& b : buf) b.assign((size_t)n, 0.0f);
        mask = n - 1;
        reset();
    }
    void reset()
    {
        for (auto& b : buf) std::fill(b.begin(), b.end(), 0.0f);
        for (float& v : lp) v = 0.0f;
        pos = 0;
    }
    int maxDelay() const { return mask - 3; }

    // in/out: W, X (right), Y (up), F (front). out = the repeats, each already
    // turned at least once.
    void process(const float in[4], float delay, const float R[3][3], float fb, float dampC, float out[4])
    {
        const int   di = (int)delay;
        const float fr = delay - (float)di;
        float d[4];
        for (int c = 0; c < 4; ++c)
        {
            const float a = buf[c][(size_t)((pos - di) & mask)];
            const float b = buf[c][(size_t)((pos - di - 1) & mask)];
            d[c] = a + (b - a) * fr;
        }
        const float r[4] = { d[0],
                             R[0][0] * d[1] + R[0][1] * d[2] + R[0][2] * d[3],
                             R[1][0] * d[1] + R[1][1] * d[2] + R[1][2] * d[3],
                             R[2][0] * d[1] + R[2][1] * d[2] + R[2][2] * d[3] };
        pos = (pos + 1) & mask;
        for (int c = 0; c < 4; ++c)
        {
            lp[c] += dampC * (r[c] - lp[c]);
            out[c] = lp[c];
            buf[c][(size_t)pos] = in[c] + fb * lp[c];
        }
    }
};

// Rotation: yaw by az, then pitch by el (degrees). Rows x, y, f; columns are
// where right, up and front end up.
inline void rotationMatrix(float azDeg, float elDeg, float R[3][3])
{
    const float ya = azDeg * kDegToRad, pa = elDeg * kDegToRad;
    const float cy = std::cos(ya), sy = std::sin(ya), cp = std::cos(pa), sp = std::sin(pa);
    const float M[3][3] = {
        {  cy, -sp * sy, cp * sy },
        { 0.0f,      cp,      sp },
        { -sy, -sp * cy, cp * cy } };
    std::memcpy(R, M, sizeof(M));
}

// ---------------------------------------------------------------------------
// Small late reverb: 4-line feedback delay network, Householder mixing.
// ---------------------------------------------------------------------------
struct Fdn
{
    static constexpr int kLines = 4;
    static constexpr int kSize  = 8192;
    float buf[kLines][kSize] = {};
    int   len[kLines] = { 1310, 1637, 1811, 1931 };
    int   pos = 0;
    float g[kLines] = {};
    float damp[kLines] = {};
    float dampC = 0.5f;

    void prepare(float sr)
    {
        const float ms[kLines] = { 29.7f, 37.1f, 41.1f, 43.7f };
        for (int i = 0; i < kLines; ++i)
            len[i] = mpc::clamp((int)(ms[i] * 0.001f * sr), 16, kSize - 1);
        for (int i = 0; i < kLines; ++i)
            g[i] = std::pow(10.0f, -3.0f * ((float)len[i] / sr) / 0.9f);   // RT60 0.9 s
        const float hz = std::fmin(6000.0f, 0.45f * sr);
        dampC = 1.0f - std::exp(-2.0f * kPi * hz / sr);
        reset();
    }
    void reset() { std::memset(buf, 0, sizeof(buf)); std::memset(damp, 0, sizeof(damp)); pos = 0; }

    void process(float in, float& outL, float& outR)
    {
        float y[kLines];
        for (int i = 0; i < kLines; ++i)
        {
            const float v = buf[i][(pos - len[i]) & (kSize - 1)];
            damp[i] += dampC * (v - damp[i]);
            y[i] = damp[i] * g[i];
        }
        const float sum = 0.5f * (y[0] + y[1] + y[2] + y[3]);
        pos = (pos + 1) & (kSize - 1);
        for (int i = 0; i < kLines; ++i)
            buf[i][pos] = in + (y[i] - sum);
        outL = y[0] - y[2];
        outR = y[1] - y[3];
    }
};

} // namespace

class BinauralEcho : public mpc::Plugin
{
public:
    BinauralEcho() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("volume",    "Volume",    -24.0f,  12.0f,   0.0f, "dB", 1));
        addParam(mpc::floatParam("azimuth",   "Azimuth",  -180.0f, 180.0f,   0.0f, "",   0));
        addParam(mpc::floatParam("elevation", "Elevation", -90.0f,  90.0f,   0.0f, "",   0));
        addParam(mpc::logParam  ("distance",  "Distance",    0.2f,  10.0f,   1.0f, "m",  2));
        addParam(mpc::floatParam("mix",       "Mix",         0.0f, 100.0f, 100.0f, "%",  0));
        addParam(mpc::choiceParam("mode",     "Mode", { "Head", "Field", "Lite" }, kModeHead));
        addParam(mpc::floatParam("width",     "Width",       0.0f, 100.0f,  40.0f, "%",  0));
        addParam(mpc::floatParam("room",      "Room",        0.0f, 100.0f,  35.0f, "%",  0));
        addParam(mpc::floatParam("air",       "Air",         0.0f, 100.0f,  50.0f, "%",  0));
        addParam(mpc::choiceParam("motion",   "Motion", { "Off", "Orbit", "Swing", "Figure 8", "Spiral", "Fly-by" }, kMotionOff));
        addParam(mpc::logParam  ("rate",      "Rate",        0.02f,  4.0f,   0.25f, "Hz", 2));
        addParam(mpc::boolParam ("sync",      "Sync",        false));
        addParam(mpc::choiceParam("division", "Division", { "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8" }, 3));
        addParam(mpc::floatParam("depth",     "Depth",       0.0f, 100.0f,  50.0f, "%",  0));
        addParam(mpc::boolParam ("reverse",   "Reverse",     false));
        // Read-only display copies of the moving position (same ranges as the
        // originals, so the screen's film strips map one to one).
        {
            mpc::Param a = mpc::floatParam("show_az",   "~Az",   -180.0f, 180.0f, 0.0f, "", 0);
            mpc::Param e = mpc::floatParam("show_el",   "~El",    -90.0f,  90.0f, 0.0f, "", 0);
            mpc::Param d = mpc::logParam  ("show_dist", "~Dist",    0.2f,  10.0f, 1.0f, "m", 2);
            a.automatable = e.automatable = d.automatable = false;
            addParam(a); addParam(e); addParam(d);
        }
        addParam(mpc::floatParam("intensity", "Intensity",   0.0f, 100.0f,  70.0f, "%",  0));
        addParam(mpc::floatParam("echo",      "Echo",        0.0f, 100.0f,   0.0f, "%",  0));
        addParam(mpc::logParam  ("echo_time", "Echo Time",  20.0f, 2000.0f, 375.0f, "ms", 0));
        addParam(mpc::boolParam ("echo_sync", "Echo Sync",   false));
        addParam(mpc::choiceParam("echo_div", "Echo Div",
                                  { "1/32", "1/16", "1/16 D", "1/8", "1/8 D", "1/4", "1/4 D", "1/2", "1 Bar" }, 5));
        addParam(mpc::floatParam("feedback",  "Feedback",    0.0f,  95.0f,  55.0f, "%",  0));
        addParam(mpc::floatParam("step",      "Step",     -180.0f, 180.0f,  90.0f, "",   0));
        addParam(mpc::floatParam("tilt",      "Tilt",      -45.0f,  45.0f,   0.0f, "",   0));
        addParam(mpc::floatParam("damping",   "Damping",     0.0f, 100.0f,  40.0f, "%",  0));

        // Programs. Motion defaults to off; the motion presets set it.
        struct Pre { const char* name; float az, el, dist, mode, width, room, air;
                     float motion, rate, sync, div, depth, rev;
                     float echo, etime, esync, ediv, fb, step, tilt, damp; };
        #define NO_ECHO 0.0f, 375.0f, 0, 5, 55.0f, 90.0f, 0.0f, 40.0f
        // Order: moving with echoes, moving, static, static with echoes.
        const Pre pres[] = {
            //  name               az      el    dist   mode   width  room   air    motion      rate  sync div depth rev
            //                     echo%   ms     sync div  fdbk   step    tilt  damp
            // --- moving, with echoes ---
            { "Comet Orbit",        0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 20.0f, 50.0f, kOrbit,   0.20f, 0, 3, 20.0f, 0,
                                   45.0f, 375.0f, 1, 1, 65.0f,    0.0f,  0.0f, 45.0f },
            { "Swirl Trail",        0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 20.0f, 50.0f, kOrbit,   0.25f, 1, 3, 20.0f, 0,
                                   45.0f, 375.0f, 1, 3, 60.0f,  -45.0f,  0.0f, 40.0f },
            { "Pendulum Echo",      0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 20.0f, 50.0f, kSwing,   0.25f, 1, 2, 70.0f, 0,
                                   45.0f, 375.0f, 1, 4, 55.0f,  180.0f,  0.0f, 40.0f },
            { "Figure 8 Trails",    0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 25.0f, 50.0f, kFigure8, 0.15f, 0, 3, 70.0f, 0,
                                   40.0f, 375.0f, 1, 1, 70.0f,    0.0f,  0.0f, 50.0f },
            { "Spiral Galaxy",      0.0f,  0.0f, 1.5f, kModeField, 20.0f, 30.0f, 50.0f, kSpiral,  0.20f, 0, 3, 80.0f, 0,
                                   40.0f, 375.0f, 1, 5, 70.0f,   30.0f, 15.0f, 35.0f },
            { "Fly-by Wake",        0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 35.0f, 70.0f, kFlyBy,   0.25f, 1, 1, 70.0f, 0,
                                   40.0f, 375.0f, 1, 3, 60.0f,    0.0f,  0.0f, 55.0f },
            { "Counter Spin",       0.0f,  0.0f, 0.9f, kModeHead,   0.0f, 15.0f, 40.0f, kOrbit,   0.25f, 1, 4,  0.0f, 1,
                                   45.0f, 375.0f, 1, 3, 55.0f,   90.0f,  0.0f, 40.0f },
            { "Halo",               0.0f, 20.0f, 1.5f, kModeHead,  10.0f, 25.0f, 50.0f, kOrbit,   0.30f, 0, 3, 60.0f, 0,
                                   40.0f, 120.0f, 0, 5, 75.0f,   20.0f, 10.0f, 35.0f },
            { "Orbit Ping-Pong",    0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 20.0f, 50.0f, kOrbit,   0.10f, 0, 3,  0.0f, 0,
                                   45.0f, 375.0f, 1, 5, 50.0f,  180.0f,  0.0f, 40.0f },
            // --- moving ---
            { "Slow Orbit",         0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 25.0f, 50.0f, kOrbit,   0.10f, 0, 3,  0.0f, 0, NO_ECHO },
            { "Orbit 1 Bar",        0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 20.0f, 50.0f, kOrbit,   0.25f, 1, 3, 20.0f, 0, NO_ECHO },
            { "Tilted Orbit",       0.0f, 10.0f, 1.5f, kModeHead,  10.0f, 25.0f, 50.0f, kOrbit,   0.15f, 0, 3, 70.0f, 1, NO_ECHO },
            { "Spin 1/4",           0.0f,  0.0f, 0.8f, kModeHead,   0.0f, 15.0f, 40.0f, kOrbit,   1.00f, 1, 5,  0.0f, 0, NO_ECHO },
            { "Swing 2 Bars",       0.0f,  0.0f, 1.0f, kModeHead,  20.0f, 20.0f, 50.0f, kSwing,   0.25f, 1, 2, 60.0f, 0, NO_ECHO },
            { "Figure 8",           0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 25.0f, 50.0f, kFigure8, 0.15f, 0, 3, 70.0f, 0, NO_ECHO },
            { "Spiral",             0.0f,  0.0f, 1.5f, kModeField, 20.0f, 30.0f, 50.0f, kSpiral,  0.25f, 0, 3, 80.0f, 0, NO_ECHO },
            { "Fly-by",             0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 35.0f, 70.0f, kFlyBy,   0.25f, 1, 1, 70.0f, 0, NO_ECHO },
            // --- static ---
            { "Front & Centre",     0.0f,  0.0f, 1.0f, kModeHead,  40.0f, 20.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Hard Right",        90.0f,  0.0f, 0.6f, kModeHead,  10.0f, 15.0f, 40.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Behind You",       180.0f,  0.0f, 1.2f, kModeHead,  30.0f, 25.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Overhead",           0.0f, 60.0f, 1.5f, kModeHead,  30.0f, 25.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Whisper",          -70.0f,  0.0f, 0.25f,kModeHead,   0.0f,  5.0f, 20.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Far Field",        -35.0f,  5.0f, 7.0f, kModeField, 60.0f, 50.0f, 80.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Wide Field",         0.0f,  0.0f, 2.0f, kModeField,100.0f, 30.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            { "Light Pan",         45.0f,  0.0f, 1.0f, kModeLite,  40.0f, 20.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0, NO_ECHO },
            // --- static, with echoes ---
            { "Circle 1/4",         0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 20.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0,
                                   50.0f, 375.0f, 1, 5, 60.0f,   90.0f,  0.0f, 35.0f },
            { "Front-Back Bounce",  0.0f,  0.0f, 1.0f, kModeHead,  10.0f, 20.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0,
                                   50.0f, 375.0f, 1, 4, 55.0f,  180.0f,  0.0f, 40.0f },
            { "Tilted Circle",      0.0f,  0.0f, 1.2f, kModeHead,  10.0f, 25.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0,
                                   45.0f, 375.0f, 1, 3, 70.0f,   60.0f, 25.0f, 30.0f },
            { "Slow Creep",       -30.0f,  0.0f, 1.5f, kModeField, 30.0f, 30.0f, 50.0f, kMotionOff, 0.25f, 0, 3, 50.0f, 0,
                                   40.0f, 450.0f, 0, 5, 75.0f,   25.0f,  0.0f, 50.0f },
        };
        #undef NO_ECHO
        // Close-up presets are loud in the near ear; their Volume compensates.
        auto presetVolume = [](const char* n) {
            return std::strcmp(n, "Whisper") == 0 ? -10.0f : std::strcmp(n, "Hard Right") == 0 ? -5.0f : 0.0f; };
        for (const Pre& p : pres)
            addProgram(p.name, { presetVolume(p.name), p.az, p.el, p.dist, 100.0f, p.mode, p.width, p.room, p.air,
                                 p.motion, p.rate, p.sync, p.div, p.depth, p.rev,
                                 0.0f, 0.0f, 1.0f,    // display copies (overwritten at once)
                                 70.0f,               // Intensity
                                 p.echo, p.etime, p.esync, p.ediv, p.fb, p.step, p.tilt, p.damp });
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = (float)sampleRate;
        rearLP_ = 1.0f - std::exp(-2.0f * kPi * std::fmin(4000.0f, 0.45f * sr_) / sr_);
        fdn_.prepare(sr_);
        erMax_ = std::min((int)(0.08f * sr_), 8192 - 4);

        // Field mode: eight virtual speakers on a cube (+/-45, +/-135 degrees
        // azimuth, +/-35.26 degrees elevation), each with a fixed head model.
        const float el = 35.264f;
        const float azs[4] = { 45.0f, 135.0f, -135.0f, -45.0f };
        int k = 0;
        for (int up = 0; up < 2; ++up)
            for (int a = 0; a < 4; ++a, ++k)
            {
                spkDir_[k] = dirFromAngles(azs[a], up ? el : -el);
                (void)k;
            }
        // Max-rE weighted basic decode, normalised to unit energy per source.
        const Vec3 t = dirFromAngles(0.0f, 0.0f);
        float e = 0.0f;
        for (int s = 0; s < kNumSpk; ++s)
        {
            const float dot = spkDir_[s].x * t.x + spkDir_[s].y * t.y + spkDir_[s].f * t.f;
            const float gk = 1.0f + 3.0f * kMaxRE * dot;
            e += gk * gk;
        }
        decNorm_ = 1.0f / std::sqrt(std::fmax(e, 1.0e-6f));
        spkIntensity_ = -1.0f;   // speaker ears are computed in computeControls()
        echo_.prepare((int)(kEchoMaxSeconds * sr_));
        reset();
    }

    void reset() override
    {
        for (SourceRing& r : srcRing_) r.reset();
        for (SourceRing& r : spkRing_) r.reset();
        for (auto& s : srcEarSt_) for (EarState& e : s) e.reset();
        for (auto& s : spkEarSt_) for (EarState& e : s) e.reset();
        for (SourceRing& r : echoRing_) r.reset();
        for (auto& s : echoEarSt_) for (EarState& e : s) e.reset();
        echo_.reset();
        echoLevel_ = get(kEcho) * 0.01f;
        echoDelay_ = -1.0f;      // snaps to the target on the first block
        airL_ = airR_ = 0.0f;
        for (DirectionEq& q : srcEq_) q.reset();
        er_.reset();
        fdn_.reset();
        volume_ = mpc::dsp::dbToGain(get(kVolume));
        mix_ = get(kMix) * 0.01f;
        mode_ = mpc::clamp(getInt(kMode), 0, 2);
        modeGain_ = 1.0f;
        distLog_ = std::log(mpc::clamp(get(kDistance), 0.2f, 10.0f));
        dir_ = dirFromAngles(get(kAzimuth), get(kElevation));
        room_ = get(kRoom) * 0.01f;
        phase_ = 0.0;
        computeTarget(0.0);
        dir_ = dirFromAngles(tgtAz_, tgtEl_);
        distLog_ = std::log(tgtDist_);
        computeControls();
        copyCurrentToPrev();
        // The framework calls reset() after a preset or state load, which sets
        // values silently; the MPC screen only redraws on host notifications.
        reportAllPending_.store(true, std::memory_order_release);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const int nch = mpc::clamp(ctx.numOutputs, 0, 2);
        if (nch == 0) return;

        const float* inL = nullptr;
        const float* inR = nullptr;
        if (ctx.inputs != nullptr && ctx.numInputs > 0)
        {
            inL = ctx.inputs[0];
            inR = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];
        }

        const float volT  = mpc::dsp::dbToGain(get(kVolume));
        const float mixT  = get(kMix) * 0.01f;
        const int   modeT = mpc::clamp(getInt(kMode), 0, 2);
        const float smooth = 1.0f - std::exp(-1.0f / (0.012f * sr_));   // per sample, 12 ms

        // Motion clock, in cycles. Synced and playing: locked to the song position.
        const bool   sync = getBool(kSync);
        const double cycleQn = kDivisionQn[mpc::clamp(getInt(kDivision), 0, 6)];
        const double bpm = (ctx.transport.tempoValid && ctx.transport.bpm > 1.0) ? ctx.transport.bpm : 120.0;
        const double cyclesPerSample = sync ? (bpm / 60.0) / cycleQn / (double)sr_
                                            : (double)get(kRate) / (double)sr_;
        if (sync && ctx.transport.playing && ctx.transport.ppqValid)
            phase_ = ctx.transport.ppqPos / cycleQn;

        // Echo controls for this block.
        const float echoT = get(kEcho) * 0.01f;
        float echoDelayT = getBool(kEchoSync)
            ? (float)(kEchoDivQn[mpc::clamp(getInt(kEchoDiv), 0, kNumEchoDiv - 1)] * 60.0 / bpm) * sr_
            : get(kEchoTime) * 0.001f * sr_;
        echoDelayT = mpc::clamp(echoDelayT, 1.0f, (float)echo_.maxDelay());
        if (echoDelay_ < 0.0f) echoDelay_ = echoDelayT;
        const float echoGlide = 1.0f - std::exp(-1.0f / (0.080f * sr_));
        echoFb_ = get(kFeedback) * 0.01f;
        {
            const float dmp = get(kDamping) * 0.01f;
            const float fc = std::fmin(600.0f + 16000.0f * (1.0f - 0.9f * dmp) * (1.0f - 0.9f * dmp), 0.45f * sr_);
            echoDampC_ = 1.0f - std::exp(-2.0f * kPi * fc / sr_);
        }
        rotationMatrix(get(kStep), get(kTilt), echoRot_);

        int done = 0;
        while (done < n)
        {
            const int len = std::min(kSub, n - done);

            // ----- controls, once per sub-block; ears interpolate prev -> cur -----
            copyCurrentToPrev();
            phase_ += cyclesPerSample * (double)len;
            if (phase_ > 1.0e6) phase_ -= std::floor(phase_);
            computeTarget(phase_);
            stepControls(len);
            computeControls();

            for (int i = 0; i < len; ++i)
            {
                const int idx = done + i;
                // Read both inputs before writing (buffers may be shared).
                const float xl = inL ? inL[idx] : 0.0f;
                const float xr = inR ? inR[idx] : xl;
                const float t  = (float)(i + 1) / (float)len;

                volume_ += smooth * (volT - volume_);
                mix_    += smooth * (mixT - mix_);
                echoLevel_ += smooth * (echoT - echoLevel_);
                echoDelay_ += echoGlide * (echoDelayT - echoDelay_);

                // Distance gain + air absorption.
                const float g = mpc::dsp::lerp(prevCtl_.gain, curCtl_.gain, t);
                airL_ += curCtl_.airC * (xl * g - airL_);
                airR_ += curCtl_.airC * (xr * g - airR_);

                float wl = 0.0f, wr = 0.0f;
                if (mode_ == kModeField) renderField(airL_, airR_, wl, wr);
                else                     renderSources(airL_, airR_, t, wl, wr);

                float rl = 0.0f, rr = 0.0f;
                roomSample(0.5f * (xl + xr), t, rl, rr);
                wl = wl * modeGain_ + rl;
                wr = wr * modeGain_ + rr;

                ctx.outputs[0][idx] = (xl + (wl - xl) * mix_) * volume_;
                if (nch > 1) ctx.outputs[1][idx] = (xr + (wr - xr) * mix_) * volume_;
            }

            // Mode change: fade the binaural part out, switch, fade back in (~10 ms each).
            const float fadeStep = (float)len / (0.010f * sr_);
            if (modeT != mode_)
            {
                modeGain_ -= fadeStep;
                if (modeGain_ <= 0.0f)
                {
                    modeGain_ = 0.0f;
                    mode_ = modeT;
                    for (auto& s : srcEarSt_) for (EarState& e : s) e.reset();
                    for (auto& s : spkEarSt_) for (EarState& e : s) e.reset();
                    computeControls();
                    copyCurrentToPrev();
                }
            }
            else if (modeGain_ < 1.0f)
            {
                modeGain_ = std::fmin(1.0f, modeGain_ + fadeStep);
            }

            done += len;
        }

        // After a preset/state load: tell the MPC every setting changed.
        if (reportAllPending_.exchange(false, std::memory_order_acq_rel))
            for (int k = 0; k < kNumParams; ++k)
                if (!isDisplayParam(k)) notifyHostParameterChanged(k);

        publishPosition(n);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[32];
        switch (index)
        {
            case kAzimuth:
            {
                const int d = (int)std::lround(plain);
                if (d == 0) out = "Front";
                else if (d == 180 || d == -180) out = "Back";
                else { std::snprintf(buf, sizeof(buf), "%s %d", d > 0 ? "R" : "L", d > 0 ? d : -d); out = buf; }
                return true;
            }
            case kElevation:
            {
                const int d = (int)std::lround(plain);
                if (d == 0) out = "Level";
                else { std::snprintf(buf, sizeof(buf), "%s %d", d > 0 ? "Up" : "Down", d > 0 ? d : -d); out = buf; }
                return true;
            }
            case kDistance:
                if (plain < 0.995f)     std::snprintf(buf, sizeof(buf), "%.2f m", (double)plain);
                else if (plain < 9.95f) std::snprintf(buf, sizeof(buf), "%.1f m", (double)plain);
                else                    std::snprintf(buf, sizeof(buf), "%.0f m", (double)plain);
                out = buf;
                return true;
            case kVolume:
                std::snprintf(buf, sizeof(buf), "%.1f dB", (double)plain);
                out = buf;
                return true;
            case kRate:
                std::snprintf(buf, sizeof(buf), plain < 1.0f ? "%.2f Hz" : "%.1f Hz", (double)plain);
                out = buf;
                return true;
            case kEchoTime:
                std::snprintf(buf, sizeof(buf), "%d ms", (int)std::lround(plain));
                out = buf;
                return true;
            case kStep:
            {
                const int d = (int)std::lround(plain);
                if (d == 0) out = "None";
                else if (d == 180 || d == -180) out = "Back";
                else { std::snprintf(buf, sizeof(buf), "%s %d", d > 0 ? "R" : "L", d > 0 ? d : -d); out = buf; }
                return true;
            }
            case kTilt:     return formatValue(kElevation, plain, out);
            case kShowAz:   return formatValue(kAzimuth, plain, out);
            case kShowEl:   return formatValue(kElevation, plain, out);
            case kShowDist: return formatValue(kDistance, plain, out);
            case kMix: case kWidth: case kRoom: case kAir: case kDepth: case kIntensity:
            case kEcho: case kFeedback: case kDamping:
                std::snprintf(buf, sizeof(buf), "%d%%", (int)std::lround(plain));
                out = buf;
                return true;
            default:
                return false;
        }
    }

private:
    static constexpr int   kNumSpk = 8;
    static constexpr float kMaxRE  = 0.577f;   // first-order max-rE weight (3D)
    static constexpr int   kNumER  = 6;

    // Controls shared by all modes, one value per sub-block.
    struct Controls
    {
        float gain = 1.0f;     // distance gain
        float airC = 1.0f;     // air lowpass coefficient (1 = open)
        float erScale = 1.0f;  // early reflection time scale
        float room = 0.0f;
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Binaural Echo";
        info.vendor       = "Lightlike";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 441000;         // echoes at high feedback (~10 s at 44.1 kHz)
        return info;
    }

    static constexpr char kUid[5] = "LvgW";

    // Where the source should be: the set position plus the motion path at
    // phase (in cycles). Writes tgtAz_ / tgtEl_ (degrees) and tgtDist_ (m).
    void computeTarget(double phase)
    {
        float az = get(kAzimuth), el = get(kElevation), dist = mpc::clamp(get(kDistance), 0.2f, 10.0f);
        const int   motion = mpc::clamp(getInt(kMotion), 0, 5);
        const float depth = get(kDepth) * 0.01f;
        const float dir = getBool(kReverse) ? -1.0f : 1.0f;
        const float ph = (float)(phase - std::floor(phase));
        const float w = 2.0f * kPi * ph;
        switch (motion)
        {
            case kOrbit:     // full turns; Depth tilts the circle (high one side, low the other)
                az += dir * 360.0f * ph;
                el += depth * 45.0f * std::sin(w);
                break;
            case kSwing:     // to and fro, up to +/-120 degrees
                az += dir * depth * 120.0f * std::sin(w);
                break;
            case kFigure8:   // side to side and up/down twice as fast
                az += dir * depth * 120.0f * std::sin(w);
                el += depth * 45.0f * std::sin(2.0f * w);
                break;
            case kSpiral:    // turns while slowly rising and falling (one rise per 4 turns)
            {
                const double slow = phase * 0.25 - std::floor(phase * 0.25);
                az += dir * 360.0f * ph;
                el += depth * 75.0f * std::sin(2.0f * kPi * (float)slow);
                break;
            }
            case kFlyBy:     // passes by on a straight line, closest at Distance, and back
            {
                const float travel = (2.0f + 8.0f * depth) * -std::cos(w) * dir;
                az += std::atan2(travel, dist) / kDegToRad;
                dist = std::sqrt(dist * dist + travel * travel);
                break;
            }
            default: break;
        }
        while (az > 180.0f)  az -= 360.0f;
        while (az < -180.0f) az += 360.0f;
        tgtAz_ = az;
        tgtEl_ = mpc::clamp(el, -90.0f, 90.0f);
        tgtDist_ = mpc::clamp(dist, 0.2f, 10.0f);
    }

    // Report the moving position to the screen (~Az, ~El, ~Dist), at most
    // ~30 times a second and only when a picture would change frame.
    void publishPosition(int frames)
    {
        sinceSend_ += frames;
        if ((float)sinceSend_ < sr_ / 30.0f) return;
        const float az = std::atan2(dir_.x, dir_.f) / kDegToRad;
        const float el = std::asin(mpc::clamp(dir_.y, -1.0f, 1.0f)) / kDegToRad;
        const float vals[3] = { az, el, std::exp(distLog_) };
        bool sent = false;
        for (int k = 0; k < 3; ++k)
        {
            const int idx = kShowAz + k;
            const float norm = param(idx).toNormalised(vals[k]);
            if (std::fabs(norm - getNormalised(idx)) >= 0.5f / 63.0f)
            {
                setNormalised(idx, norm);
                notifyHostParameterChanged(idx);
                sent = true;
            }
        }
        if (sent) sinceSend_ = 0;
    }

    void copyCurrentToPrev()
    {
        prevCtl_ = curCtl_;
        for (int s = 0; s < 2; ++s)
            for (int e = 0; e < 2; ++e) prevEar_[s][e] = curEar_[s][e];
    }

    // Glide the direction (as a unit vector, so 180 -> -180 does not spin
    // round), the distance (in log space) and the room amount, ~30 ms.
    void stepControls(int len)
    {
        const float c = 1.0f - std::exp(-(float)len / (0.030f * sr_));
        const Vec3 tgt = dirFromAngles(tgtAz_, tgtEl_);
        Vec3 v;
        v.x = dir_.x + c * (tgt.x - dir_.x);
        v.y = dir_.y + c * (tgt.y - dir_.y);
        v.f = dir_.f + c * (tgt.f - dir_.f);
        dir_ = normalise(v, tgt);
        distLog_ += c * (std::log(tgtDist_) - distLog_);
        room_ += c * (get(kRoom) * 0.01f - room_);
    }

    void computeControls()
    {
        const float r = std::exp(distLog_);
        const float k = get(kIntensity) * 0.01f;
        const float nearField = mpc::clamp((1.0f - r) / 0.8f, 0.0f, 1.0f);
        // Level with distance; Intensity steepens the curve (+6 dB at 0.2 m,
        // -21 dB at 10 m at the default 70 %).
        curCtl_.gain = std::fmin(std::pow(1.0f / (0.5f + 0.5f * r), 1.0f + 0.6f * k), 1.6f);   // at most +4 dB up close
        // Keep the front level steady as Intensity adds presence and ear echoes.
        const float trimDb = (mode_ == kModeField ? -3.9f : mode_ == kModeLite ? -1.6f : -3.0f) * k;
        curCtl_.gain *= std::pow(10.0f, trimDb / 20.0f);
        const float air = get(kAir) * 0.01f * (1.0f + k);
        float fc = 20000.0f / (1.0f + 0.9f * air * std::fmax(0.0f, r - 0.2f));
        fc = std::fmin(fc, 0.45f * sr_);
        curCtl_.airC = fc >= 19000.0f ? 1.0f : 1.0f - std::exp(-2.0f * kPi * fc / sr_);
        curCtl_.erScale = 0.55f + 0.45f * mpc::clamp(1.0f / r, 0.0f, 1.0f);
        curCtl_.room = room_;

        // Virtual speaker ears (Field mode and the echoes).
        if (std::fabs(k - spkIntensity_) > 1.0e-4f)
        {
            spkIntensity_ = k;
            for (int s = 0; s < kNumSpk; ++s)
            {
                computeHeadEar(spkEar_[s][0], spkDir_[s], -1.0f, sr_, 0.0f, k);
                computeHeadEar(spkEar_[s][1], spkDir_[s],  1.0f, sr_, 0.0f, k);
                // Echo decode (Head / Lite): same ears without the ear and shoulder
                // echoes, which cost most and add little to a repeat.
                for (int e = 0; e < 2; ++e)
                {
                    echoEar_[s][e] = spkEar_[s][e];
                    echoEar_[s][e].numTaps = 0;
                }
            }
        }

        const float spread = get(kWidth) * 0.01f * 45.0f;
        const float az = std::atan2(dir_.x, dir_.f) / kDegToRad;
        const float el = std::asin(mpc::clamp(dir_.y, -1.0f, 1.0f)) / kDegToRad;

        if (mode_ == kModeField)
        {
            // Rotation: yaw by azimuth, then pitch by elevation.
            // Rows x, y, f; columns are where right, up and front end up.
            const float ya = az * kDegToRad, pa = el * kDegToRad;
            const float cy = std::cos(ya), sy = std::sin(ya), cp = std::cos(pa), sp = std::sin(pa);
            const float R[3][3] = {
                {  cy, -sp * sy, cp * sy },
                { 0.0f,      cp,      sp },
                { -sy, -sp * cy, cp * cy } };
            std::memcpy(rot_, R, sizeof(R));
            fieldSin_ = std::sin(spread * kDegToRad);
            fieldCos_ = std::cos(spread * kDegToRad);
            // Direction colour for each source where the rotation puts it.
            for (int s = 0; s < 2; ++s)
            {
                const float x0 = s == 0 ? -fieldSin_ : fieldSin_;
                const float fy = R[1][0] * x0 + R[1][2] * fieldCos_;
                const float ff = R[2][0] * x0 + R[2][2] * fieldCos_;
                srcEq_[s].set(ff, fy, k, sr_);
            }
            return;
        }

        // Head / Lite: the left input at -spread, the right at +spread.
        for (int s = 0; s < 2; ++s)
        {
            const Vec3 d = dirFromAngles(az + (s == 0 ? -spread : spread), el);
            srcDir_[s] = d;
            srcEq_[s].set(d.f, d.y, k, sr_);
            for (int e = 0; e < 2; ++e)
            {
                const float side = e == 0 ? -1.0f : 1.0f;
                if (mode_ == kModeLite) computeLiteEar(curEar_[s][e], d, side, sr_, nearField, k);
                else                    computeHeadEar(curEar_[s][e], d, side, sr_, nearField, k);
            }
        }
    }

    // One ear: taps from the ring, shadow filter, rear treble cut, level.
    inline float earSample(const SourceRing& ring, const EarParams& a, const EarParams& b, float t, EarState& st)
    {
        const float base = a.delay + (b.delay - a.delay) * t;
        float x = ring.read(base);
        for (int k = 0; k < b.numTaps; ++k)
        {
            const float d = a.tapDelay[k] + (b.tapDelay[k] - a.tapDelay[k]) * t;
            const float g = a.tapGain[k] + (b.tapGain[k] - a.tapGain[k]) * t;
            x += g * ring.read(base + d);
        }
        float y = b.b0 * x + b.b1 * st.x1 - b.a1 * st.y1;
        st.x1 = x;
        st.y1 = y;
        st.lp += rearLP_ * (y - st.lp);
        const float rear = a.rear + (b.rear - a.rear) * t;
        y -= rear * (y - st.lp);
        return y * (a.gain + (b.gain - a.gain) * t);
    }

    // Fixed ear (Field mode's virtual speakers): no interpolation.
    inline float earSampleFixed(const SourceRing& ring, const EarParams& p, EarState& st)
    {
        float x = ring.read(p.delay);
        for (int k = 0; k < p.numTaps; ++k) x += p.tapGain[k] * ring.read(p.delay + p.tapDelay[k]);
        float y = p.b0 * x + p.b1 * st.x1 - p.a1 * st.y1;
        st.x1 = x;
        st.y1 = y;
        st.lp += rearLP_ * (y - st.lp);
        y -= p.rear * (y - st.lp);
        return y * p.gain;
    }

    // Head / Lite: two point sources (input L and R).
    inline void renderSources(float sl, float sr, float t, float& outL, float& outR)
    {
        srcRing_[0].write(srcEq_[0].process(sl));
        srcRing_[1].write(srcEq_[1].process(sr));
        outL = outR = 0.0f;
        for (int s = 0; s < 2; ++s)
        {
            outL += earSample(srcRing_[s], prevEar_[s][0], curEar_[s][0], t, srcEarSt_[s][0]);
            outR += earSample(srcRing_[s], prevEar_[s][1], curEar_[s][1], t, srcEarSt_[s][1]);
        }

        // Echoes: encode both sources where they are, run the rotating loop,
        // decode through the virtual speakers (skipped while Echo is at 0).
        const Vec3& a = srcDir_[0];
        const Vec3& b = srcDir_[1];
        const float in[4] = { sl + sr, sl * a.x + sr * b.x, sl * a.y + sr * b.y, sl * a.f + sr * b.f };
        float e[4];
        echo_.process(in, echoDelay_, echoRot_, echoFb_, echoDampC_, e);
        if (echoLevel_ > 1.0e-4f)
        {
            for (float& v : e) v *= echoLevel_;
            float el = 0.0f, er = 0.0f;
            decodeCube(e, echoRing_, echoEar_, echoEarSt_, el, er);
            outL += el;
            outR += er;
        }
    }

    // Decode a first-order field (W, X, Y, F) to the cube of virtual speakers
    // and the head model. Max-rE weighted, level-matched to Head mode.
    inline void decodeCube(const float f[4], SourceRing* rings, const EarParams (*ears)[2], EarState (*st)[2],
                           float& outL, float& outR)
    {
        const float w = 3.0f * kMaxRE;
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < kNumSpk; ++k)
        {
            const Vec3& d = spkDir_[k];
            rings[k].write(decNorm_ * (f[0] + w * (d.x * f[1] + d.y * f[2] + d.f * f[3])));
            l += earSampleFixed(rings[k], ears[k][0], st[k][0]);
            r += earSampleFixed(rings[k], ears[k][1], st[k][1]);
        }
        outL = l * kFieldTrim;
        outR = r * kFieldTrim;
    }

    // Field: encode L/R into first-order B-format (SN3D, W gain 1) at -/+ spread
    // on the horizon, rotate the field, decode to the cube of virtual speakers.
    inline void renderField(float sl, float sr, float& outL, float& outR)
    {
        sl = srcEq_[0].process(sl);
        sr = srcEq_[1].process(sr);
        const float W  = sl + sr;
        const float X0 = (sr - sl) * fieldSin_;   // right
        const float F0 = (sl + sr) * fieldCos_;   // front (up is 0 before rotation)
        const float X = rot_[0][0] * X0 + rot_[0][2] * F0;
        const float Y = rot_[1][0] * X0 + rot_[1][2] * F0;
        const float F = rot_[2][0] * X0 + rot_[2][2] * F0;

        // The echoes run on the same field; add them before decoding.
        float f[4] = { W, X, Y, F };
        float e[4];
        echo_.process(f, echoDelay_, echoRot_, echoFb_, echoDampC_, e);
        for (int c = 0; c < 4; ++c) f[c] += echoLevel_ * e[c];
        decodeCube(f, spkRing_, spkEar_, spkEarSt_, outL, outR);
    }

    // Early reflections (6 taps, alternating sides) feeding a late reverb.
    // Far sources: reflections come sooner after the direct sound.
    inline void roomSample(float x, float t, float& outL, float& outR)
    {
        const float room  = mpc::dsp::lerp(prevCtl_.room, curCtl_.room, t);
        const float scale = mpc::dsp::lerp(prevCtl_.erScale, curCtl_.erScale, t);
        er_.write(x);
        static constexpr float kErMs[kNumER]   = { 5.3f, 9.7f, 14.1f, 19.9f, 27.3f, 34.6f };
        static constexpr float kErGain[kNumER] = { 0.55f, 0.48f, 0.40f, 0.33f, 0.27f, 0.21f };
        float el = 0.0f, er = 0.0f;
        for (int k = 0; k < kNumER; ++k)
        {
            const float d = mpc::clamp(kErMs[k] * 0.001f * sr_ * scale, 1.0f, (float)erMax_);
            const float v = er_.read(d) * kErGain[k];
            if (k & 1) er += v; else el += v;
        }
        float ll, lr;
        fdn_.process((el + er) * 0.35f, ll, lr);
        outL = room * (0.45f * el + 0.5f * ll);
        outR = room * (0.45f * er + 0.5f * lr);
    }

    static constexpr float kFieldTrim = 0.765f;   // level-matches Field to Head (front, 1 m)

    float sr_ = 44100.0f;
    float rearLP_ = 0.4f;

    SourceRing srcRing_[2];
    EarParams  prevEar_[2][2], curEar_[2][2];
    EarState   srcEarSt_[2][2];

    SourceRing spkRing_[kNumSpk];
    Vec3       spkDir_[kNumSpk];
    EarParams  spkEar_[kNumSpk][2];
    EarState   spkEarSt_[kNumSpk][2];
    float      decNorm_ = 0.2f;
    float      spkIntensity_ = -1.0f;
    DirectionEq srcEq_[2];
    Vec3        srcDir_[2];

    EchoLoop   echo_;
    SourceRing echoRing_[kNumSpk];
    EarState   echoEarSt_[kNumSpk][2];
    EarParams  echoEar_[kNumSpk][2];
    float      echoRot_[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float      echoLevel_ = 0.0f, echoDelay_ = -1.0f, echoFb_ = 0.55f, echoDampC_ = 0.5f;
    float      rot_[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float      fieldSin_ = 0.3f, fieldCos_ = 0.95f;

    Ring<8192> er_;          // early reflections
    int        erMax_ = 3528;
    Fdn        fdn_;

    Controls prevCtl_, curCtl_;
    Vec3  dir_;
    float distLog_ = 0.0f;
    float room_ = 0.25f;
    float airL_ = 0.0f, airR_ = 0.0f;
    float volume_ = 1.0f, mix_ = 1.0f, modeGain_ = 1.0f;
    int   mode_ = kModeHead;
    std::atomic<bool> reportAllPending_ { false };
    double phase_ = 0.0;     // motion clock, in cycles
    float  tgtAz_ = 0.0f, tgtEl_ = 0.0f, tgtDist_ = 1.0f;
    int    sinceSend_ = 0;
};

MPC_PLUGIN_ENTRY(BinauralEcho)
