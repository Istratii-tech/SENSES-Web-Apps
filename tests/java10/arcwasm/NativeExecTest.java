package arcwasm;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;

import libcore.io.Memory;

/**
 * Topic native-exec (docs/topics/native-exec.md): what an app needs to unpack
 * and run native programs of its own, on /data (OPFS):
 *   - symbolic links (relative targets, links to directories, listing,
 *     rename, unlink, rmdir of a directory that holds one) and their
 *     persistence across a restart of the VM;
 *   - permission bits (chmod, File.setExecutable, a file created with
 *     execute bits), kept across a restart;
 *   - 4 KB pages: Os.sysconf, mmap at file offsets in 4 KB steps (/data and
 *     /system; MAP_SHARED writes reach the file);
 *   - running ARM and arm64 executables (ProcessBuilder / Runtime.exec):
 *     stdin/stdout/stderr pipes, the exit code, waitFor, destroy.
 * Two VM runs: the second is a restart (exit 75: tools/run_java_tests.sh
 * starts the VM again with the same storage) that finds what the first kept.
 */
public class NativeExecTest {
    private static final String DIR = "/data/local/tmp/native-exec";
    private static final String PHASE = "/data/local/tmp/native-exec-phase";
    // Test programs (mk/topic-native-exec.mk) in the /system image.
    private static final String PROGS = "/system/etc/arcwasm/native-exec";
    private static int passed;
    private static int failed;
    private static int skipped;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    static class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    private static void write(String path, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(path)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String read(String path) {
        try (InputStream in = new java.io.FileInputStream(path)) {
            return new String(readAll(in), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                try {
                    if (OsConstants.S_ISLNK(Os.lstat(k.getPath()).st_mode)) {
                        k.delete();
                        continue;
                    }
                } catch (ErrnoException e) {
                    // gone
                }
                deleteTree(k);
            }
        }
        f.delete();
    }

    private static int perms(String path) throws ErrnoException {
        return Os.stat(path).st_mode & 07777;
    }

    private static List<String> list(String dir) {
        String[] names = new File(dir).list();
        List<String> l = new ArrayList<>(names == null ? new ArrayList<String>() : Arrays.asList(names));
        java.util.Collections.sort(l);
        return l;
    }

    // --- phase 1: symlinks, modes, pages -------------------------------------------

    private static void phase1() {
        check("Os.sysconf(_SC_PAGESIZE) is 4096", new Check() {
            public void run() throws Exception {
                expectEq(4096L, Os.sysconf(OsConstants._SC_PAGESIZE));
            }
        });
        check("symlink with a relative target, readlink, lstat, stat through it", new Check() {
            public void run() throws Exception {
                new File(DIR + "/root/system/etc").mkdirs();
                new File(DIR + "/root/proc").mkdirs();
                write(DIR + "/root/proc/mounts", "rootfs / rootfs ro 0 0\n");
                // As a firmware tree unpacked by an app has it.
                Os.symlink("../../proc/mounts", DIR + "/root/system/etc/mtab");
                expectEq("../../proc/mounts", Os.readlink(DIR + "/root/system/etc/mtab"));
                StructStat l = Os.lstat(DIR + "/root/system/etc/mtab");
                expect(OsConstants.S_ISLNK(l.st_mode), "lstat mode " + Integer.toOctalString(l.st_mode));
                expectEq((long) "../../proc/mounts".length(), l.st_size);
                expect(OsConstants.S_ISREG(Os.stat(DIR + "/root/system/etc/mtab").st_mode), "stat follows it");
                expectEq("rootfs / rootfs ro 0 0\n", read(DIR + "/root/system/etc/mtab"));
                expect(list(DIR + "/root/system/etc").contains("mtab"), "listed: " + list(DIR + "/root/system/etc"));
            }
        });
        check("symlink: EEXIST, dangling links, links to directories and absolute paths", new Check() {
            public void run() throws Exception {
                try {
                    Os.symlink("x", DIR + "/root/system/etc/mtab");
                    throw new AssertionError("no EEXIST");
                } catch (ErrnoException e) {
                    expectEq(OsConstants.EEXIST, e.errno);
                }
                Os.symlink("nowhere", DIR + "/root/dangling");
                expectEq("nowhere", Os.readlink(DIR + "/root/dangling"));
                expect(!new File(DIR + "/root/dangling").exists(), "a dangling link does not exist()");
                Os.symlink(DIR + "/root/proc", DIR + "/root/procdir");
                expectEq("rootfs / rootfs ro 0 0\n", read(DIR + "/root/procdir/mounts"));
                expect(new File(DIR + "/root/procdir").isDirectory(), "a link to a directory isDirectory()");
                // A link inside a link's target, and a chain of links.
                Os.symlink("procdir/mounts", DIR + "/root/chain");
                expectEq("rootfs / rootfs ro 0 0\n", read(DIR + "/root/chain"));
                // Writing through a link writes its target.
                write(DIR + "/root/procdir/new", "made through a link");
                expectEq("made through a link", read(DIR + "/root/proc/new"));
                // Links out of /data (the /system image).
                Os.symlink("/system/build.prop", DIR + "/root/build.prop");
                expect(read(DIR + "/root/build.prop").contains("ro."), "/system through a link");
            }
        });
        check("symlink: rename moves the link, unlink removes only the link, rmdir", new Check() {
            public void run() throws Exception {
                Os.rename(DIR + "/root/dangling", DIR + "/root/dangling2");
                expectEq("nowhere", Os.readlink(DIR + "/root/dangling2"));
                try {
                    Os.lstat(DIR + "/root/dangling");
                    throw new AssertionError("old name still there");
                } catch (ErrnoException e) {
                    expectEq(OsConstants.ENOENT, e.errno);
                }
                Os.unlink(DIR + "/root/procdir");
                expect(new File(DIR + "/root/proc/mounts").exists(), "the target survives unlink of the link");
                new File(DIR + "/root/onlylink").mkdirs();
                Os.symlink("x", DIR + "/root/onlylink/l");
                try {
                    Os.remove(DIR + "/root/onlylink");
                    throw new AssertionError("rmdir of a directory with a link");
                } catch (ErrnoException e) {
                    expectEq(OsConstants.ENOTEMPTY, e.errno);
                }
                Os.unlink(DIR + "/root/onlylink/l");
                Os.remove(DIR + "/root/onlylink");
                // A directory renamed with its links.
                new File(DIR + "/root/d1").mkdirs();
                Os.symlink("../proc/mounts", DIR + "/root/d1/m");
                Os.rename(DIR + "/root/d1", DIR + "/root/d2");
                expectEq("../proc/mounts", Os.readlink(DIR + "/root/d2/m"));
                expectEq("rootfs / rootfs ro 0 0\n", read(DIR + "/root/d2/m"));
            }
        });
        check("chmod keeps permission bits: File.setExecutable, Os.chmod, fchmod, creat 0755", new Check() {
            public void run() throws Exception {
                String f = DIR + "/tool";
                write(f, "#!/system/bin/sh\n");
                File file = new File(f);
                expect(!file.canExecute(), "a new file is not executable");
                expect(file.setExecutable(true, true), "setExecutable");
                expect(file.canExecute(), "canExecute after setExecutable");
                expect((perms(f) & 0100) != 0, "mode " + Integer.toOctalString(perms(f)));
                Os.chmod(f, 0640);
                expectEq(0640, perms(f));
                expect(!file.canExecute(), "canExecute after chmod 0640");
                FileDescriptor fd = Os.open(DIR + "/made", OsConstants.O_CREAT | OsConstants.O_WRONLY, 0755);
                Os.close(fd);
                expectEq(0755, perms(DIR + "/made"));
                fd = Os.open(DIR + "/made", OsConstants.O_RDWR, 0);
                Os.fchmod(fd, 0711);
                expectEq(0711, Os.fstat(fd).st_mode & 07777);
                Os.close(fd);
                Os.chmod(DIR + "/root/system", 0751);
                expectEq(0751, perms(DIR + "/root/system"));
                // Through a link: the target's mode.
                Os.symlink("../made", DIR + "/root/made-link");
                expectEq(0711, perms(DIR + "/root/made-link"));
            }
        });
        check("mmap at file offsets in 4 KB steps (/data: private and shared)", new Check() {
            public void run() throws Exception {
                String f = DIR + "/pages";
                byte[] data = new byte[3 * 65536];
                for (int i = 0; i < data.length; i++) {
                    data[i] = (byte) (i / 4096 * 7 + i);
                }
                try (FileOutputStream out = new FileOutputStream(f)) {
                    out.write(data);
                }
                FileDescriptor fd = Os.open(f, OsConstants.O_RDWR, 0);
                for (int page : new int[] {1, 5, 17, 31}) {
                    long a = Os.mmap(0, 8192, OsConstants.PROT_READ, OsConstants.MAP_PRIVATE, fd, page * 4096L);
                    for (int i = 0; i < 8192; i += 509) {
                        expectEq(data[page * 4096 + i], Memory.peekByte(a + i));
                    }
                    Os.munmap(a, 8192);
                }
                long a = Os.mmap(0, 4096, OsConstants.PROT_READ | OsConstants.PROT_WRITE, OsConstants.MAP_SHARED,
                        fd, 3 * 4096L);
                Memory.pokeByte(a + 10, (byte) 0x5a);
                Os.munmap(a, 4096);
                Os.close(fd);
                try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
                    r.seek(3 * 4096 + 10);
                    expectEq(0x5a, r.read());
                }
                try {
                    fd = Os.open(f, OsConstants.O_RDONLY, 0);
                    Os.mmap(0, 4096, OsConstants.PROT_READ, OsConstants.MAP_PRIVATE, fd, 1000);
                    throw new AssertionError("an offset off the 4 KB pages is taken");
                } catch (ErrnoException e) {
                    expectEq(OsConstants.EINVAL, e.errno);
                } finally {
                    Os.close(fd);
                }
            }
        });
        check("mmap at a 4 KB offset of a /system file (a copy)", new Check() {
            public void run() throws Exception {
                String f = "/system/framework/arc-tests.jar";
                byte[] all;
                try (InputStream in = new java.io.FileInputStream(f)) {
                    all = readAll(in);
                }
                expect(all.length > 3 * 4096, "size " + all.length);
                FileDescriptor fd = Os.open(f, OsConstants.O_RDONLY, 0);
                long a = Os.mmap(0, 8192, OsConstants.PROT_READ, OsConstants.MAP_PRIVATE, fd, 4096);
                for (int i = 0; i < 8192 && 4096 + i < all.length; i += 251) {
                    expectEq(all[4096 + i], Memory.peekByte(a + i));
                }
                Os.munmap(a, 8192);
                Os.close(fd);
            }
        });
    }

