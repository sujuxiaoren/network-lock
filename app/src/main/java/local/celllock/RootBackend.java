package local.celllock;

import android.content.Context;
import android.os.Build;
import android.os.SystemClock;
import android.telephony.SubscriptionManager;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

/** Experimental nuwa profile; configuration readback is not proof of cell camping. */
public final class RootBackend {
    private final Context context;
    private final CommandRunner commandRunner;
    private final SaRegistrationReader saRegistrationReader;
    private final StringBuilder log = new StringBuilder();
    private static final int MAX_OUTPUT = 32768;
    private static final String LIBRARY_SHA256 = "942c44917e383139e1d595aa040a929d1a6a4797efd06b32a3370072e9181d21";
    private static final String BASEBAND = "MPSS.DE.3.0-Jun  1 2023-07:50:50";
    // Pinned to this verified profile: original selector does not match DE.3.0.
    static final int NR_CELL_FORMAT = 1;
    private volatile boolean writeReady, recoveryAvailable, recoveryKnown;
    private volatile boolean readyNrSa;
    private String diagnosticsNote = "";

    public RootBackend(Context context) { this(context, RootBackend::runRootProcess); }

    // Package-private command boundary allows host tests without Root or modem access.
    RootBackend(Context context, CommandRunner commandRunner) {
        this(context, commandRunner, null);
    }

    RootBackend(Context context, CommandRunner commandRunner, SaRegistrationReader saRegistrationReader) {
        this.context = context.getApplicationContext();
        this.commandRunner = commandRunner;
        this.saRegistrationReader = saRegistrationReader == null ? this::isDefaultDataRegisteredSa : saRegistrationReader;
    }

    public String probe() { return probe(false); }

