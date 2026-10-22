package local.celllock;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** Coordinates the one collector whose identity has been verified for this device profile. */
final class DiagnosticsCoordinator {
    static final String PACKAGE = "make.more.r2d2.cellular_pro";
    static final String PACKAGE_COMMAND = "dumpsys package " + PACKAGE;
    static final String STOP_COMMAND = "am force-stop --user 0 " + PACKAGE;
    static final String BLOCKED_FLAG = "CELLLOCK_DIAGNOSTICS_BLOCKED=1";
    private static final long STOP_WAIT_MS = 4000;
    private final RootBackend.CommandRunner runner;
    private final Log log;
    private final Waiter waiter;

    interface Log { void append(String value); }
    interface Waiter {
        long now();
        void sleep(long millis) throws InterruptedException;
    }

    DiagnosticsCoordinator(RootBackend.CommandRunner runner, Log log) {
        this(runner, log, new Waiter() {
            public long now() { return System.nanoTime() / 1000000L; }
            public void sleep(long millis) throws InterruptedException { Thread.sleep(millis); }
        });
    }

    DiagnosticsCoordinator(RootBackend.CommandRunner runner, Log log, Waiter waiter) {
        this.runner = runner; this.log = log; this.waiter = waiter;
    }

    String ensureIdle() throws Exception {
        List<Integer> pids = runningCollectors();
        if (pids.isEmpty()) return "";
        RootBackend.Result packageResult = runner.run(PACKAGE_COMMAND, 5, false);
        if (packageResult.code != 0) throw blocked("无法读取 Cellular-Pro 的安装信息，不能确认 dcidaemon 的归属");
        PackageIdentity app = PackageIdentity.parse(packageResult.text);
        RootBackend.Result paths = runner.run(app.canonicalPathsCommand(), 3, false);
        app = app.resolve(paths);
        List<ProcessIdentity> identities = identify(pids, app);
        // Recheck both PID membership and start time: a disappearing/reused PID is not authorization.
        if (!pids.equals(runningCollectors()) || !identities.equals(identify(pids, app)))
            throw blocked("dcidaemon 在归属核验期间发生变化，请重新操作");
        log.append("采集协调：已确认 " + PACKAGE + " 的 root dcidaemon，PID " + pids
            + "；将强行停止 Cellular-Pro，采集会中断。");
        final RootBackend.Result stopped;
        try { stopped = runner.run(STOP_COMMAND, 5, false); }
        catch (Exception e) {
            throw blocked("停止 Cellular-Pro 的命令未完成；采集是否停止尚未确认：" + e.getMessage());
        }
        log.append("采集协调：force-stop " + PACKAGE + "（退出码 " + stopped.code + "）\n" + stopped.text);
        if (stopped.code != 0 || !stopped.text.trim().isEmpty())
            throw blocked("停止 Cellular-Pro 未获得正常结果，未确认诊断通道已释放");
        long deadline = waiter.now() + STOP_WAIT_MS;
        int clearReads = 0;
        while (waiter.now() < deadline) {
            List<Integer> remaining = runningCollectors();
            if (remaining.isEmpty()) {
                if (++clearReads == 2) {
                    String note = "已自动停止 Cellular-Pro，并连续两次确认 dcidaemon 已退出。";
                    log.append("采集协调：" + note);
                    return note;
                }
            } else {
                if (clearReads != 0) throw blocked("停止后 dcidaemon 再次出现，诊断采集可能已重启");
                final List<ProcessIdentity> current;
                try { current = identify(remaining, app); }
                catch (IllegalStateException identityError) {
                    // After our verified force-stop, the process may exit between
                    // pidof and /proc. Require two fresh empty observations; an
                    // active or unqueryable collector still blocks the operation.
                    if (!runningCollectors().isEmpty()) throw identityError;
                    clearReads = 1;
                    log.append("采集协调：停止后进程在归属读取期间退出，已重新确认无 dcidaemon，等待第二次核对。");
                    waiter.sleep(200);
                    continue;
                }
                for (ProcessIdentity identity : current)
                    if (!identities.contains(identity))
                        throw blocked("停止后出现新的 dcidaemon 进程，诊断采集可能已重启");
            }
            waiter.sleep(200);
        }
        throw blocked("停止 Cellular-Pro 后 dcidaemon 未在限时内退出");
    }

