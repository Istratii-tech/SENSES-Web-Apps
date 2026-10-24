package arcwasm;

import java.util.HashMap;

/**
 * CPU-bound kernels for measuring the interpreter (docs/topics/dexopt.md).
 * Prints "bench NAME: N ms" per kernel and "Bench: total N ms"; each kernel
 * runs once to warm up (class init, verification) and then is timed.
 * Optional argument: repetition count of the timed runs (default 3, the best
 * run is reported).
 */
public class Bench {
    static int sink;

    interface Kernel {
        int run();
    }

    // --- method calls and virtual dispatch --------------------------------
    abstract static class Shape {
        abstract int area();
        int perimeter() { return 0; }
    }
    static final class Square extends Shape {
        final int s;
        Square(int s) { this.s = s; }
        int area() { return s * s; }
        int perimeter() { return 4 * s; }
    }
    static final class Rect extends Shape {
        final int w, h;
        Rect(int w, int h) { this.w = w; this.h = h; }
        int area() { return w * h; }
        int perimeter() { return 2 * (w + h); }
    }
    interface Op { int apply(int a, int b); }
    static final class Add implements Op { public int apply(int a, int b) { return a + b; } }
    static final class Xor implements Op { public int apply(int a, int b) { return a ^ b; } }

    private static int addStatic(int a, int b) { return a + b; }

    static int calls() {
        Shape[] shapes = { new Square(3), new Rect(2, 5), new Square(7), new Rect(4, 1) };
        Op[] ops = { new Add(), new Xor() };
        int acc = 0;
        for (int i = 0; i < 300000; i++) {
            Shape s = shapes[i & 3];
            acc += s.area() + s.perimeter();
            acc = ops[i & 1].apply(acc, i);
            acc = addStatic(acc, 1);
        }
        return acc;
    }

    // --- field access -------------------------------------------------------
    static final class Particle {
        int x, y, vx, vy;
        long energy;
    }
    static int staticCounter;

    static int fields() {
        Particle p = new Particle();
        p.vx = 3; p.vy = -2;
        for (int i = 0; i < 400000; i++) {
            p.x += p.vx;
            p.y += p.vy;
            if (p.x > 1000 || p.x < 0) p.vx = -p.vx;
            if (p.y > 1000 || p.y < 0) p.vy = -p.vy;
            p.energy += p.vx * p.vx + p.vy * p.vy;
            staticCounter++;
        }
        return (int) (p.energy + p.x + p.y + staticCounter);
    }

    // --- array loops ---------------------------------------------------------
    static int arrays() {
        int[] a = new int[4096];
        for (int i = 0; i < a.length; i++) a[i] = (i * 2654435761L) > 0 ? i * 31 : i;
        int acc = 0;
        for (int round = 0; round < 160; round++) {
            for (int i = 1; i < a.length; i++) {
                a[i] = a[i - 1] + (a[i] ^ round);
                acc += a[i] & 0xff;
            }
        }
        // insertion sort of a small array
        int[] b = new int[600];
        for (int i = 0; i < b.length; i++) b[i] = (i * 7919) % 1000;
        for (int i = 1; i < b.length; i++) {
            int v = b[i], j = i - 1;
            while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; }
            b[j + 1] = v;
        }
        return acc + b[0] + b[b.length - 1];
    }

    // --- string building ----------------------------------------------------
    static int strings() {
        int acc = 0;
        for (int i = 0; i < 12000; i++) {
            StringBuilder sb = new StringBuilder();
            sb.append("item-").append(i).append(':');
            for (int j = 0; j < 10; j++) sb.append((char) ('a' + (i + j) % 26));
            String s = sb.toString();
            acc += s.length() + s.indexOf(':') + (s.equals("item-0:abcdefghij") ? 1 : 0)
                    + s.charAt(s.length() - 1);
        }
        return acc;
    }

    // --- HashMap --------------------------------------------------------------
    static int hashmap() {
        HashMap<Integer, Integer> m = new HashMap<Integer, Integer>();
        for (int i = 0; i < 20000; i++) m.put(i * 7, i);
        int acc = 0;
        for (int i = 0; i < 40000; i++) {
            Integer v = m.get(i);
            if (v != null) acc += v;
        }
        HashMap<String, Integer> s = new HashMap<String, Integer>();
        for (int i = 0; i < 2000; i++) s.put("k" + i, i);
        for (int i = 0; i < 2000; i++) acc += s.get("k" + i);
        return acc;
    }

    // --- float loop: a tiny ray-sphere tracer ---------------------------------
    static final class Vec {
        final float x, y, z;
        Vec(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
        Vec sub(Vec o) { return new Vec(x - o.x, y - o.y, z - o.z); }
        float dot(Vec o) { return x * o.x + y * o.y + z * o.z; }
        Vec scale(float k) { return new Vec(x * k, y * k, z * k); }
        Vec norm() { return scale(1f / (float) Math.sqrt(dot(this))); }
    }

    static int raytrace() {
        Vec center = new Vec(0f, 0f, -3f);
        Vec light = new Vec(1f, 1f, 1f).norm();
        Vec eye = new Vec(0f, 0f, 0f);
        float radius = 1f;
        int w = 192, h = 144, lit = 0;
        float shade = 0f;
        for (int py = 0; py < h; py++) {
            for (int px = 0; px < w; px++) {
                Vec dir = new Vec((px - w / 2f) / w, (py - h / 2f) / w, -1f).norm();
                Vec oc = eye.sub(center);
                float b = oc.dot(dir);
                float c = oc.dot(oc) - radius * radius;
                float disc = b * b - c;
                if (disc > 0f) {
                    float t = -b - (float) Math.sqrt(disc);
                    Vec hit = dir.scale(t).sub(center.scale(-1f));
                    Vec n = hit.sub(center).norm();
                    float d = n.dot(light);
                    if (d > 0f) { shade += d; lit++; }
                }
            }
        }
        return lit + (int) shade;
    }

    /** Milliseconds with one decimal. */
    private static String ms(long ns) {
        long tenths = (ns + 50000) / 100000;
        return (tenths / 10) + "." + (tenths % 10);
    }

    private static long timeIt(Kernel k, int reps) {
        sink += k.run();  // warm-up: class init, verification, first resolution
        long best = Long.MAX_VALUE;
        for (int r = 0; r < reps; r++) {
            long t0 = System.nanoTime();
            sink += k.run();
            long t = System.nanoTime() - t0;
            if (t < best) best = t;
        }
        return best;
    }

    public static void main(String[] args) {
        int reps = args.length > 0 ? Integer.parseInt(args[0]) : 3;
        String[] names = { "calls", "fields", "arrays", "strings", "hashmap", "raytrace" };
        Kernel[] kernels = {
            new Kernel() { public int run() { return calls(); } },
            new Kernel() { public int run() { return fields(); } },
            new Kernel() { public int run() { return arrays(); } },
            new Kernel() { public int run() { return strings(); } },
            new Kernel() { public int run() { return hashmap(); } },
            new Kernel() { public int run() { return raytrace(); } },
        };
        long total = 0;
        for (int i = 0; i < kernels.length; i++) {
            long ns = timeIt(kernels[i], reps);
            total += ns;
            System.out.println("bench " + names[i] + ": " + ms(ns) + " ms");
        }
        System.out.println("Bench: total " + ms(total) + " ms (best of " + reps
                + ", sink " + (sink & 0xff) + ")");
    }
}
