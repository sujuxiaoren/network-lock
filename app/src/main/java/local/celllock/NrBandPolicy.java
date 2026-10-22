package local.celllock;

import java.util.ArrayList;
import java.util.List;

/** Bounded downlink-band candidates; overlap is never evidence of a unique band. */
final class NrBandPolicy {
    static final class Band {
        final int number;
        final int scsCode;
        final String label;
        Band(int number, int scsCode, String label) {
            this.number = number; this.scsCode = scsCode; this.label = label;
        }
    }

    private NrBandPolicy() { }

    static List<Band> candidates(int arfcn) {
        List<Band> result = new ArrayList<>();
        // n1 and n66 overlap but have different retained SCS defaults.
        add(result, arfcn, 422000, 434000, 1, 0, "n1");
        add(result, arfcn, 422000, 440000, 66, 1, "n66");
        add(result, arfcn, 185000, 192000, 8, 0, "n8");
        add(result, arfcn, 151601, 158199, 28, 0, "n28");
        add(result, arfcn, 361000, 376000, 3, 1, "n3");
        add(result, arfcn, 499200, 538000, 41, 1, "n41");
        // Retain both; a fresh target-matching report can disambiguate them.
        add(result, arfcn, 620000, 680000, 77, 1, "n77");
        add(result, arfcn, 620000, 653333, 78, 1, "n78");
        add(result, arfcn, 693334, 733333, 79, 1, "n79");
        return result;
    }

    static Band find(int arfcn, int band) {
        for (Band candidate : candidates(arfcn)) if (candidate.number == band) return candidate;
        return null;
    }

    static int[] numbers(int arfcn) {
        List<Band> bands = candidates(arfcn);
        int[] result = new int[bands.size()];
        for (int i = 0; i < bands.size(); ++i) result[i] = bands.get(i).number;
        return result;
    }

    static int uniqueObserved(int arfcn, int[] bands) {
        if (bands == null || bands.length == 0) return -1;
        int unique = -1;
        for (int band : bands) {
            if (find(arfcn, band) == null || (unique != -1 && unique != band)) return -1;
            unique = band;
        }
        return unique;
    }

    private static void add(List<Band> output, int arfcn, int first, int last,
                            int number, int scsCode, String label) {
        if (arfcn >= first && arfcn <= last) output.add(new Band(number, scsCode, label));
    }
}
