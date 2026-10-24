package arcwasm;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.DayOfWeek;
import java.time.Duration;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.Period;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.time.temporal.ChronoUnit;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Calendar;
import java.util.Optional;
import java.util.OptionalInt;
import java.util.TimeZone;
import java.util.TreeMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.LongAdder;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.IntBinaryOperator;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;
import java.util.stream.Collectors;
import java.util.stream.IntStream;
import java.util.stream.LongStream;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import javax.xml.parsers.SAXParserFactory;
import org.xml.sax.Attributes;
import org.xml.sax.InputSource;
import org.xml.sax.helpers.DefaultHandler;

/**
 * Java 8 on ART 8.1 (topic art8, docs/topics/art8.md): what apps with
 * minSdkVersion 24-27 compiled by d8 use and the Android 6.0.1 stack cannot
 * run. Built with d8 --min-api 26, so the dex is version 038: default and
 * static interface methods stay in the interfaces (invoke-super and
 * invoke-static on interfaces), MethodHandle.invoke/invokeExact are
 * invoke-polymorphic. Lambdas are desugared by d8 (as for every Android
 * release), but run on libcore 8.1's java.util.function and
 * java.util.stream. java.time (API 26) uses ICU's time zone data.
 */
public class Java8Test {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Throwable;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
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

    // --- interfaces with default and static methods (dex 037+) -----------------

    interface Greeter {
        String name();

        default String greet() {
            return prefix() + ", " + name() + "!";
        }

        default String prefix() {
            return "Hello";
        }

        static Greeter of(String name) {
            return () -> name;
        }
    }

    interface Polite extends Greeter {
        @Override
        default String prefix() {
            return "Good day";
        }
    }

    interface Left {
        default String side() {
            return "left";
        }
    }

    interface Right {
        default String side() {
            return "right";
        }
    }

    static final class Both implements Left, Right {
        // A conflict the class must resolve: invoke-super on an interface.
        @Override
        public String side() {
            return Left.super.side() + "+" + Right.super.side();
        }
    }

    static final class Sir implements Polite {
        @Override
        public String name() {
            return "Sir";
        }
    }

    static int twice(IntBinaryOperator op, int a, int b) {
        return op.applyAsInt(op.applyAsInt(a, b), b);
    }

    static String upper(String s) {
        return s.toUpperCase();
    }

    static final class Box {
        final int value;

        Box(int value) {
            this.value = value;
        }

        int value() {
            return value;
        }

        static int add(int a, int b) {
            return a + b;
        }

        String describe(String prefix, int n) {
            return prefix + value + "x" + n;
        }
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("dex: arc-tests.jar is dex 038 (d8 --min-api 26; 039 for ART 10)", () -> {
            try (ZipFile zip = new ZipFile("/system/framework/arc-tests.jar")) {
                ZipEntry e = zip.getEntry("classes.dex");
                expect(e != null, "classes.dex in arc-tests.jar");
                byte[] magic = new byte[8];
                try (InputStream in = zip.getInputStream(e)) {
                    int n = 0;
                    while (n < 8) {
                        int r = in.read(magic, n, 8 - n);
                        if (r < 0) break;
                        n += r;
                    }
                }
                String version = new String(magic, 0, 7, StandardCharsets.US_ASCII);
                // (ART 10's arc-tests.jar is built with --min-api 29: dex 039.)
                expect(version.equals("dex\n038") || version.equals("dex\n039"), "dex version " + version);
            }
        });

        check("lambdas: capturing, non-capturing, method references", () -> {
            int k = 3;
            Function<Integer, Integer> addK = x -> x + k;
            Function<Integer, Integer> square = x -> x * x;
            expectEq(16, addK.andThen(square).apply(1));
            expectEq(4, addK.compose(square).apply(1));
            expectEq(25, twice((a, b) -> a * 2 + b, 4, 3));  // (4*2+3)*2+3
            UnaryOperator<String> up = Java8Test::upper;
            expectEq("ABC", up.apply("abc"));
            Supplier<List<String>> mk = ArrayList::new;
            List<String> l = mk.get();
            l.add("x");
            expectEq(1, l.size());
            Box box = new Box(7);
            Supplier<Integer> bound = box::value;
            expectEq(7, bound.get());
            BiFunction<Integer, Integer, Integer> add = Box::add;
            expectEq(9, add.apply(4, 5));
            Function<Integer, Box> ctor = Box::new;
            expectEq(11, ctor.apply(11).value);
            Predicate<String> empty = String::isEmpty;
            expect(empty.negate().test("a") && empty.test(""), "Predicate.negate");
            Runnable r = () -> {
                throw new IllegalStateException("from a lambda");
            };
            try {
                r.run();
                throw new AssertionError("no exception");
            } catch (IllegalStateException expected) {
                expectEq("from a lambda", expected.getMessage());
            }
        });

