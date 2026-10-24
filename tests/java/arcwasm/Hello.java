package arcwasm;

/** The first Java program run by Dalvik in WebAssembly. */
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello from Dalvik on WebAssembly!");
        System.out.println("java.vm.name = " + System.getProperty("java.vm.name")
                + " " + System.getProperty("java.vm.version"));
        System.out.println("os.arch = " + System.getProperty("os.arch"));
        long sum = 0;
        for (int i = 1; i <= 1000; i++) {
            sum += (long) i * i;
        }
        System.out.println("sum of squares 1..1000 = " + sum);
        StringBuilder sb = new StringBuilder();
        for (String a : args) {
            sb.append('[').append(a).append(']');
        }
        System.out.println("args: " + sb);
    }
}