    public String probe(boolean nrSa) {
        writeReady = false;
        readyNrSa = nrSa;
        recoveryKnown = false;
        diagnosticsNote = "";
        boolean rootConfirmed = false;
        StringBuilder result = new StringBuilder();
        StringBuilder brief = new StringBuilder("设备：" + Build.MODEL + " / Android " + Build.VERSION.RELEASE + "\n");
        result.append("设备：").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append(" / ").append(Build.DEVICE).append("\nAndroid ").append(Build.VERSION.RELEASE)
            .append(" / SDK ").append(Build.VERSION.SDK_INT).append('\n');
        try {
            Result root = runRoot("id", 25);
            if (root.code != 0 || !root.text.contains("uid=0("))
                throw new IllegalStateException("Root 授权未成功：" + root.text);
            rootConfirmed = true;
            result.append("Root：已确认\n");
            brief.append("Root：已确认\n");
            Result system = runRoot("getprop ro.board.platform\ngetprop ro.miui.ui.version.name\n"
                + "getprop gsm.version.baseband\nls -l /dev/diag /vendor/lib64/libdiag.so", 10);
            result.append(system.text).append('\n');
            if (!"nuwa".equals(Build.DEVICE) || Build.VERSION.SDK_INT != 33) {
                result.append("当前系统不在本版诊断接口核对范围内，未调用基带接口。\n");
                brief.append("当前系统不在本版诊断接口核对范围内。\n");
            } else {
                verifyProfile();
                Result probe = runNative(nrSa ? "read-nr" : "read");
                // DIAG results decide capability; only the independent status query decides recovery.
                writeReady = probe.code == 0 && hasFlag(probe.text,
                    nrSa ? "CELLLOCK_NR_PROFILE_READY" : "CELLLOCK_WRITE_READY");
                result.append("基带只读探测（退出码 ").append(probe.code).append("）：\n").append(probe.text).append('\n');
                brief.append(probe.text.contains("successfully connected to socket")
                    ? "诊断服务：连接成功\n" : "诊断服务：未确认连接成功\n");
                int valid = 0, failed = 0;
                for (String line : probe.text.split("\n")) {
                    if (line.startsWith("ITEM:")) ++valid;
                    if (line.startsWith("ITEM_ERROR:")) {
                        ++failed;
                        brief.append("项目查询失败：").append(line.substring(11).trim()).append('\n');
                    }
                    if (line.startsWith("CELLLOCK_ERROR=")) brief.append(line.substring(15).trim()).append('\n');
                    if (line.startsWith("CELLLOCK_REASON=")) {
                        String reason = line.substring(16).trim();
                        if (reason.matches("Item [0-4] is not confirmed absent \\(errno 0\\); existing settings must be preserved")) {
                            writeReady = false;
                            brief.append("检测到预存的 LTE 配置，尚未确认能完整备份及原样恢复，锁定保持禁用；配置是否正在生效尚未确认。\n");
                        } else brief.append(reason).append('\n');
                    }
                }
                brief.append("配置只读查询：").append(valid).append(" 项有效回复，").append(failed).append(" 项失败\n");
                if (failed > 0) writeReady = false;
                if (!writeReady) {
                    if (probe.code != 0 || failed > 0) brief.append("诊断查询未通过（退出码 ").append(probe.code)
                        .append("），锁定写入保持禁用。\n");
                    else brief.append("锁定条件检查未通过，锁定写入保持禁用。\n");
                }
                String firstNetwork = firstSlotNetwork();
                result.append("第一卡槽网络：").append(firstNetwork).append('\n');
                brief.append("第一卡槽网络：").append(firstNetwork).append('\n');
                if (nrSa ? !isRegisteredSa(firstNetwork) : !isLte(firstNetwork)) {
                    writeReady = false;
                    brief.append(nrSa ? "第一卡槽尚未确认 5G SA 数据注册；请确认主卡处于 SA 后重新检测。\n"
                        : "当前选择 LTE；请先切换到 4G，再重新检测。\n");
                }
            }
        } catch (Exception e) {
            writeReady = false;
            result.append("检测未完成：").append(e.getClass().getSimpleName()).append(": ").append(e.getMessage()).append('\n');
            brief.append("检测未完成：").append(e.getMessage()).append('\n');
        } finally {
            if (rootConfirmed) {
                String recoveryError = refreshRecovery();
                if (!recoveryError.isEmpty()) brief.append(recoveryError).append('\n');
                else brief.append(recoveryAvailable ? "恢复记录：存在待恢复记录。\n"
                    : "恢复记录：未发现待恢复记录。\n");
            }
            writeReady = writeReady && recoveryKnown && !recoveryAvailable;
        }
        result.append("WRITE_READY=").append(writeReady).append("; RECOVERY=")
            .append(recoveryKnown ? String.valueOf(recoveryAvailable) : "unknown")
            .append("\n本次检测没有修改小区锁定配置。");
        append(result.toString());
        if (!diagnosticsNote.isEmpty()) brief.append("采集协调：").append(diagnosticsNote).append('\n');
        brief.append(!recoveryKnown ? "恢复记录状态未确认，锁定已禁用。请重新检测；必要时可尝试恢复，由后端核对记录。"
            : recoveryAvailable ? "有恢复记录，请先恢复修改前配置。"
            : writeReady ? "可以尝试写入；本版仅支持默认数据卡位于 SIM 卡槽 1。\n锁定前自动备份本次配置，恢复回本次操作前状态。\n写入后会读回核对并请求基带重启，两张 SIM 将短暂断网；实际驻留须在网络恢复后另行刷新核对。"
            : "当前条件不满足，锁定写入保持禁用。")
            .append("\n本次检测没有修改锁定配置；详细日志可导出。");
        return brief.toString();
    }

    public boolean canWrite() { return writeReady && recoveryKnown && !recoveryAvailable; }
    public boolean canWrite(boolean nrSa) { return canWrite() && readyNrSa == nrSa; }
    public void invalidateReadiness() { writeReady = false; }
    public boolean canRestore() { return !recoveryKnown || recoveryAvailable; }
    public boolean isRecoveryUnknown() { return !recoveryKnown; }

    public String lock(int earfcn, int pci, int slot) {
        if (earfcn < 0 || earfcn > 65535 || pci < 0 || pci > 503 || slot != 0)
            throw new IllegalArgumentException("参数超出本版范围：EARFCN 0–65535、PCI 0–503，仅 slot 0。");
        return lockTarget("lock " + earfcn + " " + pci, "锁小区",
            "EARFCN " + earfcn + " / PCI " + pci, "LOCK EARFCN=" + earfcn + " PCI=" + pci + " SLOT=0", false);
    }

    public String lockEarfcn(int earfcn, int slot) {
        if (earfcn < 0 || earfcn > 65535 || slot != 0)
            throw new IllegalArgumentException("参数超出本版范围：EARFCN 0–65535，仅 slot 0。");
        return lockTarget("lock-earfcn " + earfcn, "锁频点",
            "EARFCN " + earfcn + "（不限制 PCI）", "LOCK_EARFCN EARFCN=" + earfcn + " SLOT=0", false);
    }

