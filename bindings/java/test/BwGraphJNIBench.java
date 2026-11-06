import db.monacgraph.jni.BwGraphJNI;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** JNI + registry + ID mapping/property-path throughput, NOT topology insertion QPS. */
public class BwGraphJNIBench {
    static final int VERTICES = 1024;
    static final byte[][] ids = new byte[VERTICES][];
    static final byte[] value = new byte[64];
    static double run(BwGraphJNI api, long g, int threads, int count, int mode) throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        CountDownLatch ready = new CountDownLatch(threads), start = new CountDownLatch(1);
        List<Future<Long>> results = new ArrayList<>();
        try {
            for (int t = 0; t < threads; t++) {
                final int worker = t;
                results.add(pool.submit(() -> {
                    ready.countDown(); start.await(); long checksum = 0;
                    for (int i = 0; i < count; i++) {
                        int v = mode == 2 ? 0 : (i * 17 + worker * 127) % VERTICES;
                        if (mode != 0 && i % 5 == 0) api.putVertexData(g, v, value);
                        else {
                            long actual = api.getVertexHandleById(g, ids[v]);
                            if (actual != v) throw new AssertionError("ID mismatch");
                            checksum += actual;
                        }
                    }
                    return checksum;
                }));
            }
            ready.await(); long begin = System.nanoTime(); start.countDown();
            long checksum = 0; for (var result : results) checksum += result.get(120, TimeUnit.SECONDS);
            double seconds = (System.nanoTime() - begin) / 1e9;
            if (checksum < 0) throw new AssertionError();
            return (double)threads * count / seconds;
        } finally { start.countDown(); pool.shutdownNow(); }
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("bw-jni-bench-");
        BwGraphJNI api = new BwGraphJNI(root.toString());
        long g = api.openDB("bench");
        try {
            for (int i = 0; i < VERTICES; i++) {
                ids[i] = ("uuid-benchmark-" + i).getBytes(StandardCharsets.UTF_8);
                if (api.createVertex(g, ids[i], value) != i) throw new AssertionError();
            }
            System.out.println("workload,threads,sample,operations,qps");
            String[] names = {"id_lookup", "80_lookup_20_property_put", "hot_80_lookup_20_property_put"};
            for (int mode = 0; mode < 3; mode++) for (int threads : new int[]{1,4,8,16}) {
                run(api, g, threads, 20000, mode);
                for (int sample = 1; sample <= 3; sample++) {
                    int count = mode == 0 ? 1_000_000 : Math.max(20000, 500000 / threads);
                    double qps = run(api, g, threads, count, mode);
                    System.out.printf(Locale.ROOT, "%s,%d,%d,%d,%.0f%n", names[mode], threads, sample, threads*count, qps);
                }
            }
        } finally {
            api.closeDB(g);
            try (var paths = Files.walk(root)) {
                for (Path p : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(p);
            }
        }
    }
}