    // --- running native programs -----------------------------------------------------

    static class Result {
        String out;
        String err;
        int code;

        public String toString() {
            return "code " + code + ", out <" + out + ">, err <" + err + ">";
        }
    }

    // Runs |pb| to its end: stdin from |in| (closed after), stdout and stderr
    // read by threads of their own.
    private static Result run(ProcessBuilder pb, final byte[] in) throws Exception {
        final Process p = pb.start();
        final Result r = new Result();
        final byte[][] outs = new byte[2][];
        Thread to = new Thread(() -> {
            try {
                outs[0] = readAll(p.getInputStream());
            } catch (IOException e) {
                outs[0] = ("<" + e + ">").getBytes();
            }
        });
        Thread te = new Thread(() -> {
            try {
                outs[1] = readAll(p.getErrorStream());
            } catch (IOException e) {
                outs[1] = ("<" + e + ">").getBytes();
            }
        });
        to.start();
        te.start();
        try (OutputStream o = p.getOutputStream()) {
            if (in != null) o.write(in);
        }
        expect(p.waitFor(60, TimeUnit.SECONDS), "the program did not end in 60 s");
        to.join(10000);
        te.join(10000);
        r.code = p.exitValue();
        r.out = outs[0] == null ? null : new String(outs[0], StandardCharsets.UTF_8);
        r.err = outs[1] == null ? null : new String(outs[1], StandardCharsets.UTF_8);
        return r;
    }