        check("interfaces: default, static, overriding, X.super.m()", () -> {
            expectEq("Hello, Ann!", Greeter.of("Ann").greet());
            expectEq("Good day, Sir!", new Sir().greet());
            expectEq("left+right", new Both().side());
            List<Integer> list = new ArrayList<>(Arrays.asList(5, 1, 4, 2, 3));
            list.removeIf(x -> x % 2 == 0);  // Collection.removeIf: a default method
            list.sort(Comparator.naturalOrder());  // List.sort: a default method
            expectEq(Arrays.asList(1, 3, 5), list);
            Map<String, Integer> m = new HashMap<>();
            m.merge("a", 1, Integer::sum);
            m.merge("a", 2, Integer::sum);
            m.computeIfAbsent("b", s -> s.length() * 10);
            expectEq(Integer.valueOf(3), m.get("a"));
            expectEq(Integer.valueOf(10), m.getOrDefault("b", -1));
            StringBuilder sb = new StringBuilder();
            new TreeMap<>(m).forEach((key, v) -> sb.append(key).append('=').append(v).append(';'));
            expectEq("a=3;b=10;", sb.toString());
        });

        check("streams: map/filter/reduce, collectors, primitive streams", () -> {
            expectEq(Integer.valueOf(30),
                     Stream.of(1, 2, 3, 4).filter(x -> x % 2 == 0).map(x -> x * 5).reduce(0, Integer::sum));
            expectEq(338350L, LongStream.rangeClosed(1, 100).map(x -> x * x).sum());
            expectEq("a-b-c", Stream.of("c", "a", "b").sorted().collect(Collectors.joining("-")));
            Map<Integer, List<String>> byLength = Stream.of("one", "two", "three", "four")
                    .collect(Collectors.groupingBy(String::length));
            expectEq(Arrays.asList("one", "two"), byLength.get(3));
            expectEq(Arrays.asList("four"), byLength.get(4));
            Map<Boolean, Long> parity = IntStream.range(0, 10).boxed()
                    .collect(Collectors.partitioningBy(x -> x % 2 == 0, Collectors.counting()));
            expectEq(5L, parity.get(true));
            OptionalInt max = IntStream.of(3, 9, 4).max();
            expect(max.isPresent() && max.getAsInt() == 9, "IntStream.max");
            expectEq(2.5, IntStream.of(1, 2, 3, 4).average().getAsDouble());
            List<String> flat = Stream.of(Arrays.asList("x", "y"), Arrays.asList("z"))
                    .flatMap(List::stream).collect(Collectors.toList());
            expectEq(Arrays.asList("x", "y", "z"), flat);
            expectEq(Long.valueOf(500000500000L),
                     LongStream.rangeClosed(1, 1000000).parallel().boxed().reduce(0L, Long::sum));
        });

        check("Optional", () -> {
            Optional<String> some = Optional.of("value");
            Optional<String> none = Optional.empty();
            expectEq("VALUE", some.map(String::toUpperCase).orElse("?"));
            expectEq("?", none.map(String::toUpperCase).orElse("?"));
            expectEq(5, some.filter(s -> s.startsWith("v")).map(String::length).get().intValue());
            expect(!some.filter(String::isEmpty).isPresent(), "Optional.filter");
            expectEq("lazy", none.orElseGet(() -> "lazy"));
            expectEq(Optional.of(6), some.flatMap(s -> Optional.of(s.length() + 1)));
            try {
                none.orElseThrow(() -> new IllegalArgumentException("none"));
                throw new AssertionError("no exception");
            } catch (IllegalArgumentException expected) {
                expectEq("none", expected.getMessage());
            }
        });

