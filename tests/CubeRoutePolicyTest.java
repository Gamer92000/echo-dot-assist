public class CubeRoutePolicyTest {
    static void equal(int want,int got){if(want!=got)throw new AssertionError(want+" != "+got);}
    static void route(int mode,int state,boolean endpoint,boolean fresh,int out,int why){
        int[] r=CubeRoutePolicy.select(mode,state,endpoint,fresh);equal(out,r[0]);equal(why,r[1]);
    }
    public static void main(String[] args){
        // Recorded direct Samsung sequence: Cube awake throughout; HPD/HDCP stay on.
        equal(1,CubeRoutePolicy.detect(-1,100,1,1,1,true,true));
        equal(2,CubeRoutePolicy.detect(-1,100,1,0,1,false,true));
        equal(1,CubeRoutePolicy.detect(-1,100,1,1,1,true,true));
        // The previous AVR cannot inherit a calibration for a different EDID.
        equal(0,CubeRoutePolicy.detect(-1,100,1,1,1,true,false));
        equal(0,CubeRoutePolicy.detect(0,30001,1,1,1,true,false));
        equal(1,CubeRoutePolicy.detect(0,30000,1,1,1,true,false));
        equal(2,CubeRoutePolicy.detect(1,10,1,1,1,true,false));
        equal(2,CubeRoutePolicy.detect(0,10,0,0,0,false,false));
        equal(0,CubeRoutePolicy.detect(-1,0,1,1,0,false,true));
        route(0,1,true,true,2,0);route(0,2,false,true,1,0);
        route(0,0,true,true,1,1);route(0,1,true,false,1,4);
        route(1,1,true,true,1,0);route(2,2,true,true,2,0);
        route(2,1,false,true,1,2);
        float g=CubeRoutePolicy.gain(80,true,true,25);
        if(Math.abs(g-.2f)>.0001||CubeRoutePolicy.gain(80,true,false,25)!=.8f||CubeRoutePolicy.gain(80,false,true,25)!=.8f)throw new AssertionError("gain");
        System.out.println("PASS: recorded TV transitions, sink change, stale/unknown fallback, forced modes and duck recovery");
    }
}
