import db.monacgraph.jni.BwGraphJNI;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

public class BwGraphJNITest {
    static byte[] b(String value) { return value.getBytes(StandardCharsets.UTF_8); }
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static void expect(Class<? extends Throwable> type, Runnable op) {
        try { op.run(); } catch (Throwable e) {
            if (type.isInstance(e)) return;
            throw new AssertionError("Expected " + type + ", got " + e, e);
        }
        throw new AssertionError("Expected " + type);
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("bw-jni-图-😀-");
        try {
            BwGraphJNI api = new BwGraphJNI(root.toString());
            expect(IllegalArgumentException.class, () -> api.openDB("../bad"));
            Files.createDirectory(root.resolve("foreign"));
            expect(IllegalStateException.class, () -> api.openDB("foreign"));
            long g = api.openDB("main");
            check(api.getAllVertices(g).length == 0);
            check(api.getWCC(g)[0] == 0 && api.getSCC(g)[0] == 0 && api.getCommunities(g)[0] == 0);
            byte[] uuid = new byte[]{0, -1, 1, 8, 9, 0, 55, -128, 0, 1, 2, 3, 4, 5, 6, 7};
            byte[] properties = new byte[]{0, 2, -1, 5};
            long a = api.createVertex(g, uuid, properties);
            long b = api.createVertex(g, b("b"), b("B"));
            long c = api.createVertex(g, b("c"), b("C"));
            check(Arrays.equals(api.getVertexExternalId(g, a), uuid));
            check(Arrays.equals(api.getDataFromVertexHandle(g, a), properties));
            check(api.getVertexHandleById(g, uuid) == a);
            check(api.getVertexHandleById(g, b("missing")) == -1);
            expect(IllegalArgumentException.class, () -> api.createVertex(g, uuid, properties));
            expect(IllegalArgumentException.class, () -> api.createVertex(g, null, properties));
            expect(NoSuchElementException.class, () -> api.createEdge(g, b("bad"), a, 999, properties));
            long ab = api.createEdge(g, b("ab"), a, b, properties);
            long ba = api.createEdge(g, b("ba"), b, a, b("reverse"));
            long bc = api.createEdge(g, b("bc"), b, c, b("bc"));
            long aa = api.createEdge(g, b("aa"), a, a, b("loop"));
            check(Arrays.equals(api.getEdgeExternalId(g, ab), b("ab")));
            check(Arrays.equals(api.getEdgeHandleByVertex(g, a, 0), new long[]{aa, ab}));
            check(Arrays.equals(api.getEdgeHandleByVertex(g, a, 1), new long[]{aa, ba}));
            check(api.getEdgeHandleByVertex(g, a, 2).length == 3);
            expect(IllegalArgumentException.class, () -> api.getEdgeHandleByVertex(g, a, 3));
            expect(IllegalArgumentException.class, () -> api.createEdge(g, b("parallel"), a, b, properties));
            check(Arrays.equals(api.getBfsVertices(g, a), new long[]{a,0,b,1,c,2}));
            check(api.getWCC(g)[0] == 1 && api.getSCC(g)[0] == 2);
            check(api.getCommunities(g).length == 7);
            api.putEdgeData(g, ab, b("new")); api.putVertexData(g, a, b("newV"));
            long lease = api.openDB("main");
            api.closeDB(g);
            expect(IllegalArgumentException.class, () -> api.getAllVertices(g));
            check(Arrays.equals(api.getDataFromEdgeHandle(lease, ab), b("new")));
            api.closeDB(lease);
            long reopened = api.openDB("main");
            check(Arrays.equals(api.getDataFromVertexHandle(reopened, a), b("newV")));
            api.removeVertex(reopened, b);
            check(api.getEdgeHandleById(reopened, b("ab")) == -1);
            check(api.getEdgeHandleById(reopened, b("ba")) == -1);
            check(api.getEdgeHandleById(reopened, b("bc")) == -1);
            check(Arrays.equals(api.getAllEdges(reopened), new long[]{aa}));
            check(api.getEdgeHandleByVertex(reopened, c, 1).length == 0);
            api.removeEdge(reopened, aa);
            expect(NoSuchElementException.class, () -> api.getDataFromEdgeHandle(reopened, aa));
            api.closeDB(reopened);
            long finalOpen = api.openDB("main");
            check(api.getAllVertices(finalOpen).length == 2 && api.getAllEdges(finalOpen).length == 0);
            api.closeDB(finalOpen);

            long parallel = api.openDB("parallel"), other = api.openDB("other");
            long otherV = api.createVertex(other, uuid, b("independent"));
            int threads = 8;
            long[] vertices = new long[threads * 2];
            for (int i = 0; i < vertices.length; i++) vertices[i] = api.createVertex(parallel, b("v"+i), properties);
            ExecutorService pool = Executors.newFixedThreadPool(threads);
            AtomicInteger winners = new AtomicInteger();
            CyclicBarrier barrier = new CyclicBarrier(threads);
            List<Future<?>> tasks = new ArrayList<>();
            try {
                for (int i = 0; i < threads; i++) {
                    final int t = i;
                    tasks.add(pool.submit(() -> {
                        barrier.await();
                        long src = vertices[2*t], dst = vertices[2*t+1];
                        long edge = api.createEdge(parallel, b("e"+t), src, dst, properties);
                        for (int j = 0; j < 100; j++) {
                            byte[] value = b("value"+j);
                            api.putVertexData(parallel, src, value);
                            api.putEdgeData(parallel, edge, value);
                            check(Arrays.equals(api.getDataFromVertexHandle(parallel, src), value));
                            check(Arrays.equals(api.getDataFromEdgeHandle(parallel, edge), value));
                            check(api.getVertexHandleById(parallel, b("v"+(2*t))) == src);
                            check(Arrays.equals(api.getEdgeHandleByVertex(parallel, dst, 1), new long[]{edge}));
                            api.putVertexData(parallel, vertices[1], value);
                            api.getDataFromVertexHandle(parallel, vertices[1]);
                        }
                        try { api.createEdge(parallel, b("hotID"), dst, src, properties); winners.incrementAndGet(); }
                        catch (IllegalArgumentException expected) { }
                        return null;
                    }));
                }
                for (Future<?> task : tasks) task.get(60, TimeUnit.SECONDS);
            } finally { pool.shutdownNow(); }
            check(winners.get() == 1);
            check(api.getAllEdges(parallel).length == threads + 1);
            api.closeDB(parallel);
            check(Arrays.equals(api.getDataFromVertexHandle(other, otherV), b("independent")));
            api.closeDB(other);
            long again = api.openDB("parallel");
            check(api.getAllEdges(again).length == threads + 1);
            api.closeDB(again);
            System.out.println("JNI PASS: CRUD, binary IDs, OUT/IN/BOTH, reopen, BFS/WCC/SCC, 8-thread races");
        } finally {
            try (var paths = Files.walk(root)) {
                for (Path p : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(p);
            }
        }
    }
}