        check("java.time: dates, periods, durations, formatting (API 26)", () -> {
            LocalDate d = LocalDate.of(2026, 10, 17);
            expectEq(DayOfWeek.SATURDAY, d.getDayOfWeek());
            expectEq(LocalDate.of(2027, 2, 28), d.plusMonths(4).withDayOfMonth(28));
            expect(LocalDate.of(2024, 1, 1).isLeapYear() && !d.isLeapYear(), "isLeapYear");
            expectEq(Period.of(1, 2, 3), Period.between(LocalDate.of(2025, 8, 14), d));
            expectEq(365L, ChronoUnit.DAYS.between(d, d.plusYears(1)));
            expectEq("2026-10-17", d.format(DateTimeFormatter.ISO_LOCAL_DATE));
            expectEq(d, LocalDate.parse("2026-10-17"));
            expectEq("17.10.2026 13:05",
                     LocalDateTime.of(2026, 10, 17, 13, 5).format(DateTimeFormatter.ofPattern("dd.MM.yyyy HH:mm")));
            Duration dur = Duration.ofMinutes(90).plusSeconds(30);
            expectEq("PT1H30M30S", dur.toString());
            expectEq(5430L, dur.getSeconds());
            Instant i = Instant.parse("2026-10-17T10:15:30Z");
            expectEq(1792232130L, i.getEpochSecond());
            expectEq(LocalDateTime.of(2026, 10, 17, 10, 15, 30), LocalDateTime.ofInstant(i, ZoneOffset.UTC));
        });

        check("java.time: time zones from ICU data (ZoneId, DST)", () -> {
            ZoneId paris = ZoneId.of("Europe/Paris");
            ZonedDateTime summer = ZonedDateTime.of(2026, 7, 1, 12, 0, 0, 0, paris);
            ZonedDateTime winter = ZonedDateTime.of(2026, 1, 1, 12, 0, 0, 0, paris);
            expectEq(ZoneOffset.ofHours(2), summer.getOffset());
            expectEq(ZoneOffset.ofHours(1), winter.getOffset());
            ZonedDateTime tokyo = summer.withZoneSameInstant(ZoneId.of("Asia/Tokyo"));
            expectEq(19, tokyo.getHour());
            expect(ZoneId.getAvailableZoneIds().contains("America/New_York"), "available zone ids");
            // java.util.TimeZone: libcore's ZoneInfoDB (tzdata in /system/usr/share/zoneinfo).
            TimeZone tz = TimeZone.getTimeZone("Europe/Paris");
            expectEq("Europe/Paris", tz.getID());
            expectEq(3600000, tz.getRawOffset());
            Calendar cal = Calendar.getInstance(tz);
            cal.clear();
            cal.set(2026, Calendar.JULY, 1, 12, 0);
            expectEq(7200000, tz.getOffset(cal.getTimeInMillis()));
        });

        check("MethodHandle: lookup, invokeExact, invoke (invoke-polymorphic)", () -> {
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            MethodHandle length = lookup.findVirtual(String.class, "length", MethodType.methodType(int.class));
            expectEq(5, (int) length.invokeExact("hello"));
            MethodHandle add = lookup.findStatic(Box.class, "add",
                                                 MethodType.methodType(int.class, int.class, int.class));
            expectEq(12, (int) add.invokeExact(5, 7));
            // invoke with conversions (boxing, widening) through asType.
            Object boxed = add.invoke(Integer.valueOf(2), 3);
            expectEq(5, boxed);
            MethodHandle ctor = lookup.findConstructor(Box.class, MethodType.methodType(void.class, int.class));
            Box b = (Box) ctor.invoke(42);
            expectEq(42, b.value);
            MethodHandle describe = lookup.findVirtual(Box.class, "describe",
                    MethodType.methodType(String.class, String.class, int.class));
            MethodHandle bound = describe.bindTo(b);
            expectEq("v42x3", (String) bound.invokeExact("v", 3));
            MethodHandle getter = lookup.findGetter(Box.class, "value", int.class);
            expectEq(42, (int) getter.invokeExact(b));
            MethodHandle concat = lookup.findVirtual(String.class, "concat",
                    MethodType.methodType(String.class, String.class));
            MethodHandle withSuffix = MethodHandles.insertArguments(concat, 1, "!");
            expectEq("wow!", (String) withSuffix.invokeExact("wow"));
            MethodHandle dropped = MethodHandles.dropArguments(length, 1, int.class);
            expectEq(3, (int) dropped.invokeExact("abc", 99));
        });

        check("java.util.concurrent: CompletableFuture, LongAdder", () -> {
            CompletableFuture<Integer> f = CompletableFuture.supplyAsync(() -> 6)
                    .thenApply(x -> x * 7)
                    .thenCombine(CompletableFuture.completedFuture(0), Integer::sum);
            expectEq(Integer.valueOf(42), f.get(10, TimeUnit.SECONDS));
            LongAdder adder = new LongAdder();
            IntStream.range(0, 1000).parallel().forEach(i -> adder.increment());
            expectEq(1000L, adder.sum());
        });

