// javac whose class files go straight into a jar, not into a directory.
// macOS file systems are case-insensitive by default, and AOSP's Java has
// classes whose names differ only in case within a package (Android 10:
// android.media.AudioAttributes and the javastream class
// android.media.Audioattributes of audioattributes.proto): in a -d
// directory one class file overwrites the other. Used by
// tools/build_framework10_jars.py.
//
//   java JavacToJar.java OUT.jar SOURCES_FILE [javac options...]
//
// SOURCES_FILE has one source path per line (quotes around it are dropped).
// Prints javac's diagnostics; exit status 1 if compilation failed.
import java.io.*;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import javax.tools.*;

public class JavacToJar {
    public static void main(String[] args) throws Exception {
        Path out = Paths.get(args[0]);
        List<File> sources = new ArrayList<>();
        for (String line : Files.readAllLines(Paths.get(args[1]), StandardCharsets.UTF_8)) {
            line = line.trim();
            if (line.startsWith("\"") && line.endsWith("\"")) line = line.substring(1, line.length() - 1);
            if (!line.isEmpty()) sources.add(new File(line));
        }
        List<String> options = new ArrayList<>(Arrays.asList(args).subList(2, args.length));
        JavaCompiler javac = ToolProvider.getSystemJavaCompiler();
        StandardJavaFileManager std = javac.getStandardFileManager(null, null, StandardCharsets.UTF_8);
        final TreeMap<String, ByteArrayOutputStream> classes = new TreeMap<>();
        JavaFileManager fm = new ForwardingJavaFileManager<StandardJavaFileManager>(std) {
            @Override
            public JavaFileObject getJavaFileForOutput(Location location, String className,
                                                       JavaFileObject.Kind kind, FileObject sibling) {
                final String path = className.replace('.', '/') + kind.extension;
                return new SimpleJavaFileObject(URI.create("mem:///" + path), kind) {
                    @Override
                    public OutputStream openOutputStream() {
                        ByteArrayOutputStream b = new ByteArrayOutputStream();
                        classes.put(path, b);
                        return b;
                    }
                };
            }
        };
        StringWriter diag = new StringWriter();
        boolean ok = javac.getTask(diag, fm, null, options, null,
                                   std.getJavaFileObjectsFromFiles(sources)).call();
        for (String l : diag.toString().split("\n")) {
            if (!l.startsWith("Note:") && !l.isEmpty()) System.err.println(l);
        }
        if (!ok) System.exit(1);
        try (JarOutputStream jar = new JarOutputStream(Files.newOutputStream(out))) {
            Set<String> dirs = new HashSet<>();
            for (Map.Entry<String, ByteArrayOutputStream> e : classes.entrySet()) {
                String name = e.getKey();
                for (int i = name.indexOf('/'); i >= 0; i = name.indexOf('/', i + 1)) {
                    String d = name.substring(0, i + 1);
                    if (dirs.add(d)) jar.putNextEntry(new JarEntry(d));
                }
                jar.putNextEntry(new JarEntry(name));
                e.getValue().writeTo(jar);
            }
        }
        System.out.println("  " + classes.size() + " classes");
    }
}
