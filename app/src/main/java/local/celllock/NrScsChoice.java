package local.celllock;

/** A manual SCS choice belongs to one target in this screen session. */
final class NrScsChoice {
    private int target = -1, manual = -1;

    void targetChanged(int arfcn) {
        if (target != arfcn) manual = -1;
        target = arfcn;
    }

    void choose(int code) {
        if (code < -1 || code > 4 || (code >= 0 && (target < 0 || target > 3279165)))
            throw new IllegalArgumentException("请先填写频点，再选择 SCS。");
        manual = code;
    }

    void clearOverride() { manual = -1; }
    int manualCode() { return manual; }
    int target() { return target; }

    void restore(int savedTarget, int savedCode) {
        manual = target >= 0 && target <= 3279165 && target == savedTarget
            && savedCode >= 0 && savedCode <= 4 ? savedCode : -1;
    }
}