    private static Result run(String... cmd) throws Exception {
        return run(new ProcessBuilder(cmd), null);
    }

    private static void needProgs() {
        if (!new File(PROGS + "/arm64/exectest").exists()) throw new Skip("no " + PROGS + " in this /system");
    }

    private static void execChecks() {
        for (final String abi : new String[] {"arm64", "armeabi-v7a"}) {
            final String prog = PROGS + "/" + abi + "/exectest";
            check(abi + " executable: arguments to stdout, exit code 0 (ProcessBuilder)", new Check() {
                public void run() throws Exception {
                    needProgs();
                    Result r = NativeExecTest.run(prog, "echo", "hello", "native", "world");
                    expectEq("hello native world\n", r.out);
                    expectEq(0, r.code);
                }
            });
            check(abi + ": stdin to stdout through pipes, stderr apart", new Check() {
                public void run() throws Exception {
                    needProgs();
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < 2000; i++) sb.append("line ").append(i).append('\n');
                    byte[] in = sb.toString().getBytes(StandardCharsets.UTF_8);
                    Result r = NativeExecTest.run(new ProcessBuilder(prog, "cat"), in);
                    expectEq(sb.toString(), r.out);
                    expectEq("cat: " + in.length + " bytes\n", r.err);
                    expectEq(0, r.code);
                }
            });
            check(abi + ": exit codes (return, exit(), abort())", new Check() {
                public void run() throws Exception {
                    needProgs();
                    expectEq(42, NativeExecTest.run(prog, "exit", "42").code);
                    expectEq(5, NativeExecTest.run(prog, "exitcall", "5").code);
                    Result r = NativeExecTest.run(prog, "stderr");
                    expectEq(3, r.code);
                    expectEq("to stdout\n", r.out);
                    expectEq("to stderr\n", r.err);
                    expectEq(0x80 + 6, NativeExecTest.run(prog, "abort").code);
                }
            });
        }
        final String prog = PROGS + "/arm64/exectest";
        check("the environment, working directory and redirectErrorStream of ProcessBuilder", new Check() {
            public void run() throws Exception {
                needProgs();
                ProcessBuilder pb = new ProcessBuilder(prog, "env", "NE_VAR");
                pb.environment().put("NE_VAR", "from Java");
                expectEq("NE_VAR=from Java\n", NativeExecTest.run(pb, null).out);
                pb = new ProcessBuilder(prog, "cwd").directory(new File(DIR));
                expectEq(DIR + "\n", NativeExecTest.run(pb, null).out);
                pb = new ProcessBuilder(prog, "file", "written-here", "by the program").directory(new File(DIR));
                expectEq(0, NativeExecTest.run(pb, null).code);
                expectEq("by the program", read(DIR + "/written-here"));
                pb = new ProcessBuilder(prog, "stderr").redirectErrorStream(true);
                Result r = NativeExecTest.run(pb, null);
                expect(r.out.contains("to stdout\n") && r.out.contains("to stderr\n"), r.toString());
                expectEq("", r.err);
            }
        });
        check("pid and ppid of a program; programs at the same time", new Check() {
            public void run() throws Exception {
                needProgs();
                Result r = NativeExecTest.run(prog, "pid");
                String[] w = r.out.trim().split(" ");
                expectEq("ppid", w[2]);
                expectEq(String.valueOf(Os.getpid()), w[3]);
                expect(!w[1].equals(String.valueOf(Os.getpid())), "its own pid: " + r.out);
                List<Process> ps = new ArrayList<>();
                for (int i = 0; i < 3; i++) ps.add(new ProcessBuilder(prog, "exit", String.valueOf(10 + i)).start());
                for (int i = 0; i < 3; i++) {
                    expect(ps.get(i).waitFor(60, TimeUnit.SECONDS), "program " + i + " did not end");
                    expectEq(10 + i, ps.get(i).exitValue());
                }
            }
        });
        for (final String abi : new String[] {"arm64", "armeabi-v7a"}) {
            check(abi + ": execv in a program (the same process, its environment and pipes)", new Check() {
                public void run() throws Exception {
                    needProgs();
                    String p = PROGS + "/" + abi + "/exectest";
                    Result r = NativeExecTest.run(p, "exec", p, "env", "EXECED");
                    expectEq("EXECED=yes\n", r.out);
                    expectEq(0, r.code);
                    // a program of the other ABI, and the exit code of the new one
                    String other = PROGS + "/" + (abi.equals("arm64") ? "armeabi-v7a" : "arm64") + "/exectest";
                    expectEq(9, NativeExecTest.run(p, "exec", other, "exit", "9").code);
                    r = NativeExecTest.run(p, "exec", DIR + "/no-such-program");
                    expect(r.out.startsWith("execv: "), r.toString());
                    expectEq(6, r.code);
                }
            });
        }
        check("a program that dup2()s onto and closes every inherited descriptor", new Check() {
            public void run() throws Exception {
                needProgs();
                Result r = NativeExecTest.run(prog, "closefds");
                expectEq(0, r.code);
                expectEq("closed, reopened as 3\n", r.out);
                // The VM's own descriptors are still there (/system's image, the test's jar).
                expect(read("/system/build.prop").contains("ro."), "/system after the program");
                expectEq("hello native world\n", NativeExecTest.run(prog, "echo", "hello", "native", "world").out);
            }
        });
        check("binfmt_misc: a registered interpreter runs a matching file", new Check() {
            public void run() throws Exception {
                needProgs();
                final String interp = PROGS + "/arm64/exectest";
                // Register a magic rule (as qemu-user registers for ARM ELFs):
                // files beginning 0xCA 0xFE 0xBA 0xBE run through exectest.
                String rule = ":arctest:M:0:\\xca\\xfe\\xba\\xbe::" + interp + ":";
                try (FileOutputStream r = new FileOutputStream("/proc/sys/fs/binfmt_misc/register")) {
                    r.write(rule.getBytes(StandardCharsets.UTF_8));
                } catch (IOException e) {
                    throw new Skip("no /proc/sys/fs/binfmt_misc: " + e);
                }
                expect(read("/proc/sys/fs/binfmt_misc/status").startsWith("enabled"), "status not enabled");
                // A file of that format, marked executable.
                String prog = DIR + "/prog.arctest";
                try (FileOutputStream o = new FileOutputStream(prog)) {
                    o.write(new byte[] {(byte) 0xca, (byte) 0xfe, (byte) 0xba, (byte) 0xbe, 1, 2, 3, 4});
                }
                expect(new File(prog).setExecutable(true), "setExecutable");
                Result res = NativeExecTest.run(prog, "ignored-arg");
                // binfmt ran exectest with the matched file as argv[1].
                expect(res.out != null && res.out.contains("exectest got: " + prog), res.toString());
                // A file without the magic is not matched (runs as itself: ENOEXEC).
                String plain = DIR + "/plain.arctest";
                write(plain, "not a program\n");
                new File(plain).setExecutable(true);
                try {
                    new ProcessBuilder(plain).start();
                    throw new AssertionError("plain file started");
                } catch (IOException e) {
                    expect(String.valueOf(e.getMessage()).contains("error=" + OsConstants.ENOEXEC + ","), e.toString());
                }
            }
        });
        check("fork + exec + waitpid from within a program", new Check() {
            public void run() throws Exception {
                needProgs();
                Result r = NativeExecTest.run(PROGS + "/arm64/exectest", "spawnself", "x", "7");
                // fork+exec+waitpid: the child (self) execs "exit 7"; the parent reaps 7.
                expect(r.out != null && r.out.contains("child exited 7"), r.toString());
                expectEq(0, r.code);
            }
        });
        check("QEMU_LD_PREFIX: a guest-absolute exec path runs from under the root", new Check() {
            public void run() throws Exception {
                needProgs();
                // A guest tree whose "/" lives under a root directory (what
                // qemu-user's -L / QEMU_LD_PREFIX names): a copy of exectest at
                // <root>/gbin/tool, reachable only as the guest path /gbin/tool.
                String root = DIR + "/guestroot";
                new File(root + "/gbin").mkdirs();
                byte[] image;
                try (InputStream in = new java.io.FileInputStream(PROGS + "/arm64/exectest")) {
                    image = readAll(in);
                }
                try (FileOutputStream out = new FileOutputStream(root + "/gbin/tool")) {
                    out.write(image);
                }
                expect(new File(root + "/gbin/tool").setExecutable(true), "setExecutable");
                // A process that carries the root execs the guest-absolute path,
                // which is not in our VFS: it runs the copy under the root. The
                // root is carried the standard way, as QEMU_LD_PREFIX.
                ProcessBuilder pb = new ProcessBuilder(
                        PROGS + "/arm64/exectest", "exec", "/gbin/tool", "exit", "9");
                pb.environment().put("QEMU_LD_PREFIX", root);
                Result res = NativeExecTest.run(pb, null);
                expectEq(9, res.code);
                // Without the root, the same guest-absolute path is not found.
                try {
                    NativeExecTest.run(new ProcessBuilder("/gbin/tool"), null);
                    throw new AssertionError("ran without root");
                } catch (IOException e) {
                    expect(String.valueOf(e.getMessage()).contains("error=" + OsConstants.ENOENT + ","),
                           e.toString());
                }
                // The root also comes from a -L DIR on the command line (how
                // qemu-user is launched), carried into the process and inherited
                // by what it execs: "exectest qemu -L <root> /gbin/tool exit 11"
                // execs /gbin/tool from under <root> with no QEMU_LD_PREFIX set.
                Result r2 = NativeExecTest.run(
                        PROGS + "/arm64/exectest", "qemu", "-L", root, "/gbin/tool", "exit", "11");
                expectEq(11, r2.code);
            }
        });
        check("a static arm64 program (static-pie, raw syscalls)", new Check() {
            public void run() throws Exception {
                needProgs();
                Result r = NativeExecTest.run(PROGS + "/arm64/static64", "argument");
                expectEq("static: argument\n", r.out);
                expectEq(7, r.code);
            }
        });
        check("a program unpacked to /data: chmod +x, through a symlink, Runtime.exec", new Check() {
            public void run() throws Exception {
                needProgs();
                new File(DIR + "/bin").mkdirs();
                byte[] image;
                try (InputStream in = new java.io.FileInputStream(prog)) {
                    image = readAll(in);
                }
                try (FileOutputStream out = new FileOutputStream(DIR + "/bin/tool")) {
                    out.write(image);
                }
                expect(new File(DIR + "/bin/tool").setExecutable(true), "setExecutable");
                Os.symlink("tool", DIR + "/bin/tool-link");
                Process p = Runtime.getRuntime().exec(new String[] {DIR + "/bin/tool-link", "echo", "from /data"});
                expectEq("from /data\n", new String(readAll(p.getInputStream()), StandardCharsets.UTF_8));
                expectEq(0, p.waitFor());
            }
        });
        check("errors as exec has them: no such file (ENOENT), not a program (ENOEXEC)", new Check() {
            public void run() throws Exception {
                // (the VM's errno numbers: OsConstants)
                try {
                    new ProcessBuilder(DIR + "/no-such-program").start();
                    throw new AssertionError("started");
                } catch (IOException e) {
                    expect(String.valueOf(e.getMessage()).contains("error=" + OsConstants.ENOENT + ","),
                            e.toString());
                }
                write(DIR + "/not-a-program", "plain text\n");
                new File(DIR + "/not-a-program").setExecutable(true);
                try {
                    new ProcessBuilder(DIR + "/not-a-program").start();
                    throw new AssertionError("started");
                } catch (IOException e) {
                    expect(String.valueOf(e.getMessage()).contains("error=" + OsConstants.ENOEXEC + ","),
                            e.toString());
                }
            }
        });
        check("Process.destroy() ends a program for its parent", new Check() {
            public void run() throws Exception {
                needProgs();
                Process p = new ProcessBuilder(prog, "sleep", "100").start();
                Thread.sleep(300);
                expect(p.isAlive(), "ended by itself");
                p.destroy();
                expect(p.waitFor(20, TimeUnit.SECONDS), "not ended 20 s after destroy()");
                expectEq(0x80 + 15, p.exitValue());
            }
        });
    }

    // --- phase 2: what phase 1 kept --------------------------------------------------

    private static void phase2() {
        check("symlinks are still there after a restart of the VM", new Check() {
            public void run() throws Exception {
                expectEq("../../proc/mounts", Os.readlink(DIR + "/root/system/etc/mtab"));
                expectEq("rootfs / rootfs ro 0 0\n", read(DIR + "/root/system/etc/mtab"));
                expectEq("nowhere", Os.readlink(DIR + "/root/dangling2"));
                expectEq("../proc/mounts", Os.readlink(DIR + "/root/d2/m"));
                expect(list(DIR + "/root").contains("chain"), "listed: " + list(DIR + "/root"));
                expect(!list(DIR + "/root").contains("procdir"), "the unlinked link is gone");
            }
        });
        check("modes are still there after a restart of the VM", new Check() {
            public void run() throws Exception {
                expectEq(0640, perms(DIR + "/tool"));
                expectEq(0711, perms(DIR + "/made"));
                expectEq(0751, perms(DIR + "/root/system"));
                expect(!list("/data").contains(".arc-wasm-meta"), "the table is not listed");
            }
        });
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        // "RUN PHASE": a phase left by another run ($ARC_TEST_RUN: the runner's)
        // is not ours (a browser's storage outlives it).
        final String runId = System.getenv("ARC_TEST_RUN") != null ? System.getenv("ARC_TEST_RUN") : "-";
        final String phaseText = read(PHASE);
        final String[] saved = phaseText == null ? new String[0] : phaseText.trim().split(" ");
        final boolean restartable = !runId.equals("-");
        int phase = saved.length == 2 && saved[0].equals(runId) ? Integer.parseInt(saved[1]) : 1;
        System.out.println("NativeExecTest: phase " + phase);
        boolean restart = false;
        if (phase == 1) {
            deleteTree(new File(DIR));
            new File(DIR).mkdirs();
            phase1();
            execChecks();
            if (restartable) {
                write(PHASE, runId + " 2");
                restart = true;
            } else {
                phase = 2;  // no restart: what is kept is checked in this run
            }
        }
        if (phase == 2) {
            phase2();
            new File(PHASE).delete();
        }
        System.out.println("NativeExecTest: " + passed + " passed, " + failed + " failed, " + skipped
                + " skipped in phase " + phase + " (" + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed > 0 ? failed : restart ? 75 : 0);
    }
}
