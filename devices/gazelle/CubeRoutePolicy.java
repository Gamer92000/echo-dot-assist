/** Pure routing decisions. A cable or cached HDCP value alone never proves TV power. */
public final class CubeRoutePolicy {
    public static final int UNKNOWN=0, ON=1, OFF=2;
    public static final int AUTO=0, CUBE=1, TV=2;
    public static final int NONE=0, UNKNOWN_TV=1, HDMI_MISSING=2, ROUTE_FAILED=3, STALE=4;
    public static int detect(int cec, long cecAge, int hpd, int rx, int ready,
                             boolean hdmi, boolean verifiedSink) {
        if(hpd==0)return OFF;
        if(cecAge>=0&&cecAge<=30000){
            if(cec==0)return ON;
            if(cec==1)return OFF;
            // CEC transitioning states are not stable enough to route to HDMI.
        }
        if(verifiedSink){
            if(rx==0)return OFF;
            if(rx==1&&ready==1&&hdmi)return ON;
        }
        return UNKNOWN;
    }
    /** Return selected output and an explicit fallback reason. Forced TV cannot invent an endpoint. */
    public static int[] select(int mode,int state,boolean hdmi,boolean fresh) {
        if(mode==CUBE)return new int[]{CUBE,NONE};
        if(mode==AUTO&&(!fresh||state==UNKNOWN))return new int[]{CUBE,fresh?UNKNOWN_TV:STALE};
        if(mode==AUTO&&state==OFF)return new int[]{CUBE,NONE};
        return hdmi?new int[]{TV,NONE}:new int[]{CUBE,HDMI_MISSING};
    }
    public static float gain(int volume,boolean music,boolean busy,int duck) {
        return Math.max(0,Math.min(100,volume))/100f*(music&&busy?Math.max(0,Math.min(100,duck))/100f:1f);
    }
}