    public String lockNrArfcn(int nrArfcn, int scsCode) {
        if (nrArfcn < 0 || nrArfcn > 3279165 || scsCode < 0 || scsCode > 4)
            throw new IllegalArgumentException("参数超出范围：NR-ARFCN 0–3279165，SCS 编码 0–4。");
        return lockTarget("lock-nr-arfcn " + nrArfcn + " " + scsCode, "5G SA 频点",
            "NR-ARFCN " + nrArfcn + " / SCS " + (15 << scsCode) + " kHz",
            "LOCK_NR_ARFCN NR_ARFCN=" + nrArfcn + " SCS_CODE=" + scsCode + " SLOT=0", true);
    }

    public String lockNrCell(int nrArfcn, int pci, int scsCode, int nrBand) {
        if (nrArfcn < 0 || nrArfcn > 3279165 || pci < 0 || pci > 1007 || scsCode < 0 || scsCode > 4)
            throw new IllegalArgumentException("参数超出范围：NR-ARFCN 0–3279165，PCI 0–1007，SCS 编码 0–4。");
        if (NrBandPolicy.find(nrArfcn, nrBand) == null)
            throw new IllegalArgumentException("请选择与目标 NR-ARFCN 匹配的受支持 NR 频段。");
        return lockTarget("lock-nr-cell " + nrArfcn + " " + pci + " " + scsCode + " " + nrBand + " " + NR_CELL_FORMAT, "5G SA 小区",
            "NR-ARFCN " + nrArfcn + " / PCI " + pci + " / n" + nrBand + " / SCS " + (15 << scsCode) + " kHz",
            "LOCK_NR_CELL NR_ARFCN=" + nrArfcn + " PCI=" + pci + " SCS_CODE=" + scsCode
                + " NR_BAND=" + nrBand + " FORMAT=" + NR_CELL_FORMAT + " SLOT=0", true);
    }

    private String lockTarget(String command, String mode, String target, String operationLog, boolean nrSa) {
        if (!canWrite(nrSa)) throw new IllegalStateException("请先检测当前制式的设备能力；有恢复记录时必须先恢复。");
        writeReady = false;
        recoveryKnown = false;
        try {
            verifyProfile();
            String network = firstSlotNetwork();
            if (nrSa ? !isRegisteredSa(network) : !isLte(network))
                throw new IllegalStateException(nrSa
                    ? "第一卡槽默认数据 SIM 当前未确认 5G SA 注册，请确认网络后重新检测。"
                    : "第一卡槽当前未报告 LTE，请先在系统设置中使用 4G 并重新检测。");
            Result operation = runNative(command);
            append(operationLog + "\n" + operation.text);
            updateState(operation);
            if (!operationConfirmed(operation.code, operation.text, "CELLLOCK_CONFIG_WRITTEN_AND_READBACK"))
                throw new IllegalStateException(operationError(operation.text));
            return diagnosticsPrefix() + mode + "配置已写入并读回核对，基带重启请求已收到应答。\n目标 " + target
                + "。\n请等待两张 SIM 的网络恢复，再刷新当前服务小区核对驻留。"
                + "重启应答不能证明已驻留目标或持续锁定。\n原状态已保存，可使用恢复功能。";
        } catch (Exception e) {
            refreshRecovery();
            append("LOCK FAILED: " + e.getMessage());
            throw new IllegalStateException(e.getMessage() + recoveryAdvice(), e);
        }
    }

    public String restore() {
        writeReady = false;
        recoveryKnown = false;
        try {
            verifyProfile();
            Result operation = runNative("restore");
            append("RESTORE\n" + operation.text);
            updateState(operation);
            if (!operationConfirmed(operation.code, operation.text, "CELLLOCK_ORIGINAL_CONFIG_RESTORED"))
                throw new IllegalStateException(operationError(operation.text));
            return diagnosticsPrefix() + "修改前配置已恢复并读回核对，基带重启请求已收到应答。\n"
                + "请等待两张 SIM 的网络恢复，再刷新当前服务小区。重启应答不代表网络已经恢复。\n再次锁定前先重新检测设备能力。";
        } catch (Exception e) {
            refreshRecovery();
            append("RESTORE FAILED: " + e.getMessage());
            throw new IllegalStateException(e.getMessage() + recoveryAdvice(), e);
        }
    }