    private List<ProcessIdentity> identify(List<Integer> pids, PackageIdentity app) throws Exception {
        List<ProcessIdentity> result = new ArrayList<>();
        for (int pid : pids) {
            RootBackend.Result query = runner.run(processCommand(pid), 3, false);
            if (query.code != 0) throw blocked("无法读取 dcidaemon 的归属信息（PID " + pid + "）");
            ProcessIdentity identity = ProcessIdentity.parse(pid, query.text, app);
            log.append("采集归属：PID=" + pid + " starttime=" + identity.startTime
                + " exe=" + identity.executable + " package=" + PACKAGE);
            result.add(identity);
        }
        return result;
    }

    private List<Integer> runningCollectors() throws Exception {
        final RootBackend.Result query;
        try { query = runner.run("pidof dcidaemon", 3, false); }
        catch (Exception e) { throw blocked("无法核对原诊断采集状态：" + e.getMessage()); }
        String text = query.text.trim();
        log.append("诊断采集进程检查（退出码 " + query.code + "）：" + text);
        if (query.code == 1 && text.isEmpty()) return new ArrayList<>();
        if (query.code != 0 || !text.matches("[0-9]+(?:\\s+[0-9]+)*"))
            throw blocked("无法核对原诊断采集状态（退出码 " + query.code + "）");
        Set<Integer> pids = new LinkedHashSet<>();
        try {
            for (String value : text.split("\\s+")) {
                int pid = Integer.parseInt(value);
                if (pid <= 0 || !pids.add(pid)) throw new NumberFormatException();
            }
        } catch (NumberFormatException e) { throw blocked("无法核对原诊断采集状态：进程编号无效"); }
        List<Integer> result = new ArrayList<>(pids);
        java.util.Collections.sort(result);
        return result;
    }

    static String processCommand(int pid) {
        if (pid <= 0) throw new IllegalArgumentException("Invalid PID");
        String proc = "/proc/" + pid;
        return "printf 'STAT='\ncat " + proc + "/stat || exit 1\n"
            + "printf 'EXE='\nreadlink -f " + proc + "/exe || exit 1\n"
            + "printf 'UID='\nawk '/^Uid:/ {print $2, $3, $4, $5; found=1} END {if (!found) exit 1}' " + proc + "/status || exit 1\n"
            + "printf 'ARGS\\n'\ntr '\\000' '\\n' < " + proc + "/cmdline || exit 1\n"
            + "printf 'END_ARGS\\nSTAT_AFTER='\ncat " + proc + "/stat || exit 1";
    }

    /** The last check and helper invocation share one shell; this is not a cross-app lock. */
    static String guardNativeCommand(String command) {
        return "celllock_collectors=$(pidof dcidaemon 2>&1)\ncelllock_pidof_code=$?\n"
            + "if [ \"$celllock_pidof_code\" -ne 1 ] || [ -n \"$celllock_collectors\" ]; then\n"
            + "  printf '" + BLOCKED_FLAG + "\\n'\n  printf '%s\\n' \"$celllock_collectors\"\n  exit 73\nfi\n" + command;
    }

    private static IllegalStateException blocked(String detail) {
        return new IllegalStateException(detail + "；未启动本应用的诊断操作。");
    }

