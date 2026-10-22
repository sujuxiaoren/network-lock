package local.celllock;

import java.util.List;

/** A band choice belongs to one target. Only target-matching evidence may follow it. */
final class NrBandChoice {
    static final int UNKNOWN = 0, OBSERVED = 1, RANGE = 2, MANUAL = 3;

    static final class Selection {
        final int band, source, observedBand;
        Selection(int band, int source, int observedBand) {
            this.band = band; this.source = source; this.observedBand = observedBand;
        }
        boolean conflicts() { return source == MANUAL && observedBand > 0 && band != observedBand; }
    }

    private int arfcn = -1, pci = -1, manual = -1, confirmedBand = -1;
    private boolean cellMode;

    void targetChanged(int targetArfcn, int targetPci, boolean targetCellMode) {
        int normalizedPci = targetCellMode ? targetPci : -1;
        if (arfcn != targetArfcn || pci != normalizedPci || cellMode != targetCellMode) {
            manual = -1;
            confirmedBand = -1;
        }
        arfcn = targetArfcn; pci = normalizedPci; cellMode = targetCellMode;
    }

    void choose(int band) {
        if (band != -1 && NrBandPolicy.find(arfcn, band) == null)
            throw new IllegalArgumentException("请选择与目标频点匹配的 NR 频段。");
        manual = band;
    }

    void clearOverride() { manual = -1; confirmedBand = -1; }
    int manualBand() { return manual; }

    Selection resolve(int observedArfcn, int observedPci, int[] bands, boolean usable) {
        return resolve(observedArfcn, observedPci, bands, usable, manual);
    }

    Selection automatic(int observedArfcn, int observedPci, int[] bands, boolean usable) {
        return resolve(observedArfcn, observedPci, bands, usable, -1);
    }

    private Selection resolve(int observedArfcn, int observedPci, int[] bands, boolean usable, int manualBand) {
        boolean matches = usable && arfcn == observedArfcn
            && (!cellMode || (pci >= 0 && pci <= 1007 && pci == observedPci));
        if (matches && bands != null && bands.length > 0)
            confirmedBand = NrBandPolicy.uniqueObserved(arfcn, bands);
        int observed = confirmedBand;
        if (NrBandPolicy.find(arfcn, manualBand) != null) return new Selection(manualBand, MANUAL, observed);
        if (observed > 0) return new Selection(observed, OBSERVED, observed);
        // An explicit ambiguous/incompatible report must not be replaced by a range guess.
        if (matches && bands != null && bands.length > 0) return new Selection(-1, UNKNOWN, -1);
        List<NrBandPolicy.Band> candidates = NrBandPolicy.candidates(arfcn);
        return candidates.size() == 1 ? new Selection(candidates.get(0).number, RANGE, -1)
            : new Selection(-1, UNKNOWN, -1);
    }
}