    private void verifyProfile() throws Exception {
        if (!"nuwa".equals(Build.DEVICE) || Build.VERSION.SDK_INT != 33)
            throw new IllegalStateException("本版仅适配已核对的 Xiaomi 13 Pro / Android 13。");
        Result hash = runRoot("sha256sum /vendor/lib64/libdiag.so", 10);
        if (hash.code != 0 || !hash.text.trim().startsWith(LIBRARY_SHA256 + " "))
            throw new IllegalStateException("厂商诊断库版本与本版核对的版本不同，已停止调用。");
        Result version = runRoot("getprop gsm.version.baseband", 10);
        if (version.code != 0 || version.text.trim().isEmpty())
            throw new IllegalStateException("无法确认基带版本。");
        for (String part : version.text.trim().split(","))
            if (!BASEBAND.equals(part.trim())) throw new IllegalStateException("当前基带版本不在本版适配范围内。");
    }

    private String firstSlotNetwork() throws Exception {
        Result network = runRoot("getprop gsm.network.type", 10);
        if (network.code != 0 || network.text.trim().isEmpty())
            throw new IllegalStateException("无法确认第一卡槽的当前网络类型。");
        return network.text.trim().split(",", -1)[0].trim();
    }

    private static boolean isLte(String type) {
        return type.equals("LTE") || type.equals("LTE_CA");
    }

    private boolean isRegisteredSa(String type) throws Exception {
        return (type.equals("NR_SA") || type.equals("NR")) && saRegistrationReader.isRegistered();
    }

    private boolean isDefaultDataRegisteredSa() {
        int subId = SubscriptionManager.getDefaultDataSubscriptionId();
        int slot = subId < 0 || subId == Integer.MAX_VALUE ? -1 : SubscriptionManager.getSlotIndex(subId);
        if (slot != 0) {
            append("SA 注册核对：defaultSub=" + subId + "; slot=" + slot + "; routeConfirmed=false");
            return false;
        }
        NrRegistration.Snapshot registration = NrRegistration.read(context, subId);
        int afterSub = SubscriptionManager.getDefaultDataSubscriptionId();
        int afterSlot = SubscriptionManager.getSlotIndex(subId);
        boolean registered = registration.registered && subId == afterSub && afterSlot == 0;
        append("SA 注册核对：defaultSub=" + subId + "; slot=" + slot + "; " + registration.detail
            + "; defaultSubAfter=" + afterSub + "; slotAfter=" + afterSlot + "; confirmed=" + registered);
        return registered;
    }

    private Result runNative(String args) throws Exception {
        File helper = new File(context.getApplicationInfo().nativeLibraryDir, "libcelllock.so");
        if (!helper.isFile()) throw new IllegalStateException("安装器没有解压诊断组件，请重新安装完整 APK。");
        boolean locksNr = args.startsWith("lock-nr-arfcn ") || args.startsWith("lock-nr-cell ");
        boolean locksRadio = args.startsWith("lock ") || args.startsWith("lock-earfcn ") || locksNr;
        boolean usesDiagnostics = args.equals("read") || args.equals("read-nr") || args.equals("restore") || locksRadio;
        if (usesDiagnostics) {
            diagnosticsNote = "";
            try { diagnosticsNote = new DiagnosticsCoordinator(commandRunner, this::append).ensureIdle(); }
            catch (Exception e) { append("采集协调失败：" + e.getMessage()); throw e; }
        }
        if (locksNr && !isRegisteredSa(firstSlotNetwork()))
            throw new IllegalStateException("启动前默认数据卡的 5G SA 注册状态已变化，请重新检测。");
        long deadlineMs = SystemClock.elapsedRealtime() + 45000L;
        String command = "LD_LIBRARY_PATH=/vendor/lib64:/vendor/lib64/hw:/apex/com.android.vndk.v33/lib64:/system/lib64 "
            + shellQuote(helper.getAbsolutePath()) + " --deadline-ms " + deadlineMs + " " + args;
        Result result = runRoot(usesDiagnostics ? DiagnosticsCoordinator.guardNativeCommand(command) : command,
            60, args.equals("restore") || locksRadio);
        if (usesDiagnostics && hasFlag(result.text, "CELLLOCK_DIAGNOSTICS_BLOCKED")) {
            append("采集协调：启动前再次检测到 dcidaemon 或进程查询异常。\n" + result.text);
            throw new IllegalStateException("启动前诊断采集状态发生变化，未启动本应用的诊断操作。请重新操作。");
        }
        return result;
    }

    private String diagnosticsPrefix() { return diagnosticsNote.isEmpty() ? "" : "采集协调：" + diagnosticsNote + "\n"; }

    private void updateState(Result result) {
        recoveryKnown = false;
        if (result.code != 0) return;
        for (String line : result.text.split("\n")) {
            if (line.trim().equals("CELLLOCK_RECOVERY=1")) {
                recoveryAvailable = true; recoveryKnown = true;
            } else if (line.trim().equals("CELLLOCK_RECOVERY=0")) {
                recoveryAvailable = false; recoveryKnown = true;
            }
        }
    }

