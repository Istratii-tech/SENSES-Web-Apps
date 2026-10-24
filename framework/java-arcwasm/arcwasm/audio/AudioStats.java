// arc-wasm: what the in-process mixer (shim/audio/audio_flinger.h) has done,
// for tests and demos: frames mixed and played, whether the page's Web Audio
// output exists and runs (it starts with the first user gesture), worklet
// underruns, the peak of the last played block. Topic gles-audio.
package arcwasm.audio;

public final class AudioStats {
    /** Frames (44.1 kHz) the mixer produced. */
    public final long mixed;
    /** Frames the output played (the page's AudioWorklet, or the clock). */
    public final long played;
    /** Whether the page has Web Audio (else a clock consumes the output). */
    public final boolean webAudio;
    /** Whether the page's AudioContext runs (after a user gesture). */
    public final boolean running;
    public final int underruns;
    /** Peak of the last block the worklet played, 0..32767. */
    public final int peak;

    private AudioStats(long[] v) {
        mixed = v[0];
        played = v[1];
        webAudio = v[2] != 0;
        running = v[3] != 0;
        underruns = (int) v[4];
        peak = (int) v[5];
    }

    public static AudioStats get() {
        long[] v = new long[6];
        nativeGet(v);
        return new AudioStats(v);
    }

    @Override
    public String toString() {
        return "mixed " + mixed + ", played " + played + ", "
                + (webAudio ? (running ? "Web Audio running" : "Web Audio suspended") : "no device")
                + ", underruns " + underruns + ", peak " + peak;
    }

    private static native void nativeGet(long[] out);
}
