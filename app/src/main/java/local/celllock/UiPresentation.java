package local.celllock;

/** Short display text only; the backend remains responsible for operation and recovery state. */
final class UiPresentation {
    private UiPresentation() { }

    /** A completed restore is not permission to start another lock. UI only. */
    static final class RestoreNotice {
        private boolean confirmed;

        void begin(boolean updatesDeviceState) {
            if (updatesDeviceState) confirmed = false;
        }

        void complete(boolean restoreOperation, boolean failed, boolean returnedResult,
                      boolean recoveryUnknown, boolean restorable) {
            if (restoreOperation)
                confirmed = !failed && returnedResult && !recoveryUnknown && !restorable;
        }

        boolean visible(boolean busy, boolean recoveryUnknown, boolean restorable) {
            return confirmed && !busy && !recoveryUnknown && !restorable;
        }
    }

    static String summary(String operation, String output, boolean failed) {
        String task = operation == null ? "" : operation;
        String detail = output == null ? "" : output;
        String problem = problemSummary(detail);
        if (problem != null) return problem;
        // Never turn a failed operation into success because its output contains an earlier receipt.
        if (failed) return "操作未完成，请查看详情。";
        if (detail.trim().isEmpty() || detail.contains("后端未返回结果"))
            return "操作结果未确认，请查看详情。";
        if (task.startsWith("锁 SA 频点")
                && detail.contains("SA 频点配置已写入并读回核对，基带重启请求已收到应答。"))
            return "SA 频点配置已应用。\n等待网络恢复后，刷新核对驻留。";
        if (task.startsWith("锁 SA 小区")
                && detail.contains("SA 小区配置已写入并读回核对，基带重启请求已收到应答。"))
            return "SA 小区配置已应用。\n等待网络恢复后，刷新核对驻留。";
        if (task.startsWith("锁频点")
                && detail.contains("锁频点配置已写入并读回核对，基带重启请求已收到应答。"))
            return "频点配置已应用。\n等待网络恢复后，刷新核对驻留。";
        if (task.startsWith("锁小区")
                && detail.contains("锁小区配置已写入并读回核对，基带重启请求已收到应答。"))
            return "小区配置已应用。\n等待网络恢复后，刷新核对驻留。";
        if (task.startsWith("恢复")
                && detail.contains("修改前配置已恢复并读回核对，基带重启请求已收到应答。"))
            return "已恢复原配置。\n等待网络恢复；再次锁定前请检测设备。";
        if (task.equals("导出诊断日志") && detail.contains("诊断日志已写入你选择的文件。"))
            return "诊断日志已保存。";
        if (task.startsWith("检测")) {
            if (detail.contains("有恢复记录，请先恢复修改前配置。"))
                return "发现待恢复记录，请先恢复原配置。";
            if (detail.contains("可以尝试写入；")) return "设备检测通过，可以设置锁定目标。";
            return "设备检测未通过，请查看详情。";
        }
        return "操作结果未确认，请查看详情。";
    }

    static String nextStep(boolean failed, boolean recoveryUnknown, boolean restorable, boolean ready) {
        if (recoveryUnknown) return "请重新检测，确认恢复记录状态。";
        if (restorable) return failed ? "请查看详情并核对恢复记录。"
                : "有待恢复记录；恢复后才可再次锁定。";
        if (failed) return "请查看详情，处理后重新检测。";
        return ready ? "设置目标后即可锁定。" : "请先检测设备。";
    }

    private static String problemSummary(String detail) {
        if (detail.contains("Root 授权未成功")) return "Root 授权未完成，请查看详情。";
        if (detail.contains("本版仅适配") || detail.contains("不在本版适配范围")
                || detail.contains("不在本版诊断接口核对范围") || detail.contains("厂商诊断库版本与本版核对的版本不同"))
            return "当前设备或系统暂不支持，请查看详情。";
        if (detail.contains("未启动本应用的诊断操作") || detail.contains("CELLLOCK_DIAGNOSTICS_BLOCKED=1"))
            return "诊断采集未能协调，请查看详情。";
        if (detail.contains("未确认基带重启应答") || detail.contains("配置或重启结果未确认")
                || detail.contains("配置操作或重启请求的完整结果未能确认"))
            return "操作结果未确认，请查看详情。\n请勿重复操作，先核对恢复记录。";
        if (detail.contains("Root 只读查询超时")) return "设备查询超时，请重新检测。";
        if (detail.contains("未确认 SA 数据注册") || detail.contains("未确认 5G SA") || detail.contains("当前未报告 5G SA")
                || detail.contains("当前未报告 NR SA"))
            return "请将卡槽 1 切换到 5G SA，再检测设备。";
        if (detail.contains("本版仅处理 LTE") || detail.contains("第一卡槽当前未报告 LTE"))
            return "请将卡槽 1 切换到 4G，再检测设备。";
        if (detail.contains("恢复记录状态未确认") || detail.contains("恢复记录查询未获得有效结果"))
            return "恢复记录状态未确认，请重新检测。";
        if (detail.contains("尚未确认能完整备份及原样恢复"))
            return "现有配置未通过备份检查，请查看详情。";
        if (detail.contains("检测未完成：") || detail.contains("诊断查询未通过")
                || detail.contains("锁定条件检查未通过") || detail.contains("当前条件不满足"))
            return "设备检测未通过，请查看详情。";
        return null;
    }
}