        // libcore's ThreadPoolExecutor on phones (topic ios-threads): with
        // arcwasm.pool.core_idle_ms idle core threads end, tasks still run.
        check("ThreadPoolExecutor: idle core threads end with arcwasm.pool.core_idle_ms", () -> {
            String before = System.getProperty("arcwasm.pool.core_idle_ms");
            ThreadPoolExecutor plain = new ThreadPoolExecutor(3, 3, 0L, TimeUnit.MILLISECONDS,
                    new LinkedBlockingQueue<Runnable>());
            ThreadPoolExecutor ex = new ThreadPoolExecutor(3, 3, 0L, TimeUnit.MILLISECONDS,
                    new LinkedBlockingQueue<Runnable>());
            ScheduledThreadPoolExecutor sched = new ScheduledThreadPoolExecutor(2);
            try {
                System.clearProperty("arcwasm.pool.core_idle_ms");
                plain.prestartAllCoreThreads();
                Thread.sleep(300);
                expectEq(3, plain.getPoolSize());  // as on Android: core threads stay
                System.setProperty("arcwasm.pool.core_idle_ms", "200");
                CountDownLatch ran = new CountDownLatch(6);
                for (int i = 0; i < 6; i++) ex.execute(ran::countDown);
                expect(ran.await(10, TimeUnit.SECONDS), "tasks ran");
                long t0 = System.nanoTime();
                while (ex.getPoolSize() > 0 && System.nanoTime() - t0 < 10000000000L) Thread.sleep(50);
                expectEq(0, ex.getPoolSize());
                Thread.sleep(500);
                expectEq(0, ex.getPoolSize());  // and none started again
                expect(ex.getCompletedTaskCount() == 6, "6 tasks: " + ex.getCompletedTaskCount());
                Future<Integer> again = ex.submit(() -> 7);  // new threads for new tasks
                expectEq(Integer.valueOf(7), again.get(10, TimeUnit.SECONDS));
                Future<Integer> later = sched.schedule(() -> 9, 800, TimeUnit.MILLISECONDS);
                expectEq(Integer.valueOf(9), later.get(10, TimeUnit.SECONDS));  // a thread waits for it
            } finally {
                if (before == null) System.clearProperty("arcwasm.pool.core_idle_ms");
                else System.setProperty("arcwasm.pool.core_idle_ms", before);
                plain.shutdown();
                ex.shutdown();
                sched.shutdown();
            }
        });

        // topic threads-trim: below the core size a task goes to a thread that
        // is idle (not one more thread), and tasks that wait for each other
        // still all get threads (a reserved idle thread, else a new one).
        check("ThreadPoolExecutor: idle threads take new tasks with arcwasm.pool.core_idle_ms", () -> {
            String before = System.getProperty("arcwasm.pool.core_idle_ms");
            List<Thread> made = Collections.synchronizedList(new ArrayList<>());
            ThreadPoolExecutor ex = new ThreadPoolExecutor(4, 4, 0L, TimeUnit.MILLISECONDS,
                    new LinkedBlockingQueue<Runnable>(), r -> { Thread t = new Thread(r); made.add(t); return t; });
            try {
                System.setProperty("arcwasm.pool.core_idle_ms", "30000");
                for (int i = 0; i < 6; i++) {  // one short task after the other
                    expectEq(Integer.valueOf(i), ex.submit(() -> 0).get(10, TimeUnit.SECONDS) + i);
                    // Until its thread waits for work again: a finished task's
                    // thread is not idle yet (under load the next task came
                    // first and a second thread was made).
                    long t0 = System.nanoTime();
                    while (System.nanoTime() - t0 < 2000000000L) {
                        boolean busy = false;
                        synchronized (made) {
                            for (Thread t : made) {
                                Thread.State st = t.getState();
                                if (st != Thread.State.WAITING && st != Thread.State.TIMED_WAITING) busy = true;
                            }
                        }
                        if (!busy) break;
                        Thread.sleep(5);
                    }
                }
                expect(ex.getPoolSize() == 1, "one thread for 6 tasks in turn: " + ex.getPoolSize());
                // 4 tasks that each wait for all 4 to start: needs 4 threads at once.
                CountDownLatch all = new CountDownLatch(4);
                List<Future<Boolean>> fs = new ArrayList<>();
                for (int i = 0; i < 4; i++) fs.add(ex.submit(() -> { all.countDown(); return all.await(10, TimeUnit.SECONDS); }));
                for (Future<Boolean> f : fs) expect(f.get(15, TimeUnit.SECONDS), "tasks waiting for each other all ran");
                expect(ex.getPoolSize() == 4, "4 threads when 4 tasks need them: " + ex.getPoolSize());
            } finally {
                if (before == null) System.clearProperty("arcwasm.pool.core_idle_ms");
                else System.setProperty("arcwasm.pool.core_idle_ms", before);
                ex.shutdown();
            }
        });