    static final class PackageIdentity {
        final String dataDir, nativeDir;
        PackageIdentity(String dataDir, String nativeDir) { this.dataDir = dataDir; this.nativeDir = nativeDir; }
        static PackageIdentity parse(String output) {
            if (!output.contains("Package [" + PACKAGE + "]")) throw blocked("未取得目标 Cellular-Pro 包信息");
            String data = uniqueValue(output, "dataDir=");
            String nativeDir = uniqueValue(output, "nativeLibraryDir=");
            if (nativeDir == null) {
                nativeDir = uniqueValue(output, "legacyNativeLibraryDir=");
                if (nativeDir != null && nativeDir.endsWith("/lib")
                    && "arm64-v8a".equals(uniqueValue(output, "primaryCpuAbi="))) nativeDir += "/arm64";
            }
            if (data == null || !(data.equals("/data/user/0/" + PACKAGE) || data.equals("/data/data/" + PACKAGE))
                || nativeDir == null || !nativeDir.startsWith("/data/app/")
                || !nativeDir.contains("/" + PACKAGE + "-") || !nativeDir.endsWith("/lib/arm64")
                || nativeDir.contains("/../") || nativeDir.contains("/./"))
                throw blocked("Cellular-Pro 的安装路径与已核对的布局不符");
            return new PackageIdentity(data, nativeDir);
        }
        String canonicalPathsCommand() {
            return "printf 'DATA='\nreadlink -f " + shellQuote(dataDir) + " || exit 1\n"
                + "printf 'LIB='\nreadlink -f " + shellQuote(nativeDir) + " || exit 1";
        }
        PackageIdentity resolve(RootBackend.Result result) {
            String[] lines = result.text.split("\n");
            if (result.code != 0 || lines.length != 2 || !lines[0].equals("DATA=/data/user/0/" + PACKAGE)
                || !lines[1].equals("LIB=" + nativeDir))
                throw blocked("无法规范化核对 Cellular-Pro 的实际安装路径");
            return new PackageIdentity(lines[0].substring(5), lines[1].substring(4));
        }
        private static String uniqueValue(String text, String prefix) {
            String result = null;
            for (String line : text.split("\n")) {
                String value = line.trim();
                if (!value.startsWith(prefix)) continue;
                value = value.substring(prefix.length());
                if (value.isEmpty() || value.matches(".*\\s.*") || result != null && !result.equals(value))
                    throw blocked("Cellular-Pro 包路径信息不明确");
                result = value;
            }
            return result;
        }
    }

    static final class ProcessIdentity {
        final int pid;
        final String startTime, executable;
        ProcessIdentity(int pid, String startTime, String executable) { this.pid = pid; this.startTime = startTime; this.executable = executable; }
        static ProcessIdentity parse(int pid, String output, PackageIdentity app) {
            String[] lines = output.split("\n", -1);
            int end = Arrays.asList(lines).indexOf("END_ARGS");
            if (lines.length < 8 || !lines[0].startsWith("STAT=") || !lines[1].startsWith("EXE=")
                || !lines[2].equals("UID=0 0 0 0") || !lines[3].equals("ARGS") || end <= 4
                || end + 1 >= lines.length || !lines[end+1].startsWith("STAT_AFTER="))
                throw blocked("无法证明 dcidaemon 属于 Cellular-Pro（PID " + pid + "）");
            String start = startTime(pid, lines[0].substring(5));
            if (!start.equals(startTime(pid, lines[end+1].substring(11))))
                throw blocked("dcidaemon 在读取归属时发生变化（PID " + pid + "）");
            String executable = lines[1].substring(4);
            if (!executable.equals(app.dataDir + "/files/dcidaemon"))
                throw blocked("同名 dcidaemon 的可执行文件不属于已核对的 Cellular-Pro 路径（PID " + pid + "）");
            List<String> args = Arrays.asList(Arrays.copyOfRange(lines, 4, end));
            if (!(args.get(0).equals("dcidaemon") || args.get(0).equals(executable))
                || !PACKAGE.equals(option(args, "-i")) || !app.nativeDir.equals(option(args, "-l")))
                throw blocked("dcidaemon 的包名或库路径与 Cellular-Pro 不一致（PID " + pid + "）");
            return new ProcessIdentity(pid, start, executable);
        }
        private static String option(List<String> args, String key) {
            int index = args.indexOf(key);
            return index > 0 && index == args.lastIndexOf(key) && index + 1 < args.size() ? args.get(index + 1) : null;
        }
        private static String startTime(int pid, String stat) {
            int close = stat.lastIndexOf(") "), open = stat.indexOf(" (");
            if (open <= 0 || close <= open || !stat.substring(0, open).equals(Integer.toString(pid)))
                throw blocked("无法读取 dcidaemon 的进程身份");
            String[] fields = stat.substring(close + 2).split(" ");
            if (fields.length < 20 || !fields[19].matches("[1-9][0-9]*"))
                throw blocked("无法读取 dcidaemon 的启动时间");
            return fields[19];
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof ProcessIdentity)) return false;
            ProcessIdentity value = (ProcessIdentity) other;
            return pid == value.pid && startTime.equals(value.startTime) && executable.equals(value.executable);
        }
        @Override public int hashCode() { return pid * 31 + startTime.hashCode(); }
    }

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }
}
