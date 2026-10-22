package local.celllock;

import android.content.Context;
import android.os.Build;
import android.telephony.AccessNetworkConstants;
import android.telephony.NetworkRegistrationInfo;
import android.telephony.ServiceState;
import android.telephony.TelephonyManager;
import java.util.List;

final class NrRegistration {
    static final class Snapshot {
        final boolean registered;
        final String detail;

        Snapshot(boolean registered, String detail) {
            this.registered = registered;
            this.detail = detail;
        }
    }

    static Snapshot read(Context context, int subId) {
        if (subId < 0 || subId == Integer.MAX_VALUE)
            return new Snapshot(false, "subscription=invalid");
        int rat = -1;
        try {
            TelephonyManager manager = context.getSystemService(TelephonyManager.class);
            if (manager == null) return new Snapshot(false, "telephony=unavailable");
            TelephonyManager subscription = manager.createForSubscriptionId(subId);
            if (subscription == null) return new Snapshot(false, "subscription_telephony=unavailable");
            rat = subscription.getDataNetworkType();
            String detail = "data_rat=" + rat;
            if (rat == TelephonyManager.NETWORK_TYPE_NR)
                return new Snapshot(true, detail + "; source=data_network_type");
            if (rat != TelephonyManager.NETWORK_TYPE_UNKNOWN)
                return new Snapshot(false, detail + "; source=data_network_type");
            if (Build.VERSION.SDK_INT < 30)
                return new Snapshot(false, detail + "; service_state=unsupported_api");

            // MIUI can report UNKNOWN here while its WWAN packet registration is NR.
            ServiceState state = Build.VERSION.SDK_INT >= 33
                ? subscription.getServiceState(TelephonyManager.INCLUDE_LOCATION_DATA_NONE)
                : subscription.getServiceState();
            if (state == null) return new Snapshot(false, detail + "; service_state=unavailable");
            List<NetworkRegistrationInfo> entries = state.getNetworkRegistrationInfoList();
            if (entries == null || entries.isEmpty())
                return new Snapshot(false, detail + "; service_state=no_registration_records");
            NetworkRegistrationInfo packet = null;
            for (NetworkRegistrationInfo entry : entries) {
                if (entry == null)
                    return new Snapshot(false, detail + "; service_state=invalid_registration_record");
                if (entry.getTransportType() != AccessNetworkConstants.TRANSPORT_TYPE_WWAN
                        || (entry.getDomain() & NetworkRegistrationInfo.DOMAIN_PS) == 0) continue;
                if (entry.getDomain() != NetworkRegistrationInfo.DOMAIN_PS || packet != null)
                    return new Snapshot(false, detail + "; service_state=ambiguous_packet_registration");
                packet = entry;
            }
            if (packet == null)
                return new Snapshot(false, detail + "; service_state=no_wwan_packet_registration");
            boolean registered = packet.isRegistered();
            int packetRat = packet.getAccessNetworkTechnology();
            return new Snapshot(registered && packetRat == TelephonyManager.NETWORK_TYPE_NR,
                detail + "; source=wwan_packet_registration; ps_registered=" + registered + "; ps_rat=" + packetRat);
        } catch (Exception error) {
            return new Snapshot(false, "data_rat=" + rat + "; query_error=" + error.getClass().getSimpleName());
        }
    }

    private NrRegistration() {}
}