        // topic heavy-apps-mem: no idle pool thread waits longer than
        // arcwasm.pool.core_idle_ms (a cached pool kept its threads 60 s).
        check("ThreadPoolExecutor: cached pools' idle threads end with arcwasm.pool.core_idle_ms", () -> {
            String before = System.getProperty("arcwasm.pool.core_idle_ms");
            ExecutorService cached = Executors.newCachedThreadPool();
            try {
                System.setProperty("arcwasm.pool.core_idle_ms", "200");
                CountDownLatch all = new CountDownLatch(3);
                List<Future<Boolean>> fs = new ArrayList<>();
                for (int i = 0; i < 3; i++) fs.add(cached.submit(() -> { all.countDown(); return all.await(10, TimeUnit.SECONDS); }));
                for (Future<Boolean> f : fs) expect(f.get(15, TimeUnit.SECONDS), "3 tasks at once ran");
                ThreadPoolExecutor ex = (ThreadPoolExecutor) cached;
                expectEq(3, ex.getPoolSize());
                long t0 = System.nanoTime();
                while (ex.getPoolSize() > 0 && System.nanoTime() - t0 < 10000000000L) Thread.sleep(50);
                expectEq(0, ex.getPoolSize());
                expectEq(Integer.valueOf(5), cached.submit(() -> 5).get(10, TimeUnit.SECONDS));
            } finally {
                if (before == null) System.clearProperty("arcwasm.pool.core_idle_ms");
                else System.setProperty("arcwasm.pool.core_idle_ms", before);
                cached.shutdown();
            }
        });

        check("API 24-26 classes: Base64, String.join, java.nio.file", () -> {
            String enc = Base64.getEncoder().encodeToString("arc-wasm".getBytes(StandardCharsets.UTF_8));
            expectEq("YXJjLXdhc20=", enc);
            expectEq("arc-wasm", new String(Base64.getDecoder().decode(enc), StandardCharsets.UTF_8));
            expectEq("a,b,c", String.join(",", Arrays.asList("a", "b", "c")));
            Path p = Paths.get("/system/build.prop");
            expect(Files.exists(p), "Files.exists /system/build.prop");
            List<String> lines = Files.readAllLines(p, StandardCharsets.UTF_8);
            // (SDK 27 on ART 8.1, 29 on ART 10: topic art10 runs this test too.)
            expect(lines.contains("ro.build.version.sdk=27") || lines.contains("ro.build.version.sdk=29"),
                   "build.prop says SDK 27 or 29: " + lines);
            try (Stream<Path> dir = Files.list(Paths.get("/system/framework"))) {
                List<String> names = dir.map(x -> x.getFileName().toString()).sorted()
                        .collect(Collectors.toList());
                expect(names.contains("core-oj.jar") && names.contains("core-libart.jar"),
                       "Files.list /system/framework: " + names);
            }
        });

        check("libcore natives on BoringSSL and expat: BigInteger, BigDecimal, SAX", () -> {
            BigInteger f = BigInteger.ONE;
            for (int i = 2; i <= 30; i++) {
                f = f.multiply(BigInteger.valueOf(i));
            }
            expectEq("265252859812191058636308480000000", f.toString());
            expectEq(new BigInteger("1000000007"), BigInteger.valueOf(1000000007).nextProbablePrime()
                    .subtract(BigInteger.valueOf(2)).max(BigInteger.valueOf(1000000007)));
            expectEq(BigInteger.valueOf(2), BigInteger.valueOf(3).modPow(BigInteger.valueOf(200),
                                                                          BigInteger.valueOf(7)));
            expectEq("0.3333333333", BigDecimal.ONE.divide(BigDecimal.valueOf(3), 10, RoundingMode.HALF_UP)
                    .toPlainString());
            StringBuilder seen = new StringBuilder();
            SAXParserFactory.newInstance().newSAXParser().parse(
                    new InputSource(new java.io.StringReader("<a x='1'><b>text</b><c/></a>")),
                    new DefaultHandler() {
                        @Override
                        public void startElement(String uri, String local, String qName, Attributes attrs) {
                            seen.append('<').append(qName);
                            if (attrs.getLength() > 0) seen.append(' ').append(attrs.getValue("x"));
                            seen.append('>');
                        }

                        @Override
                        public void characters(char[] ch, int start, int length) {
                            seen.append(ch, start, length);
                        }
                    });
            expectEq("<a 1><b>text<c>", seen.toString());
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("Java8Test: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