    private String refreshRecovery() {
        recoveryKnown = false;
        try {
            Result state = runNative("status");
            updateState(state);
            append("恢复记录独立查询（退出码 " + state.code + "）：\n" + state.text);
            if (recoveryKnown) return "";
            return "恢复记录查询未获得有效结果（退出码 " + state.code + "）：" + state.text;
        } catch (Exception e) {
            String error = "无法刷新恢复记录状态，请重新检测：" + e.getMessage();
            append(error);
            return error;
        }
    }

    private static String operationError(String text) {
        String error = "配置操作或重启请求的完整结果未能确认，请查看日志。";
        for (String line : text.split("\n")) if (line.startsWith("CELLLOCK_ERROR=")) error = line.substring(15).trim();
        if (!hasFlag(text, "CELLLOCK_RESTART_RECEIPT_CONFIRMED")) {
            if (hasFlag(text, "CELLLOCK_RESTART_NOT_REQUESTED")
                    && !hasFlag(text, "CELLLOCK_RESTART_REQUEST_SENT"))
                error += "\n本次未发起基带重启请求。";
            else error += "\n未确认基带重启应答；请求可能已经发送，不会自动重试。";
        }
        return error;
    }

    private String recoveryAdvice() {
        return !recoveryKnown ? "\n恢复记录状态未确认，锁定已禁用。请先重新检测；恢复入口保留，由后端核对记录。"
            : recoveryAvailable ? "\n恢复记录仍在，锁定已禁用。请核对状态后使用恢复功能，恢复也会重启基带。"
            : "\n当前未发现恢复记录；再次写入前请重新检测。";
    }

    private static boolean hasFlag(String text, String name) {
        boolean enabled = false;
        String prefix = name + "=";
        for (String line : text.split("\n")) {
            String value = line.trim();
            if (value.startsWith(prefix)) enabled = value.equals(prefix + "1");
        }
        return enabled;
    }

    private static boolean operationConfirmed(int code, String text, String configurationFlag) {
        return code == 0 && hasFlag(text, configurationFlag)
            && hasFlag(text, "CELLLOCK_RESTART_RECEIPT_CONFIRMED");
    }

    public synchronized String getLog() { return log.toString(); }

    private synchronized void append(String text) {
        log.append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()))
            .append('\n').append(text).append("\n\n");
        if (log.length() > 100000) log.delete(0, log.length() - 100000);
    }

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    static final class Result {
        final int code;
        final String text;
        Result(int code, String text) { this.code = code; this.text = text; }
    }

    interface CommandRunner {
        Result run(String command, int timeoutSeconds, boolean changesRadio) throws Exception;
    }

    interface SaRegistrationReader {
        boolean isRegistered() throws Exception;
    }

    private Result runRoot(String command, int timeoutSeconds) throws Exception {
        return runRoot(command, timeoutSeconds, false);
    }

    private Result runRoot(String command, int timeoutSeconds, boolean changesRadio) throws Exception {
        return commandRunner.run(command, timeoutSeconds, changesRadio);
    }

    static String timeoutMessage(boolean changesRadio) {
        return changesRadio
            ? "Root 操作超时，配置或重启结果未确认；请求可能已经发送，不会自动重试。请重新检测恢复记录。"
            : "Root 只读查询超时，未取得查询结果。请重新检测。";
    }

    private static Result runRootProcess(String command, int timeoutSeconds, boolean changesRadio) throws Exception {
        Process process = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        Thread drain = new Thread(() -> {
            try (InputStream input = process.getInputStream()) {
                byte[] bytes = new byte[2048];
                int n;
                while ((n = input.read(bytes)) != -1) {
                    synchronized (output) {
                        int keep = Math.min(n, Math.max(0, MAX_OUTPUT - output.size()));
                        output.write(bytes, 0, keep);
                    }
                }
            } catch (Exception ignored) { }
        }, "celllock-root-output");
        drain.setDaemon(true);
        drain.start();
        try {
            if (!process.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                if (command.equals(DiagnosticsCoordinator.STOP_COMMAND))
                    throw new IllegalStateException("停止 Cellular-Pro 的 Root 命令超时，停止结果尚未确认。");
                throw new IllegalStateException(timeoutMessage(changesRadio));
            }
            drain.join(1000);
            synchronized (output) {
                return new Result(process.exitValue(), new String(output.toByteArray(), StandardCharsets.UTF_8));
            }
        } finally {
            process.destroy();
        }
    }
}
