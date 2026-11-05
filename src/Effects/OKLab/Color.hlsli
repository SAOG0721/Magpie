// Oklab matrices: Bjorn Ottosson, public domain, 2021-01-25 revision.
// https://bottosson.github.io/posts/oklab/
// Working RGB is linear Rec.709, relative to the configured reference white.
#ifndef MAGPIE_OKLAB_COLOR
#define MAGPIE_OKLAB_COLOR
cbuffer OKLabRuntime : register(b2) {
    float okDeltaSeconds;
    uint okHistoryValid;
    uint okLinearInput;
    uint okNeutral;
    float4 okLutMinimum;
    float4 okLutMaximum;
};
static const float OK_PI = 3.141592653589793;
float OKDecode1(float v) {
    float a=abs(v);
    return sign(v)*(a<=.04045 ? a/12.92 : pow((a+.055)/1.055,2.4));
}
float OKEncode1(float v) {
    float a=abs(v);
    return sign(v)*(a<=.0031308 ? a*12.92 : 1.055*pow(a,1.0/2.4)-.055);
}
float3 OKDecode(float3 v) { return float3(OKDecode1(v.r),OKDecode1(v.g),OKDecode1(v.b)); }
float3 OKEncode(float3 v) { return float3(OKEncode1(v.r),OKEncode1(v.g),OKEncode1(v.b)); }
float3 OKToLab(float3 c) {
    float3 q=mul(float3x3(.4122214708,.5363325363,.0514459929,
        .2119034982,.6806995451,.1073969566,
        .0883024619,.2817188376,.6299787005),c);
    q=sign(q)*pow(abs(q),1.0/3.0);
    return mul(float3x3(.2104542553,.7936177850,-.0040720468,
        1.9779984951,-2.4285922050,.4505937099,
        .0259040371,.7827717662,-.8086757660),q);
}
float3 OKFromLab(float3 c) {
    float3 q=float3(c.x+.3963377774*c.y+.2158037573*c.z,
        c.x-.1055613458*c.y-.0638541728*c.z,
        c.x-.0894841775*c.y-1.2914855480*c.z);
    q=q*q*q;
    return mul(float3x3(4.0767416621,-3.3077115913,.2309699292,
        -1.2684380046,2.6097574011,-.3413193965,
        -.0041960863,-.7034186147,1.7076147010),q);
}
float OKLuma(float3 c) { return dot(c,float3(.2126,.7152,.0722)); }
float3 OKRead(float3 c) { return okLinearInput!=0 ? c*(80.0/referenceWhite) : OKDecode(c); }
float3 OKWrite(float3 c) { return okLinearInput!=0 ? c*(referenceWhite/80.0) : OKEncode(c); }
float OKLimit() { return okLinearInput!=0 ? float(peakNits)/float(referenceWhite) : 1.0; }
float OKHue(float2 ab) { return dot(ab,ab)>1e-20 ? frac(atan2(ab.y,ab.x)/(2*OK_PI)+1) : 0; }
float OKGrayGate(float c) { return c*c/(c*c+.0001); }
float2 OKDirection(float degrees) { float a=degrees*(OK_PI/180); return float2(cos(a),sin(a)); }
float2 OKRotate(float2 ab,float degrees) { float2 r=OKDirection(degrees); return float2(ab.x*r.x-ab.y*r.y,ab.x*r.y+ab.y*r.x); }
float OKCircleDistance(float x,float y) { return abs(frac(x-y+.5)-.5); }
float OKBand(float v,float low,float high,float width) {
    float lo=min(low,high), hi=max(low,high), w=max(width,.0001);
    return smoothstep(lo-w,lo,v)*(1-smoothstep(hi,hi+w,v));
}
float3 OKZones(float l) {
    float s=1-smoothstep(.25+.15*balance-zoneWidth,.25+.15*balance+zoneWidth,l);
    float h=smoothstep(.75+.15*balance-zoneWidth,.75+.15*balance+zoneWidth,l);
    return float3(s,max(0,1-s-h),h);
}
float OKHueMask(float h,float center,float width,float feather) {
    float d=OKCircleDistance(h,center/360)*360;
    return 1-smoothstep(max(0,width*(1-feather)),width+max(1,width*feather),d);
}
float3 OKGamutRGB(float3 rgb) {
    if(okLinearInput==0 || outputGamut==0) return rgb;
    // D65 matrices derived from the published sRGB / P3 primaries.
    if(outputGamut==1) return mul(float3x3(.8224619687,.1775380313,0,
        .0331941989,.9668058011,0,.0170826307,.0723974407,.9105199286),rgb);
    return mul(float3x3(.62740390,.32928304,.04331307,
        .06909729,.91954040,.01136232,.01639144,.08801331,.89559525),rgb);
}
bool OKInGamut(float3 rgb,float limit) {
    float3 v=OKGamutRGB(rgb);
    return all(v>=-1e-6) && all(v<=limit+1e-6);
}
float3 OKMap(float3 lab,out float compression,out bool outside) {
    float limit=OKLimit(), maxL=pow(abs(limit),1.0/3.0);
    outside=!OKInGamut(OKFromLab(lab),limit);
    lab.x=clamp(lab.x,0,maxL);
    float c=length(lab.yz);
    compression=0;
    if(c<1e-8) return OKFromLab(float3(lab.x,0,0));
    // Reference ray search. Keep signed Rec.709 values for a wide output gamut.
    float lo=0, hi=max(c,1.5*maxL);
    if(outside || gamutSoft>0) {
        [loop] for(int i=0;i<24;++i) {
            float mid=(lo+hi)*.5;
            if(OKInGamut(OKFromLab(float3(lab.x,lab.yz*(mid/c))),limit)) lo=mid;
            else hi=mid;
        }
        float mapped=min(c,lo);
        if(gamutSoft>0 && lo>1e-8) {
            float knee=lo*(1-.35*gamutSoft), room=max(lo-knee,1e-8);
            if(c>knee) mapped=knee+room*(1-exp(-(c-knee)/room));
        }
        compression=saturate(1-mapped/c);
        lab.yz*=mapped/c;
    }
    return OKFromLab(lab);
}
float3 OKRgbToXyz(float3 c) { return mul(float3x3(.4123907993,.3575843394,.1804807884,.2126390059,.7151686788,.0721923154,.0193308187,.1191947798,.9505321522),c); }
float3 OKXyzToRgb(float3 c) { return mul(float3x3(3.2409699419,-1.5373831776,-.4986107603,-.9692436363,1.8759675015,.0415550574,.0556300797,-.2039769589,1.0569715142),c); }
float3 OKBradford(float3 c) { return mul(float3x3(.8951,.2664,-.1614,-.7502,1.7135,.0367,.0389,-.0685,1.0296),c); }
float3 OKBradfordInverse(float3 c) { return mul(float3x3(.9869929,-.1470543,.1599627,.4323053,.5183603,.0492912,-.0085287,.0400428,.9684867),c); }
// Kang et al. (2002) Planckian-locus fit, 2000..25000 K. Tint is xy delta-y.
float3 OKWhite(float temperature,float dy) {
    float t=1000/temperature;
    // Feather the fit's polynomial joins, avoiding an adjustment step at 4000 K.
    float x=lerp(-.2661239*t*t*t-.2343589*t*t+.8776956*t+.179910,
        -3.0258469*t*t*t+2.1070379*t*t+.2226347*t+.240390,smoothstep(3950,4050,temperature));
    float y=lerp(-1.1063814*x*x*x-1.3481102*x*x+2.18555832*x-.20219683,
        -.9549476*x*x*x-1.37418593*x*x+2.09137015*x-.16748867,smoothstep(2172,2272,temperature));
    y=lerp(y,3.081758*x*x*x-5.8733867*x*x+3.75112997*x-.37001483,smoothstep(3950,4050,temperature));
    y=clamp(y+dy,.05,.9);
    return float3(x/y,1,(1-x-y)/y);
}
float3 OKBalance(float3 rgb) {
    float3 cone=OKBradford(OKRgbToXyz(rgb));
    if(temperatureEnabled!=0 && (sourceKelvin!=targetKelvin || whiteTint!=0))
        cone*=OKBradford(OKWhite(float(targetKelvin),whiteTint))/max(OKBradford(OKWhite(float(sourceKelvin),0)),1e-5);
    // Relative warmth/tint use a continuous LMS gain with a fixed neutral reference.
    float3 gains=exp2(float3(.35*warmth+.15*tint,-.2*tint,-.35*warmth+.15*tint));
    if(warmth!=0 || tint!=0) {
        float3 whiteCone=OKBradford(OKRgbToXyz(1.0.xxx));
        float normal=max(OKBradfordInverse(whiteCone*gains).y,1e-5);
        cone*=gains/normal;
    }
    if(temperatureEnabled!=0 || warmth!=0 || tint!=0) rgb=OKXyzToRgb(OKBradfordInverse(cone));
    rgb*=float3(gain0,gain1,gain2);
    return mul(float3x3(matrix00,matrix01,matrix02,matrix10,matrix11,matrix12,matrix20,matrix21,matrix22),rgb);
}
#endif
